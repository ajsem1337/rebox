/* IDXGIDevice for the console device.
 *
 * Retail titles go from the D3D12 device to IDXGIDevice, its adapter (GetAdapter, +0x38), the
 * adapter's outputs (EnumOutputs, +0x38; the output is handed to SetFrameIntervalX) and its
 * parent IDXGIFactory2, without null checks (ScarlettX, XValkyrie). On the console the device
 * answers IDXGIDevice; vkd3d-proton's doesn't. So the device proxy hands out this object: its
 * adapter is DXVK's first adapter (the same DXGI present.c uses), whose DXGI layouts match the
 * console's, and the rest of IDXGIDevice4 gives harmless answers. */

#define COBJMACROS
#include <windows.h>
#include <dxgi1_6.h>

#include "../common/rebox_log.h"
#include "dxgi_device.h"

struct dxgi_device
{
    IDXGIDevice4 iface;
    IUnknown *d3d12; /* the device proxy, for QueryInterface back to D3D12 */
    IDXGIAdapter *adapter;
};

static struct dxgi_device *impl(IDXGIDevice4 *iface)
{
    return CONTAINING_RECORD(iface, struct dxgi_device, iface);
}

static HRESULT STDMETHODCALLTYPE dev_QueryInterface(IDXGIDevice4 *iface, REFIID riid, void **out)
{
    if (!out)
        return E_POINTER;
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDXGIObject) || IsEqualGUID(riid, &IID_IDXGIDevice)
            || IsEqualGUID(riid, &IID_IDXGIDevice1) || IsEqualGUID(riid, &IID_IDXGIDevice2)
            || IsEqualGUID(riid, &IID_IDXGIDevice3) || IsEqualGUID(riid, &IID_IDXGIDevice4))
    {
        *out = iface;
        return S_OK;
    }
    return IUnknown_QueryInterface(impl(iface)->d3d12, riid, out);
}

/* Lives as long as the process, like the device. */
static ULONG STDMETHODCALLTYPE dev_AddRef(IDXGIDevice4 *iface) { return 2; }
static ULONG STDMETHODCALLTYPE dev_Release(IDXGIDevice4 *iface) { return 1; }

static HRESULT STDMETHODCALLTYPE dev_SetPrivateData(IDXGIDevice4 *iface, REFGUID guid, UINT size, const void *data)
{
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dev_SetPrivateDataInterface(IDXGIDevice4 *iface, REFGUID guid, const IUnknown *object)
{
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dev_GetPrivateData(IDXGIDevice4 *iface, REFGUID guid, UINT *size, void *data)
{
    if (size)
        *size = 0;
    return DXGI_ERROR_NOT_FOUND;
}

static HRESULT STDMETHODCALLTYPE dev_GetAdapter(IDXGIDevice4 *iface, IDXGIAdapter **adapter);

static HRESULT STDMETHODCALLTYPE dev_GetParent(IDXGIDevice4 *iface, REFIID riid, void **parent)
{
    IDXGIAdapter *adapter;
    HRESULT hr;

    if (!parent)
        return E_POINTER;
    *parent = NULL;
    if (FAILED(hr = dev_GetAdapter(iface, &adapter)))
        return hr;
    hr = IDXGIAdapter_QueryInterface(adapter, riid, parent);
    IDXGIAdapter_Release(adapter);
    return hr;
}

static BOOL CALLBACK find_adapter(INIT_ONCE *once, void *param, void **context)
{
    struct dxgi_device *d = param;
    HRESULT (WINAPI *create)(REFIID, void **);
    HMODULE m = LoadLibraryA("dxgi.dll");
    IDXGIFactory1 *factory = NULL;
    IDXGIAdapter1 *adapter = NULL;

    create = m ? (void *)GetProcAddress(m, "CreateDXGIFactory1") : NULL;
    if (create && SUCCEEDED(create(&IID_IDXGIFactory1, (void **)&factory))
            && SUCCEEDED(IDXGIFactory1_EnumAdapters1(factory, 0, &adapter)))
        d->adapter = (IDXGIAdapter *)adapter;
    if (factory)
        IDXGIFactory1_Release(factory);
    rebox_log("d3d12_xs: IDXGIDevice::GetAdapter -> %s", d->adapter ? "DXGI's first adapter" : "none (no DXGI)");
    return TRUE;
}

static HRESULT STDMETHODCALLTYPE dev_GetAdapter(IDXGIDevice4 *iface, IDXGIAdapter **adapter)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    struct dxgi_device *d = impl(iface);

    if (!adapter)
        return E_POINTER;
    InitOnceExecuteOnce(&once, find_adapter, d, NULL);
    if (!d->adapter)
    {
        *adapter = NULL;
        return DXGI_ERROR_NOT_FOUND;
    }
    IDXGIAdapter_AddRef(d->adapter);
    *adapter = d->adapter;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dev_CreateSurface(IDXGIDevice4 *iface, const DXGI_SURFACE_DESC *desc, UINT count,
        DXGI_USAGE usage, const DXGI_SHARED_RESOURCE *shared, IDXGISurface **surface)
{
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dev_QueryResourceResidency(IDXGIDevice4 *iface, IUnknown *const *resources,
        DXGI_RESIDENCY *status, UINT count)
{
    for (UINT i = 0; status && i < count; i++)
        status[i] = DXGI_RESIDENCY_FULLY_RESIDENT;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dev_SetGPUThreadPriority(IDXGIDevice4 *iface, INT priority) { return S_OK; }

static HRESULT STDMETHODCALLTYPE dev_GetGPUThreadPriority(IDXGIDevice4 *iface, INT *priority)
{
    if (!priority)
        return E_POINTER;
    *priority = 0;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dev_SetMaximumFrameLatency(IDXGIDevice4 *iface, UINT latency) { return S_OK; }

static HRESULT STDMETHODCALLTYPE dev_GetMaximumFrameLatency(IDXGIDevice4 *iface, UINT *latency)
{
    if (!latency)
        return E_POINTER;
    *latency = 3;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dev_OfferResources(IDXGIDevice4 *iface, UINT count, IDXGIResource *const *resources,
        DXGI_OFFER_RESOURCE_PRIORITY priority)
{
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dev_ReclaimResources(IDXGIDevice4 *iface, UINT count, IDXGIResource *const *resources,
        BOOL *discarded)
{
    for (UINT i = 0; discarded && i < count; i++)
        discarded[i] = FALSE;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dev_EnqueueSetEvent(IDXGIDevice4 *iface, HANDLE event)
{
    return event && SetEvent(event) ? S_OK : E_INVALIDARG;
}

static void STDMETHODCALLTYPE dev_Trim(IDXGIDevice4 *iface) {}

static HRESULT STDMETHODCALLTYPE dev_OfferResources1(IDXGIDevice4 *iface, UINT count, IDXGIResource *const *resources,
        DXGI_OFFER_RESOURCE_PRIORITY priority, UINT flags)
{
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dev_ReclaimResources1(IDXGIDevice4 *iface, UINT count, IDXGIResource *const *resources,
        DXGI_RECLAIM_RESOURCE_RESULTS *results)
{
    for (UINT i = 0; results && i < count; i++)
        results[i] = DXGI_RECLAIM_RESOURCE_RESULT_OK;
    return S_OK;
}

static const IDXGIDevice4Vtbl vtbl = {
    dev_QueryInterface, dev_AddRef, dev_Release,
    dev_SetPrivateData, dev_SetPrivateDataInterface, dev_GetPrivateData, dev_GetParent,
    dev_GetAdapter, dev_CreateSurface, dev_QueryResourceResidency, dev_SetGPUThreadPriority, dev_GetGPUThreadPriority,
    dev_SetMaximumFrameLatency, dev_GetMaximumFrameLatency,
    dev_OfferResources, dev_ReclaimResources, dev_EnqueueSetEvent,
    dev_Trim,
    dev_OfferResources1, dev_ReclaimResources1,
};

static struct dxgi_device device = { { (IDXGIDevice4Vtbl *)&vtbl } };

BOOL dxgi_device_iid(REFIID riid)
{
    return IsEqualGUID(riid, &IID_IDXGIDevice) || IsEqualGUID(riid, &IID_IDXGIDevice1)
            || IsEqualGUID(riid, &IID_IDXGIDevice2) || IsEqualGUID(riid, &IID_IDXGIDevice3)
            || IsEqualGUID(riid, &IID_IDXGIDevice4) || IsEqualGUID(riid, &IID_IDXGIObject);
}

void *dxgi_device_get(IUnknown *d3d12_device)
{
    device.d3d12 = d3d12_device;
    return &device.iface;
}
