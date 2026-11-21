#pragma once

#include <windows.h>
#include <d3d12.h>

/* Console-format (RDNA2) descriptors for D3D12 views; see console_desc.c.
 * Resource descriptors fill 8 dwords (T# or V# plus zeros), samplers 4. */
void console_encode_srv(ID3D12Resource *res, const D3D12_SHADER_RESOURCE_VIEW_DESC *desc, UINT32 out[8]);
void console_encode_uav(ID3D12Resource *res, const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc, UINT32 out[8]);
void console_encode_cbv(const D3D12_CONSTANT_BUFFER_VIEW_DESC *desc, UINT32 out[8]);
void console_encode_sampler(const D3D12_SAMPLER_DESC *desc, UINT32 out[4]);
