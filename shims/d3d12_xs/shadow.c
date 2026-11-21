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
    UINT64 title_va, gpu_va;       /* title_va 0 for heaps that aren't shader-visible */
    struct shadow_heap *next;
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

void shadow_heap_created(ID3D12DescriptorHeap *inner, const D3D12_DESCRIPTOR_HEAP_DESC *desc, UINT increment)
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

static void write_slot(SIZE_T cpu, const UINT32 *words, UINT size)
{
    struct shadow_heap *h;
    UINT index;

    AcquireSRWLockExclusive(&lock);
    if ((h = heap_for_cpu(cpu, &index)))
    {
        unindex(h, index);
        memset(h->data + (SIZE_T)index * h->stride, 0, h->stride);
        memcpy(h->data + (SIZE_T)index * h->stride, words, size < h->stride ? size : h->stride);
        index_slot(h, index);
    }
    ReleaseSRWLockExclusive(&lock);
}

void shadow_write_cbv(SIZE_T cpu, const D3D12_CONSTANT_BUFFER_VIEW_DESC *desc)
{
    UINT32 w[8];
    console_encode_cbv(desc, w);
    write_slot(cpu, w, sizeof(w));
}

void shadow_write_srv(SIZE_T cpu, ID3D12Resource *res, const D3D12_SHADER_RESOURCE_VIEW_DESC *desc)
{
    UINT32 w[8];
    console_encode_srv(res, desc, w);
    write_slot(cpu, w, sizeof(w));
}

void shadow_write_uav(SIZE_T cpu, ID3D12Resource *res, const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc)
{
    UINT32 w[8];
    console_encode_uav(res, desc, w);
    write_slot(cpu, w, sizeof(w));
}

void shadow_write_sampler(SIZE_T cpu, const D3D12_SAMPLER_DESC *desc)
{
    UINT32 w[4];
    if (!desc)
        return;
    console_encode_sampler(desc, w);
    write_slot(cpu, w, sizeof(w));
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
            memcpy(d->data + (SIZE_T)di * d->stride, s->data + (SIZE_T)si * s->stride, d->stride);
        else
            memset(d->data + (SIZE_T)di * d->stride, 0, d->stride);
        index_slot(d, di);
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
 * identity when the view swizzle takes it over) and an S#'s filters (word 2 bits 20-27, for the
 * point-filtered twin of each sampler). */
static BOOL loose_match(const UINT32 *slot, const UINT32 *words, UINT n, BOOL sampler)
{
    for (UINT i = 0; i < n; i++)
    {
        UINT32 mask = ~0u;
        if (sampler && i == 2)
            mask = ~(0xffu << 20);
        else if (!sampler && n == 8 && i == 3)
            mask = ~0xfffu;
        if ((slot[i] ^ words[i]) & mask)
            return FALSE;
    }
    return TRUE;
}

BOOL shadow_find_index(SIZE_T heap_cpu, const UINT32 *words, UINT n, BOOL sampler, UINT *index)
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
    for (UINT i = 0; heap && !found && i < heap->count; i++)
        if (loose_match((const UINT32 *)(heap->data + (SIZE_T)i * heap->stride), words, n, sampler))
        {
            *index = i;
            found = TRUE;
        }
    ReleaseSRWLockShared(&lock);
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
