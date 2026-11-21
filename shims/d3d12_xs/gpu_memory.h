#pragma once

#include <windows.h>
#include <d3d12.h>

/* The one device the process uses. Console titles have a single device, and
 * GPU memory handed out by xmem must live on the device the title renders
 * with, since vkd3d-proton resolves GPU VAs per device. NULL if creation
 * failed. Not reference-counted for the caller. */
ID3D12Device *rebox_shared_device(void);

/* GPU-visible memory, suballocated from CPU-visible D3D12 heaps on the shared
 * device. With VK_LAYER_REBOX_gpuva_identity the CPU pointer and the GPU
 * VA are equal; both are returned so callers can tell. */
HRESULT WINAPI ReboxGpuAlloc(SIZE_T size, SIZE_T alignment, void **cpu, UINT64 *gpu);
void WINAPI ReboxGpuFree(void *cpu);

/* If address lies in memory from ReboxGpuAlloc, returns its chunk's range. */
BOOL WINAPI ReboxGpuQuery(const void *address, void **base, SIZE_T *size);

/* Bytes handed out by ReboxGpuAlloc and not yet freed. */
UINT64 WINAPI ReboxGpuBytesInUse(void);

/* Every chunk's GPU VA, CPU pointer and size, for the shader recompiler's guest memory
 * (console_compile.c). Returns the count, at most max. */
struct gpu_memory_region { UINT64 gpu; const void *cpu; SIZE_T size; };
UINT gpu_memory_regions(struct gpu_memory_region *out, UINT max);
