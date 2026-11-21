/* Descriptor lookup table (docs/shaders.md, patch 4).
 *
 * A translated console shader holds raw descriptor bytes (a T# or S# it
 * loaded from a descriptor table) and needs the bindless index of the
 * descriptor they came from. One GPU-visible table maps descriptor bytes to
 * heap index for every shader-visible CBV/SRV/UAV and sampler heap. It is
 * shared because translated shaders (Leonida's) look T#s and S#s up in the
 * same table; a title with several shader-visible heaps of one type would see
 * indices collide (console engines normally use one of each). The patched
 * vkd3d-proton (0003) passes it to console shaders in their push block.
 *
 * Layout (all dwords), the one Leonida's translator reads: [0] mask
 * (capacity - 1), then entries { key[8], index, pad } from dword 1, index
 * 0xffffffff when empty. Key: the descriptor's bytes as they sit in the heap,
 * 32 bytes for CBV/SRV/UAV, 16 for samplers padded with 0xffffffff. Hash:
 * FNV-1a over the 8 key dwords (h = 2166136261; h = (h ^ k[i]) * 16777619),
 * entry (h + p) & mask for probes p = 0..7. A lookup that finds no entry
 * within 8 probes misses (Leonida's shaders then use index 0).
 *
 * Entries are added when the title writes descriptors through the API
 * (Create*View, CreateSampler, CopyDescriptors*), read back from the heap
 * after vkd3d-proton wrote them; that needs descriptor buffers (RADV). Other
 * heaps still get the (empty) table, so lookups miss instead of reading
 * address 0. Bytes a title writes into heap memory itself are not seen.
 * Overwritten descriptors leave their old entries behind; those only matter
 * to a shader still holding the old bytes. */

#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS
#include <windows.h>
#include <d3d12.h>
#include <stdlib.h>
#include <string.h>

#include "../common/rebox_log.h"
#include "gpu_memory.h"
#include "lookup.h"

#define ENTRY_DWORDS 10
#define HEADER_DWORDS 1
#define PROBES 8
#define EMPTY 0xffffffffu
#define MAX_CAPACITY (1u << 22)

struct lookup_heap
{
    ID3D12DescriptorHeap *inner; /* not referenced: only compared and passed back */
    SIZE_T cpu_start, cpu_end;   /* 0 when the heap's descriptors can't be read */
    UINT stride, count;
    struct lookup_heap *next;
};

static SRWLOCK lock = SRWLOCK_INIT;
static struct lookup_heap *heaps;
static UINT32 *table;   /* current table; older ones are kept, the GPU may still read them */
static UINT64 table_gpu;
static UINT capacity, descriptors;

typedef HRESULT (WINAPI *heap_va_fn)(ID3D12DescriptorHeap *, UINT64 *, UINT *);
typedef HRESULT (WINAPI *set_lookup_fn)(ID3D12DescriptorHeap *, UINT64);

static UINT32 hash_key(const UINT32 key[8])
{
    UINT32 h = 2166136261u;
    for (int i = 0; i < 8; i++)
        h = (h ^ key[i]) * 16777619u;
    return h;
}

static void insert(const struct lookup_heap *heap, UINT index)
{
    UINT32 key[8], mask = table[0], h;
    static LONG full_logged;

    memset(key, 0xff, sizeof(key));
    memcpy(key, (const BYTE *)heap->cpu_start + (SIZE_T)index * heap->stride,
           heap->stride < sizeof(key) ? heap->stride : sizeof(key));
    h = hash_key(key);
    for (UINT p = 0; p < PROBES; p++)
    {
        UINT32 *e = table + HEADER_DWORDS + ((h + p) & mask) * ENTRY_DWORDS;
        if (e[8] == EMPTY || !memcmp(e, key, sizeof(key)))
        {
            memcpy(e, key, sizeof(key));
            e[8] = index;
            e[9] = 0;
            return;
        }
    }
    if (!InterlockedExchange(&full_logged, 1))
        rebox_log("d3d12_xs: descriptor lookup table: no free slot within %u probes (heap index %u)", PROBES, index);
}

/* Caller holds the lock. Allocates a table for `want` entries and refills it
 * from every readable heap. */
static BOOL grow(UINT want)
{
    UINT32 *cpu;
    UINT64 gpu;
    SIZE_T size;

    size = (HEADER_DWORDS + (SIZE_T)want * ENTRY_DWORDS) * sizeof(UINT32);
    if (FAILED(ReboxGpuAlloc(size, 256, (void **)&cpu, &gpu)))
    {
        rebox_log("d3d12_xs: no memory for a descriptor lookup table (%u entries)", want);
        return FALSE;
    }
    memset(cpu, 0xff, size);
    cpu[0] = want - 1;
    table = cpu;
    table_gpu = gpu;
    capacity = want;
    for (struct lookup_heap *h = heaps; h; h = h->next)
        if (h->cpu_start)
            for (UINT i = 0; i < h->count; i++)
                insert(h, i);
    return TRUE;
}

void lookup_heap_created(ID3D12DescriptorHeap *inner, const D3D12_DESCRIPTOR_HEAP_DESC *desc)
{
    HMODULE core = GetModuleHandleA("d3d12core.dll");
    heap_va_fn heap_va = core ? (heap_va_fn)(void *)GetProcAddress(core, "ReboxGetDescriptorHeapVA") : NULL;
    set_lookup_fn set_lookup = core ? (set_lookup_fn)(void *)GetProcAddress(core, "ReboxSetDescriptorHeapLookup") : NULL;
    struct lookup_heap *heap, **p;
    UINT want = capacity ? capacity : 1024;
    UINT64 real;
    UINT stride;

    if (!desc || !(desc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) || !set_lookup
            || (desc->Type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV && desc->Type != D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER)
            || !(heap = calloc(1, sizeof(*heap))))
        return;
    heap->inner = inner;
    heap->count = desc->NumDescriptors;
    if (heap_va && SUCCEEDED(heap_va(inner, &real, &stride)))
    {
        D3D12_CPU_DESCRIPTOR_HANDLE start = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(inner);
        heap->cpu_start = start.ptr;
        heap->cpu_end = start.ptr + (SIZE_T)desc->NumDescriptors * stride;
        heap->stride = stride;
    }

    AcquireSRWLockExclusive(&lock);
    /* A destroyed heap's CPU range or object can come back: forget it. */
    for (p = &heaps; *p;)
    {
        struct lookup_heap *h = *p;
        if (h->inner == inner || (heap->cpu_start && h->cpu_start < heap->cpu_end && heap->cpu_start < h->cpu_end))
        {
            descriptors -= h->count;
            *p = h->next;
            free(h);
        }
        else
            p = &h->next;
    }
    heap->next = heaps;
    heaps = heap;
    descriptors += heap->count;
    while (want < descriptors * 2u && want < MAX_CAPACITY)
        want <<= 1;
    if (want != capacity && grow(want))
    {
        /* Every heap, including older ones, now points at the new table. */
        for (struct lookup_heap *h = heaps; h; h = h->next)
            set_lookup(h->inner, table_gpu);
    }
    else if (table)
        set_lookup(inner, table_gpu);
    ReleaseSRWLockExclusive(&lock);
}

void lookup_heap_destroyed(ID3D12DescriptorHeap *inner)
{
    AcquireSRWLockExclusive(&lock);
    for (struct lookup_heap **p = &heaps; *p; p = &(*p)->next)
        if ((*p)->inner == inner)
        {
            struct lookup_heap *h = *p;
            descriptors -= h->count;
            *p = h->next;
            free(h);
            break;
        }
    ReleaseSRWLockExclusive(&lock);
}

void lookup_descriptors_written(SIZE_T cpu, UINT count)
{
    AcquireSRWLockExclusive(&lock);
    if (table)
        for (struct lookup_heap *h = heaps; h; h = h->next)
            if (h->cpu_start && cpu >= h->cpu_start && cpu < h->cpu_end)
            {
                UINT first = (UINT)((cpu - h->cpu_start) / h->stride);
                for (UINT i = first; i < first + count && i < h->count; i++)
                    insert(h, i);
                break;
            }
    ReleaseSRWLockExclusive(&lock);
}
