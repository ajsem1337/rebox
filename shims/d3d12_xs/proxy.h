#pragma once

#include <windows.h>
#include <d3d12.h>

/* D3D12.X proxies: objects with the Xbox vtable layouts that forward to
 * vkd3d-proton. See docs/d3d12x.md for the slot tables. */

/* Returns the device proxy for the shared device, as riid. */
HRESULT proxy_query_device(ID3D12Device *inner, REFIID riid, void **out);
/* The SDK version from D3D12XBOX_CREATE_DEVICE_PARAMETERS: picks the queue layout. */
void proxy_set_sdk_version(UINT32 version);

/* For work d3d12_xs records into a title's command list: the wrapped list,
 * plus the descriptor heaps the title last set on it (up to 2), so they can
 * be restored. A list that isn't a proxy is returned as is, with no heaps. */
ID3D12GraphicsCommandList *proxy_list(void *list, ID3D12DescriptorHeap **heaps, UINT *heap_count);

/* REBOX_SLOT_STATS: called once per PresentX; logs per-slot call counts
 * every N frames. Slots are marked c (our code), T (trap), or neither
 * (forwarded to the PC slot). */
void proxy_frame_presented(void);
void proxy_log_slot_stats(void);

/* Proxy -> vkd3d-proton object; anything else is returned unchanged. */
void *proxy_unwrap(void *object);

/* The texture the title knows at this address (a T# it wrote itself holds it), or NULL. */
ID3D12Resource *proxy_texture_at(UINT64 address);
