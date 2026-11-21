#pragma once

#include <windows.h>
#include <d3d12.h>

/* Console-format shadow heaps; see shadow.c. */
void shadow_heap_created(ID3D12DescriptorHeap *inner, const D3D12_DESCRIPTOR_HEAP_DESC *desc, UINT increment);
void shadow_heap_destroyed(ID3D12DescriptorHeap *inner);
void shadow_write_cbv(SIZE_T cpu, const D3D12_CONSTANT_BUFFER_VIEW_DESC *desc);
void shadow_write_srv(SIZE_T cpu, ID3D12Resource *res, const D3D12_SHADER_RESOURCE_VIEW_DESC *desc);
void shadow_write_uav(SIZE_T cpu, ID3D12Resource *res, const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc);
void shadow_write_sampler(SIZE_T cpu, const D3D12_SAMPLER_DESC *desc);
void shadow_copy(SIZE_T dst, SIZE_T src, UINT count);
void shadow_copy_ranges(UINT dst_count, const D3D12_CPU_DESCRIPTOR_HANDLE *dst_starts, const UINT *dst_sizes,
        UINT src_count, const D3D12_CPU_DESCRIPTOR_HANDLE *src_starts, const UINT *src_sizes);

/* Shader-visible shadows, for the recompiler's guest memory and the BDA page table. */
struct shadow_region { UINT64 title_va, gpu_va; const void *data; SIZE_T size; };
UINT shadow_regions(struct shadow_region *out, UINT max);
/* The slot of the heap starting at heap_cpu that holds these console words (4 or 8), allowing
 * the fields AnyPS5 rewrites in bound descriptors. */
BOOL shadow_find_index(SIZE_T heap_cpu, const UINT32 *words, UINT n, BOOL sampler, UINT *index);
