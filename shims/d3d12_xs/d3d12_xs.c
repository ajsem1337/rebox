/* Replacement for the console's d3d12_xs.dll (D3D12.X), layered on
 * vkd3d-proton's d3d12.dll.
 *
 *   - forwards the standard D3D12 exports (see d3d12_xs.def);
 *   - D3D12XboxCreateDevice returns a proxy with the Xbox vtable layouts
 *     (proxy.c) over one shared vkd3d-proton device;
 *   - hands out GPU-visible memory for xmem.dll (gpu_memory.c);
 *   - hooks pipeline creation and swaps console shader bytecode for
 *     AnyPS5-translated DXIL from the shader cache. */

#define COBJMACROS
#include <initguid.h>
#include <windows.h>
#include <d3d12.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "../common/rebox_log.h"
#include "gpu_memory.h"
#include "proxy.h"
#include "shader_cache.h"

static INIT_ONCE load_once = INIT_ONCE_STATIC_INIT;
static PFN_D3D12_CREATE_DEVICE real_create_device;

static HRESULT (STDMETHODCALLTYPE *orig_create_graphics)(ID3D12Device2 *, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *, REFIID, void **);
static HRESULT (STDMETHODCALLTYPE *orig_create_compute)(ID3D12Device2 *, const D3D12_COMPUTE_PIPELINE_STATE_DESC *, REFIID, void **);
static HRESULT (STDMETHODCALLTYPE *orig_create_pipeline)(ID3D12Device2 *, const D3D12_PIPELINE_STATE_STREAM_DESC *, REFIID, void **);

static BOOL CALLBACK load_d3d12(INIT_ONCE *once, void *param, void **ctx)
{
    HMODULE module = LoadLibraryA("d3d12.dll");

    if (!module)
    {
        rebox_log("d3d12_xs: failed to load d3d12.dll (is vkd3d-proton installed?)");
        return TRUE;
    }
    real_create_device = (PFN_D3D12_CREATE_DEVICE)(void *)GetProcAddress(module, "D3D12CreateDevice");
    return TRUE;
}

static const char *debugstr_guid(const GUID *g, char *buf)
{
    wsprintfA(buf, "{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}", g->Data1, g->Data2, g->Data3,
              g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6],
              g->Data4[7]);
    return buf;
}

/* Pipeline state streams: each subobject is { type; payload } aligned to
 * pointer size. Payload offset and total size per type come from these. */
#define SUBOBJ(name, T) \
    typedef struct { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type; T inner; } __attribute__((aligned(sizeof(void *)))) subobj_##name;
SUBOBJ(ptr, void *)
SUBOBJ(bytecode, D3D12_SHADER_BYTECODE)
SUBOBJ(stream_output, D3D12_STREAM_OUTPUT_DESC)
SUBOBJ(blend, D3D12_BLEND_DESC)
SUBOBJ(uint, UINT)
SUBOBJ(rasterizer, D3D12_RASTERIZER_DESC)
SUBOBJ(depth_stencil, D3D12_DEPTH_STENCIL_DESC)
SUBOBJ(input_layout, D3D12_INPUT_LAYOUT_DESC)
SUBOBJ(rt_formats, struct D3D12_RT_FORMAT_ARRAY) /* no typedef in mingw headers */
SUBOBJ(sample_desc, DXGI_SAMPLE_DESC)
SUBOBJ(cached_pso, D3D12_CACHED_PIPELINE_STATE)
SUBOBJ(depth_stencil1, D3D12_DEPTH_STENCIL_DESC1)
SUBOBJ(view_instancing, D3D12_VIEW_INSTANCING_DESC)
#undef SUBOBJ

#define LAYOUT(name, stage) { offsetof(subobj_##name, inner), sizeof(subobj_##name), stage }
static const struct
{
    SIZE_T offset, size;
    const char *stage; /* non-NULL for shader bytecode subobjects */
} subobj_layouts[] =
{
    [D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE] = LAYOUT(ptr, NULL),
    [D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS] = LAYOUT(bytecode, "vs"),
    [D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS] = LAYOUT(bytecode, "ps"),
    [D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS] = LAYOUT(bytecode, "ds"),
    [D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS] = LAYOUT(bytecode, "hs"),
    [D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS] = LAYOUT(bytecode, "gs"),
    [D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS] = LAYOUT(bytecode, "cs"),
    [D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT] = LAYOUT(stream_output, NULL),
    [D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND] = LAYOUT(blend, NULL),
    [D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK] = LAYOUT(uint, NULL),
    [D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER] = LAYOUT(rasterizer, NULL),
    [D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL] = LAYOUT(depth_stencil, NULL),
    [D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT] = LAYOUT(input_layout, NULL),
    [D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE] = LAYOUT(uint, NULL),
    [D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY] = LAYOUT(uint, NULL),
    [D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS] = LAYOUT(rt_formats, NULL),
    [D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT] = LAYOUT(uint, NULL),
    [D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC] = LAYOUT(sample_desc, NULL),
    [D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK] = LAYOUT(uint, NULL),
    [D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO] = LAYOUT(cached_pso, NULL),
    [D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS] = LAYOUT(uint, NULL),
    [D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1] = LAYOUT(depth_stencil1, NULL),
    [D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING] = LAYOUT(view_instancing, NULL),
    [D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS] = LAYOUT(bytecode, "as"),
    [D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS] = LAYOUT(bytecode, "ms"),
};
#undef LAYOUT

static HRESULT STDMETHODCALLTYPE hook_create_graphics(ID3D12Device2 *device,
        const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc, REFIID riid, void **pso)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC copy;

    if (!desc)
        return orig_create_graphics(device, desc, riid, pso);
    copy = *desc;
    shader_cache_substitute(&desc->VS, &copy.VS, "vs");
    shader_cache_substitute(&desc->PS, &copy.PS, "ps");
    shader_cache_substitute(&desc->DS, &copy.DS, "ds");
    shader_cache_substitute(&desc->HS, &copy.HS, "hs");
    shader_cache_substitute(&desc->GS, &copy.GS, "gs");
    return orig_create_graphics(device, &copy, riid, pso);
}

static HRESULT STDMETHODCALLTYPE hook_create_compute(ID3D12Device2 *device,
        const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc, REFIID riid, void **pso)
{
    D3D12_COMPUTE_PIPELINE_STATE_DESC copy;

    if (!desc)
        return orig_create_compute(device, desc, riid, pso);
    copy = *desc;
    shader_cache_substitute(&desc->CS, &copy.CS, "cs");
    return orig_create_compute(device, &copy, riid, pso);
}

static HRESULT STDMETHODCALLTYPE hook_create_pipeline(ID3D12Device2 *device,
        const D3D12_PIPELINE_STATE_STREAM_DESC *desc, REFIID riid, void **pso)
{
    D3D12_PIPELINE_STATE_STREAM_DESC copy;
    SIZE_T offset = 0;
    BYTE *stream;
    HRESULT hr;

    if (!desc || !desc->pPipelineStateSubobjectStream || !(stream = malloc(desc->SizeInBytes)))
        return orig_create_pipeline(device, desc, riid, pso);
    memcpy(stream, desc->pPipelineStateSubobjectStream, desc->SizeInBytes);

    while (offset < desc->SizeInBytes)
    {
        UINT type = *(const UINT *)(stream + offset);

        if (type >= ARRAYSIZE(subobj_layouts) || !subobj_layouts[type].size)
        {
            static LONG once;

            if (!InterlockedExchange(&once, 1))
                rebox_log("d3d12_xs: unknown pipeline subobject type %u, passing stream through untranslated", type);
            free(stream);
            return orig_create_pipeline(device, desc, riid, pso);
        }
        if (subobj_layouts[type].stage)
        {
            D3D12_SHADER_BYTECODE *bc = (D3D12_SHADER_BYTECODE *)(stream + offset + subobj_layouts[type].offset);
            shader_cache_substitute(bc, bc, subobj_layouts[type].stage);
        }
        offset += subobj_layouts[type].size;
    }

    copy.SizeInBytes = desc->SizeInBytes;
    copy.pPipelineStateSubobjectStream = stream;
    hr = orig_create_pipeline(device, &copy, riid, pso);
    free(stream);
    return hr;
}

static void patch_slot(void **slot, void *hook, void **orig)
{
    DWORD old;

    if (*slot == hook)
        return;
    VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old);
    *orig = *slot;
    *slot = hook;
    VirtualProtect(slot, sizeof(*slot), old, &old);
}

static void install_hooks(IUnknown *unk)
{
    static SRWLOCK lock = SRWLOCK_INIT;
    ID3D12Device2 *device;
    ID3D12Device2Vtbl *vtbl;

    if (FAILED(IUnknown_QueryInterface(unk, &IID_ID3D12Device2, (void **)&device)))
    {
        rebox_log("d3d12_xs: device lacks ID3D12Device2, shader substitution disabled");
        return;
    }
    /* vkd3d-proton shares one vtable across devices, so patch it once. */
    vtbl = device->lpVtbl;
    AcquireSRWLockExclusive(&lock);
    patch_slot((void **)&vtbl->CreateGraphicsPipelineState, hook_create_graphics, (void **)&orig_create_graphics);
    patch_slot((void **)&vtbl->CreateComputePipelineState, hook_create_compute, (void **)&orig_create_compute);
    patch_slot((void **)&vtbl->CreatePipelineState, hook_create_pipeline, (void **)&orig_create_pipeline);
    ReleaseSRWLockExclusive(&lock);
    ID3D12Device2_Release(device);
}

HRESULT WINAPI D3D12CreateDevice(IUnknown *adapter, D3D_FEATURE_LEVEL level, REFIID riid, void **device)
{
    HRESULT hr;

    InitOnceExecuteOnce(&load_once, load_d3d12, NULL, NULL);
    if (!real_create_device)
        return E_FAIL;
    hr = real_create_device(adapter, level, riid, device);
    if (SUCCEEDED(hr) && device && *device)
        install_hooks(*device);
    return hr;
}

static BOOL CALLBACK create_shared_device(INIT_ONCE *once, void *param, void **ctx)
{
    ID3D12Device **device = param;
    /* 11_0 is the D3D12 minimum; capabilities beyond it are queried, not
     * requested, so this doesn't limit real GPUs. */
    HRESULT hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)device);

    if (FAILED(hr))
    {
        rebox_log("d3d12_xs: creating the shared device failed: %#lx", hr);
        *device = NULL;
    }
    return TRUE;
}

ID3D12Device *rebox_shared_device(void)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    static ID3D12Device *device;

    InitOnceExecuteOnce(&once, create_shared_device, &device, NULL);
    return device;
}

/* Console device creation. The adapter and D3D12XBOX_CREATE_DEVICE_PARAMETERS
 * are ignored; vkd3d-proton picks the Vulkan device. Every call returns the
 * same device, as on the console. */
HRESULT WINAPI D3D12XboxCreateDevice(IUnknown *adapter, const void *params, REFIID riid, void **device)
{
    ID3D12Device *shared = rebox_shared_device();
    char buf[40];

    rebox_log("d3d12_xs: D3D12XboxCreateDevice(riid %s), creation parameters ignored", debugstr_guid(riid, buf));
    if (!device)
        return E_POINTER;
    *device = NULL;
    if (!shared)
        return E_FAIL;
    return proxy_query_device(shared, riid, device);
}
