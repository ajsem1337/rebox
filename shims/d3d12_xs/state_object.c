/* ID3D12Device8::DeserializeStateObjectX (Xbox device slot 115): a ray tracing state object from a
 * blob the console's compiler serialized.
 *
 * Unreal 5 on the console loads its ray tracing pipelines so (FD3D12RayTracingPipelineCache::
 * GetOrCompileShader), with (blob, length, global root signature, 0, IID_ID3D12StateObject, out),
 * and stops with a fatal error if that fails. The blobs hold native programs (version 2: four
 * programs, each after a DXBC root signature with RTS0 and two metadata records; the first three
 * name XDXR_TraceRay, TuxBox's census of a UE5 sample), which aren't translated yet.
 *
 * So for now the object is a placeholder: it answers as a state object and its properties, with
 * zero shader identifiers and stack sizes, and traces nothing. Ray traced passes then draw without
 * their rays instead of the title stopping. Slots past the desktop interfaces (state object 8,
 * properties 7: the console's, called with selectors 0 and 1, per TuxBox) log their arguments and
 * return 0. REBOX_DUMP_STATE_OBJECTS=DIR writes each distinct blob there, for working out the
 * format. */

#define COBJMACROS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../common/rebox_log.h"
#include "state_object.h"

#define EXTRA_SLOTS 8

struct state_object
{
    void *const *vtbl;            /* ID3D12StateObject */
    void *const *properties_vtbl; /* ID3D12StateObjectProperties */
    LONG refs;
    IUnknown *device;
    UINT64 pipeline_stack_size;
};

static struct state_object *from_properties(void *iface)
{
    return (struct state_object *)((BYTE *)iface - offsetof(struct state_object, properties_vtbl));
}

static ULONG WINAPI so_AddRef(struct state_object *so) { return InterlockedIncrement(&so->refs); }

static ULONG WINAPI so_Release(struct state_object *so)
{
    LONG left = InterlockedDecrement(&so->refs);
    if (!left)
    {
        if (so->device)
            IUnknown_Release(so->device);
        free(so);
    }
    return left;
}

static HRESULT WINAPI so_QueryInterface(struct state_object *so, REFIID iid, void **out)
{
    if (!out)
        return E_POINTER;
    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_ID3D12Object) || IsEqualGUID(iid, &IID_ID3D12DeviceChild)
            || IsEqualGUID(iid, &IID_ID3D12Pageable) || IsEqualGUID(iid, &IID_ID3D12StateObject))
        *out = so;
    else if (IsEqualGUID(iid, &IID_ID3D12StateObjectProperties))
        *out = &so->properties_vtbl;
    else
    {
        static LONG logged;
        if (InterlockedIncrement(&logged) <= 4)
            rebox_log("d3d12_xs: state object asked for interface {%08lx-%04x-%04x-...}: not a placeholder's",
                         (unsigned long)iid->Data1, iid->Data2, iid->Data3);
        *out = NULL;
        return E_NOINTERFACE;
    }
    so_AddRef(so);
    return S_OK;
}

static HRESULT WINAPI so_GetPrivateData(struct state_object *so, REFGUID guid, UINT *size, void *data)
{
    if (size)
        *size = 0;
    return DXGI_ERROR_NOT_FOUND;
}
static HRESULT WINAPI so_SetPrivateData(struct state_object *so, REFGUID guid, UINT size, const void *data) { return S_OK; }
static HRESULT WINAPI so_SetPrivateDataInterface(struct state_object *so, REFGUID guid, const IUnknown *data) { return S_OK; }
static HRESULT WINAPI so_SetName(struct state_object *so, const WCHAR *name) { return S_OK; }
static HRESULT WINAPI so_GetDevice(struct state_object *so, REFIID iid, void **out)
{
    if (!out)
        return E_POINTER;
    *out = NULL;
    return so->device ? IUnknown_QueryInterface(so->device, iid, out) : E_FAIL;
}

/* Slots past the desktop interfaces: what they're asked, once each, and 0. */
static UINT64 extra_slot(const char *iface, UINT slot, UINT64 a, UINT64 b, UINT64 c)
{
    static LONG seen[2][EXTRA_SLOTS];
    if (InterlockedIncrement(&seen[iface[0] == 'p'][slot % EXTRA_SLOTS]) <= 4)
        rebox_log("d3d12_xs: placeholder %s slot %u (%#llx, %#llx, %#llx): 0", iface, slot, (unsigned long long)a,
                     (unsigned long long)b, (unsigned long long)c);
    return 0;
}

#define SO_EXTRA(n) \
    static UINT64 WINAPI so_extra_##n(void *self, UINT64 a, UINT64 b, UINT64 c) { return extra_slot("state object", n, a, b, c); }
#define PROPS_EXTRA(n) \
    static UINT64 WINAPI props_extra_##n(void *self, UINT64 a, UINT64 b, UINT64 c) { return extra_slot("properties", n, a, b, c); }
SO_EXTRA(8) SO_EXTRA(9) SO_EXTRA(10) SO_EXTRA(11) SO_EXTRA(12) SO_EXTRA(13) SO_EXTRA(14) SO_EXTRA(15)
PROPS_EXTRA(7) PROPS_EXTRA(8) PROPS_EXTRA(9) PROPS_EXTRA(10) PROPS_EXTRA(11) PROPS_EXTRA(12) PROPS_EXTRA(13) PROPS_EXTRA(14)

static void *const so_vtbl[] = {
    so_QueryInterface, so_AddRef, so_Release, so_GetPrivateData, so_SetPrivateData, so_SetPrivateDataInterface,
    so_SetName, so_GetDevice, so_extra_8, so_extra_9, so_extra_10, so_extra_11, so_extra_12, so_extra_13, so_extra_14,
    so_extra_15,
};

static HRESULT WINAPI props_QueryInterface(void *iface, REFIID iid, void **out)
{
    return so_QueryInterface(from_properties(iface), iid, out);
}
static ULONG WINAPI props_AddRef(void *iface) { return so_AddRef(from_properties(iface)); }
static ULONG WINAPI props_Release(void *iface) { return so_Release(from_properties(iface)); }

/* A shader identifier is 32 bytes (D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES); a placeholder's are all
 * zero, which records copied into shader tables name no shader. */
static void *WINAPI props_GetShaderIdentifier(void *iface, const WCHAR *export_name)
{
    static const BYTE zero[D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES];
    return (void *)zero;
}
static UINT64 WINAPI props_GetShaderStackSize(void *iface, const WCHAR *export_name) { return 0; }
static UINT64 WINAPI props_GetPipelineStackSize(void *iface) { return from_properties(iface)->pipeline_stack_size; }
static void WINAPI props_SetPipelineStackSize(void *iface, UINT64 size) { from_properties(iface)->pipeline_stack_size = size; }

static void *const props_vtbl[] = {
    props_QueryInterface, props_AddRef, props_Release, props_GetShaderIdentifier, props_GetShaderStackSize,
    props_GetPipelineStackSize, props_SetPipelineStackSize, props_extra_7, props_extra_8, props_extra_9,
    props_extra_10, props_extra_11, props_extra_12, props_extra_13, props_extra_14,
};

static UINT64 fnv1a(const BYTE *p, SIZE_T n)
{
    UINT64 h = 0xcbf29ce484222325ull;
    while (n--)
        h = (h ^ *p++) * 0x100000001b3ull;
    return h;
}

/* REBOX_DUMP_STATE_OBJECTS=DIR: each distinct blob as DIR\<fnv1a64>.bin. */
static void dump(const void *code, SIZE_T length)
{
    static char dir[MAX_PATH];
    static LONG init;
    char path[MAX_PATH + 32];
    HANDLE f;

    if (!InterlockedExchange(&init, 1)
            && !GetEnvironmentVariableA("REBOX_DUMP_STATE_OBJECTS", dir, sizeof(dir)))
        dir[0] = 0;
    if (!dir[0] || !code || !length)
        return;
    CreateDirectoryA(dir, NULL);
    snprintf(path, sizeof(path), "%s\\%016llx.bin", dir, (unsigned long long)fnv1a(code, length));
    f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
    if (f != INVALID_HANDLE_VALUE)
    {
        DWORD written;
        WriteFile(f, code, (DWORD)length, &written, NULL);
        CloseHandle(f);
    }
}

HRESULT state_object_deserialize(IUnknown *device, const void *code, SIZE_T length, void *root_signature, UINT flags,
                                 REFIID riid, void **out)
{
    static LONG count;
    struct state_object *so;
    HRESULT hr;
    LONG n;

    if (!out)
        return E_POINTER;
    *out = NULL;
    if (!code || !length)
        return E_INVALIDARG;
    n = InterlockedIncrement(&count);
    if (n == 1 || !(n & (n - 1)))
        rebox_log("d3d12_xs: DeserializeStateObjectX #%ld (%zu bytes, version %u, root signature %p, flags %#x): a "
                     "placeholder that traces nothing (console ray tracing code isn't translated yet)", n, (size_t)length,
                     length >= 4 ? *(const UINT *)code : 0, root_signature, flags);
    dump(code, length);
    if (!(so = calloc(1, sizeof(*so))))
        return E_OUTOFMEMORY;
    so->vtbl = so_vtbl;
    so->properties_vtbl = props_vtbl;
    so->refs = 1;
    if ((so->device = device))
        IUnknown_AddRef(device);
    hr = so_QueryInterface(so, riid, out);
    so_Release(so);
    return hr;
}
