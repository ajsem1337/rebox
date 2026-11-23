#pragma once

#include <windows.h>
#include <d3d12.h>

/* Ray tracing state objects from serialized console code, device slot 115 (state_object.c). */
HRESULT state_object_deserialize(IUnknown *device, const void *code, SIZE_T length, void *root_signature, UINT flags,
                                 REFIID riid, void **out);
