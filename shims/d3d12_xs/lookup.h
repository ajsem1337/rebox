#pragma once

#include <windows.h>
#include <d3d12.h>

/* Descriptor lookup tables for console shaders; see lookup.c. */
void lookup_heap_created(ID3D12DescriptorHeap *inner, const D3D12_DESCRIPTOR_HEAP_DESC *desc);
void lookup_descriptors_written(SIZE_T cpu, UINT count);
