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
 * their rays instead of the title stopping. REBOX_DUMP_STATE_OBJECTS=DIR writes each distinct
 * blob there, for working out the format.
 *
 * Unreal then inspects the native programs (FD3D12Device::GetRayTracingPipelineInfo, read from its
 * code): properties slot 7 (selector, 16-byte struct out): selector 0 fills two dwords, selector 1
 * an interface whose slot 3 gives a struct with a count at +8; state object slot 8 (out) gives an
 * interface whose slot 9 (1, 0, out) gives another, whose slot 3 gives a table (0x18-byte entries,
 * count at +0x10) of register counts (bits 0-8 and 11-18 of the dword at +0x14). The placeholder's
 * answers describe no programs: zero structs, and an inspection object whose slot 3 points at
 * zeros. Other console slots log their arguments and return 0. */

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

/* What the console's inspection slots hand out: an object whose slot 3 points at zeros (no
 * programs, no counts) and whose slot 9 gives itself. */
static const BYTE zeros[1024];
static void *const inspect_vtbl[16];
static void *const inspection = (void *)inspect_vtbl;

static HRESULT WINAPI inspect_QueryInterface(void *self, REFIID iid, void **out)
{
    if (!out)
        return E_POINTER;
    *out = self;
    return S_OK;
}
static ULONG WINAPI inspect_ref(void *self) { return 1; }
static const void *WINAPI inspect_data(void *self) { return zeros; }
static HRESULT WINAPI inspect_child(void *self, UINT64 a, UINT64 b, void **out)
{
    if (out)
        *out = (void *)&inspection;
    return S_OK;
}
static UINT64 WINAPI inspect_other(void *self, UINT64 a, UINT64 b, UINT64 c)
{
    static LONG logged;
    if (InterlockedIncrement(&logged) <= 8)
        rebox_log("d3d12_xs: placeholder inspection slot (%#llx, %#llx, %#llx): 0", (unsigned long long)a,
                     (unsigned long long)b, (unsigned long long)c);
    return 0;
}
static void *const inspect_vtbl[16] = {
    inspect_QueryInterface, inspect_ref, inspect_ref, inspect_data, inspect_other, inspect_other, inspect_other,
    inspect_other, inspect_other, inspect_child, inspect_other, inspect_other, inspect_other, inspect_other,
    inspect_other, inspect_other,
};

/* State object slot 8 (out): the inspection object. */
static HRESULT WINAPI so_programs(void *self, void **out)
{
    if (out)
        *out = (void *)&inspection;
    return S_OK;
}

/* Properties slot 7 (selector, 16-byte struct out): zeros; selector 1's first qword the
 * inspection object. */
static HRESULT WINAPI props_info(void *iface, UINT selector, void *out)
{
    static LONG logged;
    if (InterlockedIncrement(&logged) <= 4)
        rebox_log("d3d12_xs: placeholder properties slot 7 (selector %u): no programs", selector);
    if (out)
    {
        memset(out, 0, 16);
        if (selector == 1)
            *(void **)out = (void *)&inspection;
    }
    return S_OK;
}

#define SO_EXTRA(n) \
    static UINT64 WINAPI so_extra_##n(void *self, UINT64 a, UINT64 b, UINT64 c) { return extra_slot("state object", n, a, b, c); }
#define PROPS_EXTRA(n) \
    static UINT64 WINAPI props_extra_##n(void *self, UINT64 a, UINT64 b, UINT64 c) { return extra_slot("properties", n, a, b, c); }
SO_EXTRA(9) SO_EXTRA(10) SO_EXTRA(11) SO_EXTRA(12) SO_EXTRA(13) SO_EXTRA(14) SO_EXTRA(15)
PROPS_EXTRA(8) PROPS_EXTRA(9) PROPS_EXTRA(10) PROPS_EXTRA(11) PROPS_EXTRA(12) PROPS_EXTRA(13) PROPS_EXTRA(14)

static void *const so_vtbl[] = {
    so_QueryInterface, so_AddRef, so_Release, so_GetPrivateData, so_SetPrivateData, so_SetPrivateDataInterface,
    so_SetName, so_GetDevice, so_programs, so_extra_9, so_extra_10, so_extra_11, so_extra_12, so_extra_13, so_extra_14,
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
    props_GetPipelineStackSize, props_SetPipelineStackSize, props_info, props_extra_8, props_extra_9,
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

BOOL state_object_is_placeholder(const void *object)
{
    return object && *(void *const *const *)object == so_vtbl;
}

static HRESULT placeholder(IUnknown *device, REFIID riid, void **out)
{
    struct state_object *so;
    HRESULT hr;
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

/* Device slot 101 (CreateStateObject, desktop 62): Unreal links its deserialized collections into a
 * ray tracing pipeline (D3D12RayTracing.cpp). Everything in it is console code or placeholders, so
 * the pipeline is a placeholder too; the subobject types are logged at first. */
HRESULT state_object_create(IUnknown *device, const D3D12_STATE_OBJECT_DESC *desc, REFIID riid, void **out)
{
    static LONG count;
    LONG n;

    if (!out)
        return E_POINTER;
    *out = NULL;
    if (!desc)
        return E_INVALIDARG;
    n = InterlockedIncrement(&count);
    if (n == 1 || !(n & (n - 1)))
    {
        char types[160];
        int at = 0, collections = 0;
        types[0] = 0;
        for (UINT i = 0; i < desc->NumSubobjects && desc->pSubobjects && at < 140; i++)
        {
            at += snprintf(types + at, sizeof(types) - at, " %u", (UINT)desc->pSubobjects[i].Type);
            if (desc->pSubobjects[i].Type == D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION)
                collections++;
        }
        rebox_log("d3d12_xs: CreateStateObject #%ld (type %u, %u subobjects:%s; %d collections): a placeholder that "
                     "traces nothing", n, (UINT)desc->Type, desc->NumSubobjects, types, collections);
    }
    return placeholder(device, riid, out);
}

HRESULT state_object_deserialize(IUnknown *device, const void *code, SIZE_T length, void *root_signature, UINT flags,
                                 REFIID riid, void **out)
{
    static LONG count;
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
    return placeholder(device, riid, out);
}
