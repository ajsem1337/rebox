#pragma once

#include <windows.h>
#include <d3d12.h>

/* Derived pipeline states, device slot 55 (derived_pso.c). */
#define DERIVED_PSO_PATCH_SIZE 0x14C

void derived_pso_record(const void *pso, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc);
HRESULT derived_pso_desc(const void *base, UINT count, const void *patches, D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc);
