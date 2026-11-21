/* Descriptor lookup tables (docs/shaders.md, patch 4).
 *
 * A translated console shader holds raw descriptor bytes (a T# or S# it
 * loaded from a descriptor table) and needs the bindless index of the
 * descriptor they came from. Each shader-visible CBV/SRV/UAV or sampler heap
 * gets a GPU-visible table mapping descriptor bytes to heap index. The
 * patched vkd3d-proton (0003) passes the bound heaps' tables to console
 * shaders in their push block.
 *
 * Layout (all dwords): [0] mask (capacity - 1), [1] entry size in dwords (9),
 * [2..3] reserved, then entries { key[8], index }, index 0xffffffff when
 * empty. Key: the descriptor's bytes as they sit in the heap, 32 bytes for
 * CBV/SRV/UAV, 16 for samplers padded with 0xffffffff. Hash: FNV-1a over the
 * 8 key dwords (h = 2166136261; h = (h ^ k[i]) * 16777619), slot (h + p) &
 * mask for probes p = 0..7. A lookup that finds no entry within 8 probes
 * misses.
 *
 * Entries are added when the title writes descriptors through the API
 * (Create*View, CreateSampler, CopyDescriptors*), read back from the heap
 * after vkd3d-proton wrote them. Bytes a title writes into heap memory
 * itself are not seen. Overwritten descriptors leave their old entries
 * behind; those only matter to a shader still holding the old bytes. */

#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS
#include <windows.h>
#include <d3d12.h>
#include <stdlib.h>
#include <string.h>

#include "../common/rebox_log.h"
#include "gpu_memory.h"
#include "lookup.h"

#define ENTRY_DWORDS 9
#define HEADER_DWORDS 4
#define PROBES 8
#define EMPTY 0xffffffffu

struct lookup_heap
{
    SIZE_T cpu_start, cpu_end;
    UINT stride;
    UINT32 *table;
    void *table_cpu; /* ReboxGpuAlloc block */
    struct lookup_heap *next;
};

static SRWLOCK lock = SRWLOCK_INIT;
static struct lookup_heap *heaps;

typedef HRESULT (WINAPI *heap_va_fn)(ID3D12DescriptorHeap *, UINT64 *, UINT *);
typedef HRESULT (WINAPI *set_lookup_fn)(ID3D12DescriptorHeap *, UINT64);

static UINT32 hash_key(const UINT32 key[8])
{
    UINT32 h = 2166136261u;
    for (int i = 0; i < 8; i++)
        h = (h ^ key[i]) * 16777619u;
    return h;
}

static void insert(struct lookup_heap *heap, UINT index)
{
    UINT32 key[8], mask = heap->table[0], h;
    static LONG full_logged;

    memset(key, 0xff, sizeof(key));
    memcpy(key, (const BYTE *)heap->cpu_start + (SIZE_T)index * heap->stride,
           heap->stride < sizeof(key) ? heap->stride : sizeof(key));
    h = hash_key(key);
    for (UINT p = 0; p < PROBES; p++)
    {
        UINT32 *e = heap->table + HEADER_DWORDS + ((h + p) & mask) * ENTRY_DWORDS;
        if (e[8] == EMPTY || !memcmp(e, key, sizeof(key)))
        {
            memcpy(e, key, sizeof(key));
            e[8] = index;
            return;
        }
    }
    if (!InterlockedExchange(&full_logged, 1))
        rebox_log("d3d12_xs: descriptor lookup table: no free slot within %u probes (heap index %u)", PROBES, index);
}

void lookup_heap_created(ID3D12DescriptorHeap *inner, const D3D12_DESCRIPTOR_HEAP_DESC *desc)
{
    HMODULE core = GetModuleHandleA("d3d12core.dll");
    heap_va_fn heap_va = core ? (heap_va_fn)(void *)GetProcAddress(core, "ReboxGetDescriptorHeapVA") : NULL;
    set_lookup_fn set_lookup = core ? (set_lookup_fn)(void *)GetProcAddress(core, "ReboxSetDescriptorHeapLookup") : NULL;
    struct lookup_heap *heap, **p;
    D3D12_CPU_DESCRIPTOR_HANDLE start;
    UINT64 real, table_gpu;
    UINT stride, capacity = 64;
    SIZE_T size;
    void *cpu;

    if (!desc || !(desc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) || !heap_va || !set_lookup
            || (desc->Type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV && desc->Type != D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER)
            || FAILED(heap_va(inner, &real, &stride)))
        return;
    while (capacity < desc->NumDescriptors * 2u && capacity < (1u << 24))
        capacity <<= 1;
    size = (HEADER_DWORDS + (SIZE_T)capacity * ENTRY_DWORDS) * sizeof(UINT32);
    if (FAILED(ReboxGpuAlloc(size, 256, &cpu, &table_gpu)) || !(heap = calloc(1, sizeof(*heap))))
    {
        rebox_log("d3d12_xs: no memory for a descriptor lookup table (%u entries)", capacity);
        return;
    }
    memset(cpu, 0xff, size);
    heap->table = cpu;
    heap->table[0] = capacity - 1;
    heap->table[1] = ENTRY_DWORDS;
    heap->table[2] = heap->table[3] = 0;
    heap->table_cpu = cpu;
    start = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(inner);
    heap->cpu_start = start.ptr;
    heap->cpu_end = start.ptr + (SIZE_T)desc->NumDescriptors * stride;
    heap->stride = stride;
    set_lookup(inner, table_gpu);

    AcquireSRWLockExclusive(&lock);
    /* A destroyed heap's CPU range can come back: drop overlapping tables. */
    for (p = &heaps; *p;)
    {
        struct lookup_heap *h = *p;
        if (h->cpu_start < heap->cpu_end && heap->cpu_start < h->cpu_end)
        {
            *p = h->next;
            ReboxGpuFree(h->table_cpu);
            free(h);
        }
        else
            p = &h->next;
    }
    heap->next = heaps;
    heaps = heap;
    ReleaseSRWLockExclusive(&lock);
}

void lookup_descriptors_written(SIZE_T cpu, UINT count)
{
    AcquireSRWLockExclusive(&lock);
    for (struct lookup_heap *h = heaps; h; h = h->next)
        if (cpu >= h->cpu_start && cpu < h->cpu_end)
        {
            UINT first = (UINT)((cpu - h->cpu_start) / h->stride);
            UINT last = (UINT)((h->cpu_end - h->cpu_start) / h->stride);
            for (UINT i = first; i < first + count && i < last; i++)
                insert(h, i);
            break;
        }
    ReleaseSRWLockExclusive(&lock);
}
