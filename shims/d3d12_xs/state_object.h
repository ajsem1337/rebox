#pragma once

#include <windows.h>
#include <d3d12.h>

/* Ray tracing state objects from serialized console code, device slot 115 (state_object.c). */
HRESULT state_object_deserialize(IUnknown *device, const void *code, SIZE_T length, void *root_signature, UINT flags,
                                 REFIID riid, void **out);
HRESULT state_object_create(IUnknown *device, const D3D12_STATE_OBJECT_DESC *desc, REFIID riid, void **out);
BOOL state_object_is_placeholder(const void *object);
