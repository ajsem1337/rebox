#pragma once

#include <windows.h>
#include <d3d12.h>

/* Bilinear scale of the src_w x src_h corner of src into dst_w x dst_h of
 * dst, recorded into list (an Xbox list proxy or a plain list). src must be
 * readable as an SRV, dst be in UNORDERED_ACCESS. See blit.c. */
HRESULT WINAPI ReboxBlit(void *list, ID3D12Resource *src, UINT src_w, UINT src_h, ID3D12Resource *dst,
                            UINT dst_w, UINT dst_h);
