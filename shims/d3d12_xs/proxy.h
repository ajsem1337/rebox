#pragma once

#include <windows.h>
#include <d3d12.h>

/* D3D12.X proxies: objects with the Xbox vtable layouts that forward to
 * vkd3d-proton. See docs/d3d12x.md for the slot tables. */

/* Returns the device proxy for the shared device, as riid. */
HRESULT proxy_query_device(ID3D12Device *inner, REFIID riid, void **out);

/* Proxy -> vkd3d-proton object; anything else is returned unchanged. */
void *proxy_unwrap(void *object);
