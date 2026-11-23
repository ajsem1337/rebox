/* Console-format shadow heaps (docs/anyps5.md).
 *
 * AnyPS5 reads descriptors as the console wrote them (RDNA2 T#/V#/S#), while
 * vkd3d-proton's heaps hold the host driver's own format. So every CBV/SRV/UAV
 * and sampler heap gets a shadow: the same number of descriptors at the same
 * stride (the increment the title gets from GetDescriptorHandleIncrementSize),
 * filled in console format (console_desc.c) whenever the title writes a
 * descriptor through the API (Create*View, CreateSampler, CopyDescriptors*).
 * Bytes a title writes into heap memory itself are not seen.
 *
 * Shader-visible heaps also get a title address: a 32-bit address of our
 * choosing (from 0x10000000, 64 KiB aligned) that descriptor-table pointers
 * are built from, and that ReboxShadowRead resolves for the recompiler.
 * Heap slot 10 reports it (proxy.c), and vkd3d-proton packs console SGPRs
 * with it. REBOX_HEAP_VA=real or virtual turns title addresses off; the
 * shadows are still kept.
 * Their shadows live in GPU-visible memory (ReboxGpuAlloc) so shaders can
 * later read them directly; other heaps' shadows are plain memory.
 *
 * AnyPS5 returns each binding by value (the descriptor words it resolved).
 * ReboxShadowFind maps those words back to the vkd3d-proton descriptor
 * that was written alongside them, through a hash of every shadow slot. */

#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS
#include <windows.h>
#include <d3d12.h>
#include <stdlib.h>
#include <string.h>

#include "../common/rebox_log.h"
#include "console_desc.h"
#include "gpu_memory.h"
#include "shadow.h"

#define TITLE_BASE 0x10000000ull
#define TITLE_ALIGN 0x10000ull
#define BUCKETS 4096

struct shadow_heap
{
    ID3D12DescriptorHeap *inner;   /* not referenced: only compared */
    D3D12_DESCRIPTOR_HEAP_TYPE type;
    BOOL shader_visible, gpu_memory;
    SIZE_T cpu_start;              /* vkd3d-proton's CPU handle for descriptor 0 */
    UINT stride, count;
    BYTE *data;                    /* count * stride bytes, console format */
    BYTE *views;                   /* count bytes: enum shadow_view of each slot */
    struct uav_texture *uavs;      /* count entries, made on the first texture UAV write */
    UINT64 title_va, gpu_va;       /* title_va 0 for heaps that aren't shader-visible */
    UINT reserved, reserved_used;  /* hidden slots after count in vkd3d-proton's heap */
    UINT32 (*reserved_words)[4];   /* the S# each used one holds */
    struct shadow_heap *next;
};

/* A texture UAV's resource and view, so a sampled view of it can be made (AnyPS5 binds a
 * storage image as a sampled image too, for size queries). The resource isn't referenced: a slot
 * is only used while the title keeps its resource alive. */
struct uav_texture
{
    ID3D12Resource *res;
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc;
    BOOL has_desc;
};

struct entry
{
    UINT32 key[8];
    struct shadow_heap *heap;
    UINT index;
    struct entry *next;
};

static SRWLOCK lock = SRWLOCK_INIT;
static struct shadow_heap *heaps;
static struct entry *buckets[BUCKETS];
static UINT64 next_title_va = TITLE_BASE;

/* REBOX_HEAP_VA: title (default), real or virtual; REBOX_REAL_HEAP_VA=0
 * is the older spelling of virtual. */
static BOOL CALLBACK read_heap_va_mode(INIT_ONCE *once, void *param, void **ctx)
{
    char *mode = rebox_getenv("REBOX_HEAP_VA"), *legacy = rebox_getenv("REBOX_REAL_HEAP_VA");
    enum shadow_heap_va *out = param;

    *out = SHADOW_HEAP_VA_TITLE;
    if (legacy && legacy[0] == '0')
        *out = SHADOW_HEAP_VA_VIRTUAL;
    if (mode && !strcmp(mode, "real"))
        *out = SHADOW_HEAP_VA_REAL;
    else if (mode && !strcmp(mode, "virtual"))
        *out = SHADOW_HEAP_VA_VIRTUAL;
    else if (mode && !strcmp(mode, "title"))
        *out = SHADOW_HEAP_VA_TITLE;
    free(mode);
    free(legacy);
    return TRUE;
}

enum shadow_heap_va shadow_heap_va_mode(void)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    static enum shadow_heap_va mode;

    InitOnceExecuteOnce(&once, read_heap_va_mode, &mode, NULL);
    return mode;
}

BOOL shadow_title(SIZE_T cpu_start, UINT64 *title_va, UINT64 *size)
{
    BOOL found = FALSE;

    AcquireSRWLockShared(&lock);
    for (struct shadow_heap *h = heaps; h && !found; h = h->next)
        if (h->cpu_start == cpu_start && h->title_va)
        {
            *title_va = h->title_va;
            *size = (UINT64)h->count * h->stride;
            found = TRUE;
        }
    ReleaseSRWLockShared(&lock);
    return found;
}

static UINT desc_size(const struct shadow_heap *h)
{
    return h->type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ? 16 : 32;
}

/* The key for a slot: its first 8 dwords; samplers (4 dwords) are padded with
 * 0xffffffff so an S# never matches a V# with zero padding. */
static void make_key(const UINT32 *words, UINT word_count, BOOL sampler, UINT32 key[8])
{
    memset(key, sampler ? 0xff : 0, 8 * sizeof(*key));
    memcpy(key, words, (word_count < 8 ? word_count : 8) * sizeof(*key));
}

static UINT hash_key(const UINT32 key[8])
{
    UINT32 h = 2166136261u;
    for (int i = 0; i < 8; i++)
        h = (h ^ key[i]) * 16777619u;
    return h % BUCKETS;
}

static void slot_key(const struct shadow_heap *h, UINT index, UINT32 key[8])
{
    make_key((const UINT32 *)(h->data + (SIZE_T)index * h->stride), desc_size(h) / 4,
             h->type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, key);
}

static BOOL key_is_null(const UINT32 key[8])
{
    for (int i = 0; i < 8; i++)
        if (key[i] && key[i] != 0xffffffffu)
            return FALSE;
    return TRUE;
}

/* Caller holds the lock exclusively. */
static void unindex(struct shadow_heap *h, UINT index)
{
    UINT32 key[8];
    struct entry **p;

    slot_key(h, index, key);
    for (p = &buckets[hash_key(key)]; *p; p = &(*p)->next)
        if ((*p)->heap == h && (*p)->index == index)
        {
            struct entry *e = *p;
            *p = e->next;
            free(e);
            return;
        }
}

static void index_slot(struct shadow_heap *h, UINT index)
{
    struct entry *e;
    UINT b;

    if (!(e = malloc(sizeof(*e))))
        return;
    slot_key(h, index, e->key);
    if (key_is_null(e->key))
    {
        free(e);
        return;
    }
    e->heap = h;
    e->index = index;
    b = hash_key(e->key);
    e->next = buckets[b];
    buckets[b] = e;
}

static void heap_free(struct shadow_heap *h)
{
    for (UINT i = 0; i < h->count; i++)
        unindex(h, i);
    if (h->gpu_memory)
        ReboxGpuFree(h->data);
    else
        free(h->data);
    free(h->reserved_words);
    free(h->views);
    free(h->uavs);
    free(h);
}

/* Caller holds the lock. */
static struct shadow_heap *heap_for_cpu(SIZE_T cpu, UINT *index)
{
    for (struct shadow_heap *h = heaps; h; h = h->next)
        if (cpu >= h->cpu_start && cpu - h->cpu_start < (SIZE_T)h->count * h->stride)
        {
            *index = (UINT)((cpu - h->cpu_start) / h->stride);
            return h;
        }
    return NULL;
}

void shadow_heap_created(ID3D12DescriptorHeap *inner, const D3D12_DESCRIPTOR_HEAP_DESC *desc, UINT increment,
        UINT reserved)
{
    struct shadow_heap *heap, **p;
    SIZE_T size;

    if (!desc || (desc->Type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV && desc->Type != D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER)
            || !desc->NumDescriptors || !(heap = calloc(1, sizeof(*heap))))
        return;
    heap->inner = inner;
    heap->type = desc->Type;
    heap->shader_visible = !!(desc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
    heap->cpu_start = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(inner).ptr;
    heap->count = desc->NumDescriptors;
    heap->stride = increment;
    heap->reserved = reserved;
    if (increment < desc_size(heap))
    {
        static LONG logged;
        if (!InterlockedExchange(&logged, 1))
            rebox_log("d3d12_xs: descriptor increment %u is too small for console descriptors (%u bytes); "
                         "no shadow heaps", increment, desc_size(heap));
        free(heap);
        return;
    }
    size = (SIZE_T)heap->count * heap->stride;
    if (heap->shader_visible && SUCCEEDED(ReboxGpuAlloc(size, 256, (void **)&heap->data, &heap->gpu_va)))
    {
        heap->gpu_memory = TRUE;
        memset(heap->data, 0, size);
    }
    else if (!(heap->data = calloc(1, size)))
        ;
    if (heap->data && !(heap->views = calloc(1, heap->count)))
    {
        if (heap->gpu_memory)
            ReboxGpuFree(heap->data);
        else
            free(heap->data);
        heap->data = NULL;
    }
    if (!heap->data)
    {
        rebox_log("d3d12_xs: no memory for a %u-descriptor shadow heap", heap->count);
        free(heap);
        return;
    }

    AcquireSRWLockExclusive(&lock);
    /* A destroyed heap's object or CPU range can come back: forget it. */
    for (p = &heaps; *p;)
    {
        struct shadow_heap *h = *p;
        if (h->inner == inner || (h->cpu_start < heap->cpu_start + size
                                  && heap->cpu_start < h->cpu_start + (SIZE_T)h->count * h->stride))
        {
            *p = h->next;
            heap_free(h);
        }
        else
            p = &h->next;
    }
    if (heap->shader_visible && shadow_heap_va_mode() == SHADOW_HEAP_VA_TITLE)
    {
        heap->title_va = next_title_va;
        next_title_va = (next_title_va + size + TITLE_ALIGN - 1) & ~(TITLE_ALIGN - 1);
        if (next_title_va > 0xffffffffull)
        {
            rebox_log("d3d12_xs: shadow heap title addresses passed 4 GiB; wrapping");
            next_title_va = TITLE_BASE;
        }
    }
    heap->next = heaps;
    heaps = heap;
    ReleaseSRWLockExclusive(&lock);
}

void shadow_heap_destroyed(ID3D12DescriptorHeap *inner)
{
    AcquireSRWLockExclusive(&lock);
    for (struct shadow_heap **p = &heaps; *p; p = &(*p)->next)
        if ((*p)->inner == inner)
        {
            struct shadow_heap *h = *p;
            *p = h->next;
            heap_free(h);
            break;
        }
    ReleaseSRWLockExclusive(&lock);
}

/* What kind of view a write made: is_buffer -1 means "from the resource" (no view desc). */
static enum shadow_view view_kind(ID3D12Resource *res, int is_buffer, DXGI_FORMAT format, BOOL raw, BOOL uav)
{
    if (!res)
        return SHADOW_VIEW_NONE;
    if (is_buffer < 0)
    {
        D3D12_RESOURCE_DESC rd = ID3D12Resource_GetDesc(res);
        is_buffer = rd.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER;
        raw = is_buffer;
    }
    if (!is_buffer)
        return uav ? SHADOW_VIEW_UAV_TEXTURE : SHADOW_VIEW_SRV_TEXTURE;
    if (raw || format == DXGI_FORMAT_UNKNOWN)
        return uav ? SHADOW_VIEW_UAV_RAW : SHADOW_VIEW_SRV_RAW;
    return uav ? SHADOW_VIEW_UAV_TYPED : SHADOW_VIEW_SRV_TYPED;
}

/* GPU address ranges of buffers the title made views of, for AnyPS5's BDA page table: memory a
 * shader may reach by address. Kept after the views go: a buffer's range only matters while some
 * descriptor or pointer still names it. */
static SRWLOCK ranges_lock = SRWLOCK_INIT;
static struct { UINT64 begin, end; } *buffer_ranges;
static UINT buffer_range_count, buffer_range_capacity;

static volatile LONG ranges_generation;

LONG shadow_buffer_ranges_generation(void)
{
    return ranges_generation;
}

static void note_buffer_range(UINT64 va, UINT64 size)
{
    if (!va || !size)
        return;
    AcquireSRWLockExclusive(&ranges_lock);
    for (UINT i = 0; i < buffer_range_count; i++)
        if (buffer_ranges[i].begin == va)
        {
            if (buffer_ranges[i].end < va + size)
            {
                buffer_ranges[i].end = va + size;
                InterlockedIncrement(&ranges_generation);
            }
            ReleaseSRWLockExclusive(&ranges_lock);
            return;
        }
    if (buffer_range_count == buffer_range_capacity)
    {
        UINT capacity = buffer_range_capacity ? buffer_range_capacity * 2 : 256;
        void *grown = realloc(buffer_ranges, capacity * sizeof(*buffer_ranges));
        if (!grown)
        {
            ReleaseSRWLockExclusive(&ranges_lock);
            return;
        }
        buffer_ranges = grown;
        buffer_range_capacity = capacity;
    }
    buffer_ranges[buffer_range_count].begin = va;
    buffer_ranges[buffer_range_count++].end = va + size;
    InterlockedIncrement(&ranges_generation);
    ReleaseSRWLockExclusive(&ranges_lock);
}

static void note_buffer_view(ID3D12Resource *res)
{
    D3D12_RESOURCE_DESC rd;
    if (!res)
        return;
    rd = ID3D12Resource_GetDesc(res);
    if (rd.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
        note_buffer_range(ID3D12Resource_GetGPUVirtualAddress(res), rd.Width);
}

UINT shadow_buffer_ranges(UINT64 (*out)[2], UINT max)
{
    UINT n;

    AcquireSRWLockShared(&ranges_lock);
    n = min(buffer_range_count, max);
    for (UINT i = 0; i < n; i++)
    {
        out[i][0] = buffer_ranges[i].begin;
        out[i][1] = buffer_ranges[i].end;
    }
    ReleaseSRWLockShared(&ranges_lock);
    return n;
}

/* The first writes into shader-visible heaps, by title address: do the slots the console tables
 * point at get written, and before the draws that read them? */
static void note_visible_write(const struct shadow_heap *h, UINT index, const char *how)
{
    static LONG shown;
    if (h->title_va && InterlockedIncrement(&shown) <= 64)
        rebox_log("d3d12_xs: shadow write (%s) at %#llx: view %u, words %08x %08x %08x %08x", how,
                     (unsigned long long)(h->title_va + (UINT64)index * h->stride), h->views[index],
                     ((const UINT32 *)(h->data + (SIZE_T)index * h->stride))[0],
                     ((const UINT32 *)(h->data + (SIZE_T)index * h->stride))[1],
                     ((const UINT32 *)(h->data + (SIZE_T)index * h->stride))[2],
                     ((const UINT32 *)(h->data + (SIZE_T)index * h->stride))[3]);
}

static void write_slot(SIZE_T cpu, const UINT32 *words, UINT size, enum shadow_view view)
{
    struct shadow_heap *h;
    UINT index;

    AcquireSRWLockExclusive(&lock);
    if ((h = heap_for_cpu(cpu, &index)))
    {
        unindex(h, index);
        memset(h->data + (SIZE_T)index * h->stride, 0, h->stride);
        memcpy(h->data + (SIZE_T)index * h->stride, words, size < h->stride ? size : h->stride);
        h->views[index] = view;
        index_slot(h, index);
        note_visible_write(h, index, "view");
    }
    ReleaseSRWLockExclusive(&lock);
}

void shadow_write_cbv(SIZE_T cpu, const D3D12_CONSTANT_BUFFER_VIEW_DESC *desc)
{
    UINT32 w[8];
    if (desc)
        note_buffer_range(desc->BufferLocation, desc->SizeInBytes);
    console_encode_cbv(desc, w);
    write_slot(cpu, w, sizeof(w), desc && desc->BufferLocation ? SHADOW_VIEW_CBV : SHADOW_VIEW_NONE);
}

void shadow_write_srv(SIZE_T cpu, ID3D12Resource *res, const D3D12_SHADER_RESOURCE_VIEW_DESC *desc)
{
    UINT32 w[8];
    note_buffer_view(res);
    console_encode_srv(res, desc, w);
    write_slot(cpu, w, sizeof(w), view_kind(res, desc ? desc->ViewDimension == D3D12_SRV_DIMENSION_BUFFER : -1,
               desc ? desc->Format : DXGI_FORMAT_UNKNOWN, desc && desc->ViewDimension == D3D12_SRV_DIMENSION_BUFFER
               ? (desc->Buffer.StructureByteStride || (desc->Buffer.Flags & D3D12_BUFFER_SRV_FLAG_RAW)) : 0, FALSE));
}

void shadow_write_uav(SIZE_T cpu, ID3D12Resource *res, const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc)
{
    struct shadow_heap *h;
    UINT32 w[8];
    UINT index;

    AcquireSRWLockExclusive(&lock);
    if ((h = heap_for_cpu(cpu, &index)) && h->shader_visible && res
            && (h->uavs || (h->uavs = calloc(h->count, sizeof(*h->uavs)))))
    {
        h->uavs[index].res = res;
        h->uavs[index].has_desc = desc != NULL;
        if (desc)
            h->uavs[index].desc = *desc;
    }
    ReleaseSRWLockExclusive(&lock);
    note_buffer_view(res);
    console_encode_uav(res, desc, w);
    write_slot(cpu, w, sizeof(w), view_kind(res, desc ? desc->ViewDimension == D3D12_UAV_DIMENSION_BUFFER : -1,
               desc ? desc->Format : DXGI_FORMAT_UNKNOWN, desc && desc->ViewDimension == D3D12_UAV_DIMENSION_BUFFER
               ? (desc->Buffer.StructureByteStride || (desc->Buffer.Flags & D3D12_BUFFER_UAV_FLAG_RAW)) : 0, TRUE));
}

void shadow_write_sampler(SIZE_T cpu, const D3D12_SAMPLER_DESC *desc)
{
    UINT32 w[4];
    if (!desc)
        return;
    console_encode_sampler(desc, w);
    write_slot(cpu, w, sizeof(w), SHADOW_VIEW_SAMPLER);
}

void shadow_copy(SIZE_T dst, SIZE_T src, UINT count)
{
    AcquireSRWLockExclusive(&lock);
    for (UINT i = 0; i < count; i++)
    {
        struct shadow_heap *d, *s;
        UINT di, si;

        if (!(d = heap_for_cpu(dst, &di)))
            break;
        dst += d->stride;
        s = heap_for_cpu(src, &si);
        src += s ? s->stride : d->stride;
        unindex(d, di);
        if (s && s->stride == d->stride)
        {
            memcpy(d->data + (SIZE_T)di * d->stride, s->data + (SIZE_T)si * s->stride, d->stride);
            d->views[di] = s->views[si];
        }
        else
        {
            memset(d->data + (SIZE_T)di * d->stride, 0, d->stride);
            d->views[di] = SHADOW_VIEW_NONE;
        }
        index_slot(d, di);
        note_visible_write(d, di, "copy");
    }
    ReleaseSRWLockExclusive(&lock);
}

void shadow_copy_ranges(UINT dst_count, const D3D12_CPU_DESCRIPTOR_HANDLE *dst_starts, const UINT *dst_sizes,
        UINT src_count, const D3D12_CPU_DESCRIPTOR_HANDLE *src_starts, const UINT *src_sizes)
{
    UINT di = 0, si = 0, d_off = 0, s_off = 0;

    /* Walk both range lists one run at a time, as CopyDescriptors does. */
    while (di < dst_count && si < src_count)
    {
        UINT d_size = dst_sizes ? dst_sizes[di] : 1, s_size = src_sizes ? src_sizes[si] : 1;
        UINT n = min(d_size - d_off, s_size - s_off);
        struct shadow_heap *dh, *sh;
        UINT unused;

        AcquireSRWLockShared(&lock);
        dh = heap_for_cpu(dst_starts[di].ptr, &unused);
        sh = heap_for_cpu(src_starts[si].ptr, &unused);
        ReleaseSRWLockShared(&lock);
        if (dh && n)
            shadow_copy(dst_starts[di].ptr + (SIZE_T)d_off * dh->stride,
                        src_starts[si].ptr + (SIZE_T)s_off * (sh ? sh->stride : dh->stride), n);
        if ((d_off += n) == d_size) { di++; d_off = 0; }
        if ((s_off += n) == s_size) { si++; s_off = 0; }
    }
}

/* ------------------------------------------------- for console_compile.c */

UINT shadow_regions(struct shadow_region *out, UINT max)
{
    UINT n = 0;

    AcquireSRWLockShared(&lock);
    for (struct shadow_heap *h = heaps; h && n < max; h = h->next)
        if (h->title_va)
        {
            out[n].title_va = h->title_va;
            out[n].gpu_va = h->gpu_memory ? h->gpu_va : 0;
            out[n].data = h->data;
            out[n].size = (SIZE_T)h->count * h->stride;
            n++;
        }
    ReleaseSRWLockShared(&lock);
    return n;
}

/* Fields AnyPS5 may rewrite in the words it binds: a T#'s DST_SEL (word 3 bits 0-11, made
 * identity when the view swizzle takes it over), an S#'s filters (word 2 bits 20-27, for the
 * point-filtered twin of each sampler) and a V#'s word 3 (format and DST_SEL, zeroed for buffers
 * it reads raw, such as constant buffers read with s_buffer_load; base, stride and size in words
 * 0-2 still identify the view). */
static BOOL loose_match(const UINT32 *slot, const UINT32 *words, UINT n, BOOL sampler)
{
    for (UINT i = 0; i < n; i++)
    {
        UINT32 mask = ~0u;
        if (sampler && i == 2)
            mask = ~(0xffu << 20);
        else if (!sampler && n == 8 && i == 3)
            mask = ~0xfffu;
        else if (!sampler && n == 4 && i == 3)
            mask = 0;
        if ((slot[i] ^ words[i]) & mask)
            return FALSE;
    }
    return TRUE;
}

BOOL shadow_find_index(SIZE_T heap_cpu, const UINT32 *words, UINT n, BOOL sampler, BOOL loose, UINT *index)
{
    struct shadow_heap *heap = NULL;
    UINT32 key[8];
    BOOL found = FALSE;

    if (!words || !n || n > 8)
        return FALSE;
    make_key(words, n, sampler, key);
    AcquireSRWLockShared(&lock);
    for (struct entry *e = buckets[hash_key(key)]; e && !found; e = e->next)
        if (e->heap->cpu_start == heap_cpu && !memcmp(e->key, key, sizeof(key)))
        {
            *index = e->index;
            found = TRUE;
        }
    for (struct shadow_heap *h = heaps; h && !heap; h = h->next)
        if (h->cpu_start == heap_cpu)
            heap = h;
    for (UINT i = 0; loose && heap && !found && i < heap->count; i++)
        if (loose_match((const UINT32 *)(heap->data + (SIZE_T)i * heap->stride), words, n, sampler))
        {
            *index = i;
            found = TRUE;
        }
    ReleaseSRWLockShared(&lock);
    return found;
}

enum shadow_view shadow_slot_view(SIZE_T heap_cpu, UINT index)
{
    enum shadow_view view = SHADOW_VIEW_NONE;

    AcquireSRWLockShared(&lock);
    for (struct shadow_heap *h = heaps; h; h = h->next)
        if (h->cpu_start == heap_cpu)
        {
            if (index < h->count)
                view = h->views[index];
            else if (index < h->count + h->reserved_used)
                view = h->type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ? SHADOW_VIEW_SAMPLER : SHADOW_VIEW_SRV_TEXTURE;
            break;
        }
    ReleaseSRWLockShared(&lock);
    return view;
}

UINT shadow_reserved(SIZE_T cpu_start)
{
    UINT reserved = 0;

    AcquireSRWLockShared(&lock);
    for (struct shadow_heap *h = heaps; h; h = h->next)
        if (h->cpu_start == cpu_start)
            reserved = h->reserved;
    ReleaseSRWLockShared(&lock);
    return reserved;
}

/* An SRV equivalent to a texture UAV's view. */
static void srv_from_uav(const struct uav_texture *u, D3D12_SHADER_RESOURCE_VIEW_DESC *srv)
{
    D3D12_RESOURCE_DESC rd = ID3D12Resource_GetDesc(u->res);
    const D3D12_UNORDERED_ACCESS_VIEW_DESC *d = &u->desc;
    D3D12_UAV_DIMENSION dim = u->has_desc ? d->ViewDimension
            : rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ? (rd.DepthOrArraySize > 1 ? D3D12_UAV_DIMENSION_TEXTURE1DARRAY : D3D12_UAV_DIMENSION_TEXTURE1D)
            : rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? D3D12_UAV_DIMENSION_TEXTURE3D
            : rd.DepthOrArraySize > 1 ? D3D12_UAV_DIMENSION_TEXTURE2DARRAY : D3D12_UAV_DIMENSION_TEXTURE2D;

    memset(srv, 0, sizeof(*srv));
    srv->Format = u->has_desc && d->Format != DXGI_FORMAT_UNKNOWN ? d->Format : rd.Format;
    srv->Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    switch (dim)
    {
    case D3D12_UAV_DIMENSION_TEXTURE1D:
        srv->ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
        srv->Texture1D.MostDetailedMip = u->has_desc ? d->Texture1D.MipSlice : 0;
        srv->Texture1D.MipLevels = 1;
        break;
    case D3D12_UAV_DIMENSION_TEXTURE1DARRAY:
        srv->ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
        srv->Texture1DArray.MostDetailedMip = u->has_desc ? d->Texture1DArray.MipSlice : 0;
        srv->Texture1DArray.MipLevels = 1;
        srv->Texture1DArray.FirstArraySlice = u->has_desc ? d->Texture1DArray.FirstArraySlice : 0;
        srv->Texture1DArray.ArraySize = u->has_desc ? d->Texture1DArray.ArraySize : rd.DepthOrArraySize;
        break;
    case D3D12_UAV_DIMENSION_TEXTURE2DARRAY:
        srv->ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srv->Texture2DArray.MostDetailedMip = u->has_desc ? d->Texture2DArray.MipSlice : 0;
        srv->Texture2DArray.MipLevels = 1;
        srv->Texture2DArray.FirstArraySlice = u->has_desc ? d->Texture2DArray.FirstArraySlice : 0;
        srv->Texture2DArray.ArraySize = u->has_desc ? d->Texture2DArray.ArraySize : rd.DepthOrArraySize;
        break;
    case D3D12_UAV_DIMENSION_TEXTURE3D:
        srv->ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        srv->Texture3D.MostDetailedMip = u->has_desc ? d->Texture3D.MipSlice : 0;
        srv->Texture3D.MipLevels = 1;
        break;
    default:
        srv->ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv->Texture2D.MostDetailedMip = u->has_desc ? d->Texture2D.MipSlice : 0;
        srv->Texture2D.MipLevels = 1;
        break;
    }
}

BOOL shadow_reserve_srv_for_uav(SIZE_T heap_cpu, UINT uav_index, UINT *index)
{
    ID3D12Device *device = rebox_shared_device();
    struct shadow_heap *heap = NULL;
    BOOL found = FALSE;
    UINT32 key[4];

    AcquireSRWLockExclusive(&lock);
    for (struct shadow_heap *h = heaps; h && !heap; h = h->next)
        if (h->cpu_start == heap_cpu && h->type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV && h->reserved)
            heap = h;
    if (heap && uav_index < heap->count && heap->uavs && heap->uavs[uav_index].res
            && heap->views[uav_index] == SHADOW_VIEW_UAV_TEXTURE)
    {
        const struct uav_texture *u = &heap->uavs[uav_index];
        /* Keyed by the UAV's resource and view, in the slot's first words. */
        UINT64 r = (UINT64)(ULONG_PTR)u->res;
        key[0] = (UINT32)r;
        key[1] = (UINT32)(r >> 32);
        key[2] = u->has_desc ? u->desc.Format : 0;
        key[3] = u->has_desc ? (UINT32)u->desc.ViewDimension | u->desc.Texture2DArray.MipSlice << 8
                 | u->desc.Texture2DArray.FirstArraySlice << 16 : ~0u;
        for (UINT i = 0; i < heap->reserved_used && !found; i++)
            if (!memcmp(heap->reserved_words[i], key, 16))
            {
                *index = heap->count + i;
                found = TRUE;
            }
        if (!found && device && heap->reserved_used < heap->reserved
                && (heap->reserved_words || (heap->reserved_words = calloc(heap->reserved, 16))))
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv;
            D3D12_CPU_DESCRIPTOR_HANDLE cpu;
            UINT i = heap->reserved_used++;

            srv_from_uav(u, &srv);
            memcpy(heap->reserved_words[i], key, 16);
            *index = heap->count + i;
            cpu.ptr = heap->cpu_start + (SIZE_T)*index * heap->stride;
            ID3D12Device_CreateShaderResourceView(device, u->res, &srv, cpu);
            found = TRUE;
        }
    }
    ReleaseSRWLockExclusive(&lock);
    return found;
}

/* A view of a 1x1 zeroed dummy texture (SRV, or UAV for a storage image) in the heap's reserved
 * tail, made once: what a console shader gets for a descriptor the title left empty or wrote
 * outside our heaps (a UE5 title's T# with base 0x9200, a dummy), which reads zero there as a null
 * descriptor does. A real texture, not a null view: lavapipe crashed on a null storage image. */
static ID3D12Resource *dummy_texture(ID3D12Device *device)
{
    static ID3D12Resource *texture;
    D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC desc = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 1, 1, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM,
                                 { 1, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS };

    if (!texture)
        ID3D12Device_CreateCommittedResource(device, &heap, D3D12_HEAP_FLAG_NONE, &desc,
                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL, &IID_ID3D12Resource,
                                             (void **)&texture);
    return texture;
}
BOOL shadow_reserve_null(SIZE_T heap_cpu, BOOL storage, UINT *index)
{
    ID3D12Device *device = rebox_shared_device();
    struct shadow_heap *heap = NULL;
    const UINT32 key[4] = { 0x4c4c554e /* NULL */, storage, 0, 0 };
    BOOL found = FALSE;

    AcquireSRWLockExclusive(&lock);
    for (struct shadow_heap *h = heaps; h && !heap; h = h->next)
        if (h->cpu_start == heap_cpu && h->type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV && h->reserved)
            heap = h;
    for (UINT i = 0; heap && heap->reserved_words && i < heap->reserved_used && !found; i++)
        if (!memcmp(heap->reserved_words[i], key, 16))
        {
            *index = heap->count + i;
            found = TRUE;
        }
    if (heap && !found && device && dummy_texture(device) && heap->reserved_used < heap->reserved
            && (heap->reserved_words || (heap->reserved_words = calloc(heap->reserved, 16))))
    {
        ID3D12Resource *texture = dummy_texture(device);
        D3D12_CPU_DESCRIPTOR_HANDLE cpu;
        UINT i = heap->reserved_used++;

        memcpy(heap->reserved_words[i], key, 16);
        *index = heap->count + i;
        cpu.ptr = heap->cpu_start + (SIZE_T)*index * heap->stride;
        if (storage)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav = { DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_UAV_DIMENSION_TEXTURE2D };
            ID3D12Device_CreateUnorderedAccessView(device, texture, NULL, &uav, cpu);
        }
        else
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv = { DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_SRV_DIMENSION_TEXTURE2D,
                                                    D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING };
            srv.Texture2D.MipLevels = 1;
            ID3D12Device_CreateShaderResourceView(device, texture, &srv, cpu);
        }
        found = TRUE;
    }
    ReleaseSRWLockExclusive(&lock);
    return found;
}

BOOL shadow_reserve_sampler(SIZE_T heap_cpu, const UINT32 words[4], UINT *index)
{
    ID3D12Device *device = rebox_shared_device();
    struct shadow_heap *heap = NULL;
    BOOL found = FALSE;

    AcquireSRWLockExclusive(&lock);
    for (struct shadow_heap *h = heaps; h && !heap; h = h->next)
        if (h->cpu_start == heap_cpu && h->type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER && h->reserved)
            heap = h;
    for (UINT i = 0; heap && i < heap->reserved_used && !found; i++)
        if (!memcmp(heap->reserved_words[i], words, 16))
        {
            *index = heap->count + i;
            found = TRUE;
        }
    if (heap && !found && device && heap->reserved_used < heap->reserved
            && (heap->reserved_words || (heap->reserved_words = calloc(heap->reserved, 16))))
    {
        D3D12_SAMPLER_DESC desc;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu;
        UINT i = heap->reserved_used++;

        console_decode_sampler(words, &desc);
        memcpy(heap->reserved_words[i], words, 16);
        *index = heap->count + i;
        cpu.ptr = heap->cpu_start + (SIZE_T)*index * heap->stride;
        ID3D12Device_CreateSampler(device, &desc, cpu);
        found = TRUE;
    }
    ReleaseSRWLockExclusive(&lock);
    return found;
}

/* ------------------------------------------------------------- exports */

/* The shadow of the heap whose descriptor 0 has vkd3d-proton CPU handle
 * cpu_start: its title address (0 if not shader-visible), contents, stride
 * and size. */
HRESULT WINAPI ReboxShadowHeap(SIZE_T cpu_start, UINT64 *title_va, void **data, UINT *stride, UINT *count)
{
    HRESULT hr = E_INVALIDARG;

    AcquireSRWLockShared(&lock);
    for (struct shadow_heap *h = heaps; h; h = h->next)
        if (h->cpu_start == cpu_start)
        {
            if (title_va) *title_va = h->title_va;
            if (data) *data = h->data;
            if (stride) *stride = h->stride;
            if (count) *count = h->count;
            hr = S_OK;
            break;
        }
    ReleaseSRWLockShared(&lock);
    return hr;
}

/* Copies size bytes at a title address out of the shadow heaps. Fails unless
 * the whole range lies in one heap. */
BOOL WINAPI ReboxShadowRead(UINT64 address, void *out, SIZE_T size)
{
    BOOL ok = FALSE;

    AcquireSRWLockShared(&lock);
    for (struct shadow_heap *h = heaps; h; h = h->next)
    {
        UINT64 end = h->title_va + (UINT64)h->count * h->stride;
        if (h->title_va && address >= h->title_va && address < end && size <= end - address)
        {
            memcpy(out, h->data + (address - h->title_va), size);
            ok = TRUE;
            break;
        }
    }
    ReleaseSRWLockShared(&lock);
    return ok;
}

/* Finds the descriptor whose console words are these (4 or 8 dwords; sampler
 * for S#) and returns vkd3d-proton's CPU handle for it, preferring
 * shader-visible heaps. Several slots holding the same words hold the same
 * view, so any of them will do. */
BOOL WINAPI ReboxShadowFind(const UINT32 *words, UINT word_count, BOOL sampler, SIZE_T *cpu)
{
    struct entry *best = NULL;
    UINT32 key[8];

    if (!words || !word_count)
        return FALSE;
    make_key(words, word_count, sampler, key);
    AcquireSRWLockShared(&lock);
    for (struct entry *e = buckets[hash_key(key)]; e; e = e->next)
        if (!memcmp(e->key, key, sizeof(key)) && (sampler == (e->heap->type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER))
                && (!best || (e->heap->shader_visible && !best->heap->shader_visible)))
            best = e;
    if (best && cpu)
        *cpu = best->heap->cpu_start + (SIZE_T)best->index * best->heap->stride;
    ReleaseSRWLockShared(&lock);
    return best != NULL;
}
