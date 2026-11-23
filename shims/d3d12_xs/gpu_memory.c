/* GPU-visible memory for xmem.dll and for command-list embedded data.
 *
 * Memory comes from CPU-visible custom heaps (WRITE_BACK, L0) on the shared
 * device, each spanned by one UAV-capable buffer, so vkd3d-proton knows the
 * whole range as a resource and resolves raw GPU VAs inside it. Small
 * requests are suballocated from 64 MiB chunks.
 *
 * Whether the returned CPU pointer equals its GPU VA depends on the Vulkan
 * driver and VK_LAYER_REBOX_gpuva_identity (docs/d3d12x.md). It is checked
 * once per chunk and logged. */

#define COBJMACROS
#include <windows.h>
#include <d3d12.h>
#include <stdlib.h>
#include <string.h>

#include "../common/rebox_log.h"
#include "../../layers/gpuva_mailbox.h"
#include "gpu_memory.h"

#define CHUNK_SIZE (64u << 20)
#define DEDICATED_THRESHOLD (16u << 20)
#define HEAP_ALIGN (64u << 10)

struct range { UINT64 start, end; };

struct chunk
{
    ID3D12Heap *heap;
    ID3D12Resource *buffer;
    BYTE *cpu;
    UINT64 gpu, size, used;
    BOOL dedicated, dirty; /* dirty: something was freed, so ranges may hold old data */
    BOOL adopted; /* title memory made GPU-visible in place (gpu_memory_adopt): never freed */
    struct range *free;
    size_t free_count, free_cap;
    struct chunk *next;
};

struct allocation
{
    BYTE *cpu;
    struct chunk *chunk;
    UINT64 offset, size;
    struct allocation *next;
};

#define BUCKETS 4096

static SRWLOCK lock = SRWLOCK_INIT;
static struct chunk *chunks;
static UINT64 bytes_in_use;
static struct allocation *table[BUCKETS];

static unsigned bucket(const void *p)
{
    ULONG_PTR h = (ULONG_PTR)p >> 6;
    return (unsigned)((h ^ (h >> 13)) % BUCKETS);
}

/* --------------------------------------------------------------- ranges */

/* Alignment applies to the CPU address (and so to the GPU VA when they are
 * equal), not just the offset: without the layer, mappings may only be
 * page-aligned. */
static BOOL range_alloc(struct chunk *c, UINT64 size, UINT64 align, UINT64 *out)
{
    const UINT64 base = (ULONG_PTR)c->cpu;

    for (size_t i = 0; i < c->free_count; i++)
    {
        struct range *r = &c->free[i];
        UINT64 start = ((base + r->start + align - 1) & ~(align - 1)) - base;

        if (start + size > r->end)
            continue;
        if (start > r->start && start + size < r->end)
        {
            if (c->free_count == c->free_cap)
                c->free = realloc(c->free, (c->free_cap *= 2) * sizeof(*c->free));
            r = &c->free[i];
            memmove(&c->free[i + 2], &c->free[i + 1], (c->free_count - i - 1) * sizeof(*c->free));
            c->free[i + 1] = (struct range){ start + size, r->end };
            r->end = start;
            c->free_count++;
        }
        else if (start > r->start)
            r->end = start;
        else if (start + size < r->end)
            r->start = start + size;
        else
        {
            memmove(&c->free[i], &c->free[i + 1], (c->free_count - i - 1) * sizeof(*c->free));
            c->free_count--;
        }
        *out = start;
        return TRUE;
    }
    return FALSE;
}

static void range_free(struct chunk *c, UINT64 start, UINT64 size)
{
    UINT64 end = start + size;
    size_t i;

    for (i = 0; i < c->free_count && c->free[i].start < start; i++)
        ;
    if (i > 0 && c->free[i - 1].end == start)
    {
        c->free[i - 1].end = end;
        if (i < c->free_count && c->free[i].start == end)
        {
            c->free[i - 1].end = c->free[i].end;
            memmove(&c->free[i], &c->free[i + 1], (c->free_count - i - 1) * sizeof(*c->free));
            c->free_count--;
        }
        return;
    }
    if (i < c->free_count && c->free[i].start == end)
    {
        c->free[i].start = start;
        return;
    }
    if (c->free_count == c->free_cap)
        c->free = realloc(c->free, (c->free_cap *= 2) * sizeof(*c->free));
    memmove(&c->free[i + 1], &c->free[i], (c->free_count - i) * sizeof(*c->free));
    c->free[i] = (struct range){ start, end };
    c->free_count++;
}

/* --------------------------------------------------------------- chunks */

static struct chunk *chunk_create(ID3D12Device *device, UINT64 size, BOOL dedicated, BOOL adopted)
{
    D3D12_HEAP_DESC hd = {
        .SizeInBytes = size,
        .Properties = { D3D12_HEAP_TYPE_CUSTOM, D3D12_CPU_PAGE_PROPERTY_WRITE_BACK, D3D12_MEMORY_POOL_L0, 0, 0 },
        .Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT,
        /* Adopted memory gets the title's bytes copied back; zeroing it first is wasted. */
        .Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS | (adopted ? D3D12_HEAP_FLAG_CREATE_NOT_ZEROED : 0),
    };
    D3D12_RESOURCE_DESC rd = {
        .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER, .Width = size, .Height = 1, .DepthOrArraySize = 1,
        .MipLevels = 1, .Format = DXGI_FORMAT_UNKNOWN, .SampleDesc = { 1, 0 },
        .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR, .Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
    };
    D3D12_RANGE none = { 0, 0 };
    struct chunk *c = calloc(1, sizeof(*c));
    static LONG reported;
    HRESULT hr;

    if (FAILED(hr = ID3D12Device_CreateHeap(device, &hd, &IID_ID3D12Heap, (void **)&c->heap)))
    {
        rebox_log("d3d12_xs: CreateHeap(%#llx) for GPU memory failed: %#lx", (unsigned long long)size, hr);
        goto fail;
    }
    if (FAILED(hr = ID3D12Device_CreatePlacedResource(device, c->heap, 0, &rd, D3D12_RESOURCE_STATE_COMMON,
                                                      NULL, &IID_ID3D12Resource, (void **)&c->buffer)))
    {
        rebox_log("d3d12_xs: CreatePlacedResource(%#llx) for GPU memory failed: %#lx", (unsigned long long)size, hr);
        goto fail;
    }
    if (FAILED(hr = ID3D12Resource_Map(c->buffer, 0, &none, (void **)&c->cpu)))
    {
        rebox_log("d3d12_xs: Map of GPU memory failed: %#lx", hr);
        goto fail;
    }
    c->gpu = ID3D12Resource_GetGPUVirtualAddress(c->buffer);
    c->size = size;
    c->dedicated = dedicated;
    c->free_cap = 8;
    c->free = calloc(c->free_cap, sizeof(*c->free));
    c->free[0] = (struct range){ 0, size };
    c->free_count = 1;

    if (adopted)
        return c;
    if ((UINT64)(ULONG_PTR)c->cpu != c->gpu)
    {
        if (!InterlockedExchange(&reported, 1))
            rebox_log("d3d12_xs: GPU memory is NOT identity-mapped (CPU %p, GPU VA %#llx): titles that use "
                         "GPU VAs as pointers will fault. Use .#radv-rebox and VK_LAYER_REBOX_gpuva_identity.",
                         c->cpu, (unsigned long long)c->gpu);
    }
    else if (!InterlockedExchange(&reported, 1))
        rebox_log("d3d12_xs: GPU memory is identity-mapped (first chunk at %p)", c->cpu);
    return c;

fail:
    if (c->buffer)
        ID3D12Resource_Release(c->buffer);
    if (c->heap)
        ID3D12Heap_Release(c->heap);
    free(c);
    return NULL;
}

static void chunk_destroy(struct chunk *c)
{
    ID3D12Resource_Unmap(c->buffer, 0, NULL);
    ID3D12Resource_Release(c->buffer);
    ID3D12Heap_Release(c->heap);
    free(c->free);
    free(c);
}

/* ---------------------------------------------------------------- API */

HRESULT WINAPI ReboxGpuAlloc(SIZE_T size, SIZE_T alignment, void **cpu, UINT64 *gpu)
{
    ID3D12Device *device = rebox_shared_device();
    struct allocation *a;
    struct chunk *c;
    UINT64 offset = 0;
    BOOL recycled;

    if (!cpu)
        return E_INVALIDARG;
    *cpu = NULL;
    if (!device)
        return E_FAIL;
    if (!size)
        size = 1;
    if (alignment < 256)
        alignment = 256; /* D3D12 constant-buffer alignment; cheap to always give */
    if (alignment & (alignment - 1))
        return E_INVALIDARG;

    AcquireSRWLockExclusive(&lock);
    if (size > DEDICATED_THRESHOLD || alignment > HEAP_ALIGN)
    {
        UINT64 chunk_size = (size + alignment + HEAP_ALIGN - 1) & ~(UINT64)(HEAP_ALIGN - 1);
        if ((c = chunk_create(device, chunk_size, TRUE, FALSE)) && range_alloc(c, size, alignment, &offset))
        {
            c->next = chunks;
            chunks = c;
        }
        else if (c)
        {
            chunk_destroy(c);
            c = NULL;
        }
    }
    else
    {
        for (c = chunks; c; c = c->next)
            if (!c->dedicated && range_alloc(c, size, alignment, &offset))
                break;
        if (!c && (c = chunk_create(device, CHUNK_SIZE, FALSE, FALSE)))
        {
            range_alloc(c, size, alignment, &offset);
            c->next = chunks;
            chunks = c;
        }
    }
    if (!c)
    {
        ReleaseSRWLockExclusive(&lock);
        return E_OUTOFMEMORY;
    }
    a = calloc(1, sizeof(*a));
    a->cpu = c->cpu + offset;
    recycled = c->dirty;
    a->chunk = c;
    a->offset = offset;
    a->size = size;
    a->next = table[bucket(a->cpu)];
    table[bucket(a->cpu)] = a;
    c->used += size;
    bytes_in_use += size;
    ReleaseSRWLockExclusive(&lock);

    /* New heaps are zero-filled; ranges reused within a chunk aren't. XMemAlloc
     * memory comes back zeroed, so clear them: anything from a chunk that has
     * had a free. */
    if (recycled)
        memset(a->cpu, 0, size);

    *cpu = a->cpu;
    if (gpu)
        *gpu = c->gpu + offset;
    return S_OK;
}

void WINAPI ReboxGpuFree(void *cpu)
{
    struct allocation **p, *a;
    struct chunk *c, **cp;

    if (!cpu)
        return;
    AcquireSRWLockExclusive(&lock);
    for (p = &table[bucket(cpu)]; *p && (*p)->cpu != cpu; p = &(*p)->next)
        ;
    if (!(a = *p))
    {
        ReleaseSRWLockExclusive(&lock);
        rebox_log("d3d12_xs: ReboxGpuFree(%p): not a GPU allocation", cpu);
        return;
    }
    *p = a->next;
    c = a->chunk;
    range_free(c, a->offset, a->size);
    c->used -= a->size;
    c->dirty = TRUE;
    bytes_in_use -= a->size;
    free(a);
    if (c->dedicated && !c->used)
    {
        for (cp = &chunks; *cp != c; cp = &(*cp)->next)
            ;
        *cp = c->next;
        chunk_destroy(c);
    }
    ReleaseSRWLockExclusive(&lock);
}

BOOL WINAPI ReboxGpuQuery(const void *address, void **base, SIZE_T *size)
{
    const BYTE *p = address;
    struct chunk *c;
    BOOL found = FALSE;

    AcquireSRWLockShared(&lock);
    for (c = chunks; c; c = c->next)
        if (p >= c->cpu && p < c->cpu + c->size)
        {
            if (base)
                *base = c->cpu;
            if (size)
                *size = c->size;
            found = TRUE;
            break;
        }
    ReleaseSRWLockShared(&lock);
    return found;
}

UINT64 WINAPI ReboxGpuBytesInUse(void)
{
    UINT64 n;

    AcquireSRWLockShared(&lock);
    n = bytes_in_use;
    ReleaseSRWLockShared(&lock);
    return n;
}

UINT gpu_memory_regions(struct gpu_memory_region *out, UINT max)
{
    UINT n = 0;

    AcquireSRWLockShared(&lock);
    for (struct chunk *c = chunks; c && n < max; c = c->next)
        out[n++] = (struct gpu_memory_region){ c->gpu, c->cpu, c->size };
    ReleaseSRWLockShared(&lock);
    return n;
}

/* Spans of title memory made GPU memory: 64 KiB-aligned (.#radv-rebox takes a lower-half address
 * with the page alignment), at least 2 MiB (vkd3d-proton suballocates smaller buffer heaps, so
 * the layer would never see the allocation), and as much of the committed run around the request
 * as ADOPT_GRANULE allows, so neighbouring placements land in one chunk instead of straddling two. */
#define ADOPT_ALIGN (64ull << 10)
#define ADOPT_MIN (2ull << 20)
#define ADOPT_GRANULE (256ull << 20)

/* The run of committed, writable pages around address: [*start, *end). */
static BOOL committed_run(UINT64 address, UINT64 *start, UINT64 *end)
{
    MEMORY_BASIC_INFORMATION mbi;
    UINT64 at, run_start = 0, run_end = 0;

    if (!VirtualQuery((void *)(ULONG_PTR)address, &mbi, sizeof(mbi)) || !mbi.AllocationBase)
        return FALSE;
    for (at = (UINT64)(ULONG_PTR)mbi.AllocationBase;
         VirtualQuery((void *)(ULONG_PTR)at, &mbi, sizeof(mbi)) && mbi.AllocationBase;
         at = (UINT64)(ULONG_PTR)mbi.BaseAddress + mbi.RegionSize)
    {
        BOOL usable = mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))
                && !(mbi.Protect & PAGE_GUARD);
        if (!usable)
        {
            if (run_end > address)
                break;
            run_start = run_end = 0;
            continue;
        }
        if (!run_end)
            run_start = (UINT64)(ULONG_PTR)mbi.BaseAddress;
        run_end = (UINT64)(ULONG_PTR)mbi.BaseAddress + mbi.RegionSize;
    }
    if (!run_end || address < run_start || address >= run_end)
        return FALSE;
    *start = run_start;
    *end = run_end;
    return TRUE;
}

/* Makes [address, address + size) of the title's own memory GPU-visible at the same address: a
 * heap whose memory VK_LAYER_REBOX_gpuva_identity puts at that device address and maps over the
 * title's pages (asked through the mailbox, layers/gpuva_mailbox.h), with the title's bytes
 * copied across. TRUE if the range is GPU memory now. */
BOOL gpu_memory_adopt(UINT64 address, UINT64 size)
{
    static struct gpuva_mailbox *(WINAPI *get_mailbox)(void);
    static LONG logged;
    ID3D12Device *device = rebox_shared_device();
    UINT64 end = address + size, run_start, run_end, base, limit, bytes;
    struct gpuva_mailbox *box;
    struct chunk *c;
    void *saved;
    BOOL ok = FALSE;

    if (!get_mailbox)
    {
        HMODULE xmem = GetModuleHandleA("xmem.dll");
        get_mailbox = xmem ? (void *)GetProcAddress(xmem, "ReboxGpuvaMailbox") : NULL;
    }
    if (!device || !size || !get_mailbox || !(box = get_mailbox()) || box->magic != GPUVA_MAILBOX_MAGIC)
        return FALSE;
    if (!committed_run(address, &run_start, &run_end) || end > run_end)
        return FALSE;
    /* The span: the granule-aligned neighbourhood, inside the run, 2 MiB-aligned inwards. */
    base = address & ~(ADOPT_GRANULE - 1);
    limit = (end + ADOPT_GRANULE - 1) & ~(ADOPT_GRANULE - 1);
    if (base < run_start)
        base = run_start;
    if (limit > run_end)
        limit = run_end;
    base = (base + ADOPT_ALIGN - 1) & ~(ADOPT_ALIGN - 1);
    limit &= ~(ADOPT_ALIGN - 1);

    AcquireSRWLockExclusive(&lock);
    /* Already GPU memory, wholly; otherwise keep clear of the chunks there are. */
    for (c = chunks; c; c = c->next)
    {
        if (address >= c->gpu && end <= c->gpu + c->size)
        {
            ReleaseSRWLockExclusive(&lock);
            return TRUE;
        }
        if (c->gpu < limit && base < c->gpu + c->size)
        {
            /* A page apart: RADV maps a guard page past each allocation. */
            if (c->gpu + c->size <= address)
                base = ((c->gpu + c->size + ADOPT_ALIGN - 1) & ~(ADOPT_ALIGN - 1)) + ADOPT_ALIGN;
            else if (c->gpu >= end)
                limit = (c->gpu & ~(ADOPT_ALIGN - 1)) - ADOPT_ALIGN;
            else
                limit = base; /* the request straddles a chunk: nothing to do */
        }
    }
    if (base > address || limit < end || limit - base < ADOPT_MIN)
    {
        ReleaseSRWLockExclusive(&lock);
        if (InterlockedIncrement(&logged) <= 8)
            rebox_log("d3d12_xs: title memory %#llx+%#llx can't be made GPU-visible in place (committed run "
                      "%#llx-%#llx, span %#llx-%#llx)", (unsigned long long)address, (unsigned long long)size,
                      (unsigned long long)run_start, (unsigned long long)run_end, (unsigned long long)base,
                      (unsigned long long)limit);
        return FALSE;
    }
    bytes = limit - base;
    if (!(saved = VirtualAlloc(NULL, bytes, MEM_COMMIT, PAGE_READWRITE)))
    {
        ReleaseSRWLockExclusive(&lock);
        return FALSE;
    }
    memcpy(saved, (void *)(ULONG_PTR)base, bytes);
    box->result = 0;
    box->size = bytes;
    MemoryBarrier();
    box->address = base;
    c = chunk_create(device, bytes, TRUE, TRUE);
    box->address = 0;
    if (c && (UINT64)(ULONG_PTR)c->cpu == base && c->gpu == base && box->result == base)
    {
        memcpy((void *)(ULONG_PTR)base, saved, bytes);
        c->adopted = TRUE;
        c->used = bytes;
        c->free_count = 0; /* nothing to suballocate */
        c->next = chunks;
        chunks = c;
        ok = TRUE;
    }
    else if (c)
    {
        /* Mapped elsewhere: the title's pages are untouched. Mapped there with another device
         * address can't happen (the layer uses one address for both). */
        BOOL there = (UINT64)(ULONG_PTR)c->cpu == base;
        chunk_destroy(c); /* if it was there, the layer puts plain pages back */
        if (there)
            memcpy((void *)(ULONG_PTR)base, saved, bytes);
    }
    ReleaseSRWLockExclusive(&lock);
    VirtualFree(saved, 0, MEM_RELEASE);
    if (InterlockedIncrement(&logged) <= 8)
        rebox_log("d3d12_xs: title memory %#llx+%#llx %s GPU-visible in place (for %#llx+%#llx)",
                  (unsigned long long)base, (unsigned long long)bytes, ok ? "made" : "could not be made",
                  (unsigned long long)address, (unsigned long long)size);
    return ok;
}

BOOL gpu_memory_heap_at(UINT64 address, ID3D12Heap **heap, UINT64 *offset)
{
    BOOL found = FALSE;

    AcquireSRWLockShared(&lock);
    for (struct chunk *c = chunks; c; c = c->next)
        if (address >= c->gpu && address < c->gpu + c->size)
        {
            ID3D12Heap_AddRef(c->heap);
            *heap = c->heap;
            *offset = address - c->gpu;
            found = TRUE;
            break;
        }
    ReleaseSRWLockShared(&lock);
    return found;
}
