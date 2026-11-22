#pragma once

#include <windows.h>
#include <d3d12.h>

/* Textures placed at title addresses, uploaded from title memory; see title_textures.c. */
void title_texture_register(ID3D12Resource *resource, UINT64 address, const D3D12_RESOURCE_DESC *console_desc,
                            const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES state);
void title_textures_upload(ID3D12CommandQueue *queue);
void WINAPI ReboxTitleMemoryWritten(UINT64 address, UINT64 size);
