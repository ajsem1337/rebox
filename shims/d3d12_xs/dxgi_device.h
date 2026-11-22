#pragma once

#include <windows.h>

/* The console device's IDXGIDevice (dxgi_device.c). */
BOOL dxgi_device_iid(REFIID riid);
void *dxgi_device_get(IUnknown *d3d12_device);
