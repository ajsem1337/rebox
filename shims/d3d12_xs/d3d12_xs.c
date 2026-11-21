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
#include "../../generated/fallback_shaders.h"

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
/* Newer public subobjects the mingw headers lack. Only their sizes matter
 * here; every field is 4 bytes wide (stencil masks are two UINT8s padded to
 * 4), as in the D3D12 headers:
 *   DEPTH_STENCIL_DESC2: 4 fields, 2 x DEPTH_STENCILOP_DESC1 (5 words), 1 field
 *   RASTERIZER_DESC1: 11 fields; RASTERIZER_DESC2: 10 fields */
typedef struct { UINT words[15]; } depth_stencil_desc2;
typedef struct { UINT words[11]; } rasterizer_desc1;
typedef struct { UINT words[10]; } rasterizer_desc2;
SUBOBJ(depth_stencil2, depth_stencil_desc2)
SUBOBJ(rasterizer1, rasterizer_desc1)
SUBOBJ(rasterizer2, rasterizer_desc2)
#undef SUBOBJ

#define SUBOBJECT_TYPE_DEPTH_STENCIL2 26
#define SUBOBJECT_TYPE_RASTERIZER1 27
#define SUBOBJECT_TYPE_RASTERIZER2 28

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
    [SUBOBJECT_TYPE_DEPTH_STENCIL2] = LAYOUT(depth_stencil2, NULL),
    [SUBOBJECT_TYPE_RASTERIZER1] = LAYOUT(rasterizer1, NULL),
    [SUBOBJECT_TYPE_RASTERIZER2] = LAYOUT(rasterizer2, NULL),
};
#undef LAYOUT

/* A console shader with no translation would fail PSO creation, which a
 * title rarely survives. Instead the whole pipeline gets fallback shaders
 * (generated/fallback_shaders.h): graphics draws nothing, compute does
 * nothing, and the title keeps running with that effect missing. They use no
 * resources, so they fit any root signature. REBOX_SHADER_FALLBACK=0
 * restores failing, to find missing shaders. Mesh pipelines get a mesh
 * shader that emits nothing and an amplification shader that launches
 * nothing (DXIL, SM 6.5). */
static const D3D12_SHADER_BYTECODE fallback_vs_bc = { fallback_vs, sizeof(fallback_vs) };
static const D3D12_SHADER_BYTECODE fallback_ps_bc = { fallback_ps, sizeof(fallback_ps) };
static const D3D12_SHADER_BYTECODE fallback_cs_bc = { fallback_cs, sizeof(fallback_cs) };
static const D3D12_SHADER_BYTECODE fallback_ms_bc = { fallback_ms, sizeof(fallback_ms) };
static const D3D12_SHADER_BYTECODE fallback_as_bc = { fallback_as, sizeof(fallback_as) };
static const D3D12_SHADER_BYTECODE no_shader;

static BOOL fallback_enabled(void)
{
    const char *env = getenv("REBOX_SHADER_FALLBACK");
    return !env || *env != '0';
}

static void note_fallback(const char *kind)
{
    static LONG count;
    LONG n = InterlockedIncrement(&count);

    if (n <= 16 || !(n & (n - 1)))
        rebox_log("d3d12_xs: %s PSO #%ld uses fallback shaders (missing translation); the effect is skipped",
                     kind, n);
}

/* A CachedPSO blob vkd3d-proton didn't make (a console PSO cache entry, say)
 * fails creation as a cache mismatch; retry once without it. */
static BOOL retry_without_cache(HRESULT hr, const D3D12_CACHED_PIPELINE_STATE *cached)
{
    static LONG logged;

    if (SUCCEEDED(hr) || !cached->pCachedBlob || !cached->CachedBlobSizeInBytes)
        return FALSE;
    if (!InterlockedExchange(&logged, 1))
        rebox_log("d3d12_xs: PSO creation with a %zu-byte CachedPSO blob failed (%#lx); retrying without it",
                     (size_t)cached->CachedBlobSizeInBytes, hr);
    return TRUE;
}

static HRESULT STDMETHODCALLTYPE hook_create_graphics(ID3D12Device2 *device,
        const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc, REFIID riid, void **pso)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC copy;
    BOOL miss = FALSE;

    HRESULT hr;

    if (!desc)
        return orig_create_graphics(device, desc, riid, pso);
    {
        static const char *const names[] = { "vs", "ps", "ds", "hs", "gs" };
        const D3D12_SHADER_BYTECODE stages[] = { desc->VS, desc->PS, desc->DS, desc->HS, desc->GS };
        shader_cache_census("graphics", names, stages, 5, &desc->CachedPSO);
    }
    copy = *desc;
    miss |= shader_cache_substitute(&desc->VS, &copy.VS, "vs") == SHADER_MISS;
    miss |= shader_cache_substitute(&desc->PS, &copy.PS, "ps") == SHADER_MISS;
    miss |= shader_cache_substitute(&desc->DS, &copy.DS, "ds") == SHADER_MISS;
    miss |= shader_cache_substitute(&desc->HS, &copy.HS, "hs") == SHADER_MISS;
    miss |= shader_cache_substitute(&desc->GS, &copy.GS, "gs") == SHADER_MISS;
    if (miss && fallback_enabled())
    {
        copy.VS = fallback_vs_bc;
        copy.PS = desc->PS.BytecodeLength ? fallback_ps_bc : no_shader;
        copy.HS = copy.DS = copy.GS = no_shader;
        memset(&copy.StreamOutput, 0, sizeof(copy.StreamOutput));
        if (copy.PrimitiveTopologyType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH)
            copy.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        note_fallback("graphics");
    }
    hr = orig_create_graphics(device, &copy, riid, pso);
    if (retry_without_cache(hr, &copy.CachedPSO))
    {
        memset(&copy.CachedPSO, 0, sizeof(copy.CachedPSO));
        hr = orig_create_graphics(device, &copy, riid, pso);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hook_create_compute(ID3D12Device2 *device,
        const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc, REFIID riid, void **pso)
{
    D3D12_COMPUTE_PIPELINE_STATE_DESC copy;
    static const char *const names[] = { "cs" };
    HRESULT hr;

    if (!desc)
        return orig_create_compute(device, desc, riid, pso);
    shader_cache_census("compute", names, &desc->CS, 1, &desc->CachedPSO);
    copy = *desc;
    if (shader_cache_substitute(&desc->CS, &copy.CS, "cs") == SHADER_MISS && fallback_enabled())
    {
        copy.CS = fallback_cs_bc;
        note_fallback("compute");
    }
    hr = orig_create_compute(device, &copy, riid, pso);
    if (retry_without_cache(hr, &copy.CachedPSO))
    {
        memset(&copy.CachedPSO, 0, sizeof(copy.CachedPSO));
        hr = orig_create_compute(device, &copy, riid, pso);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hook_create_pipeline(ID3D12Device2 *device,
        const D3D12_PIPELINE_STATE_STREAM_DESC *desc, REFIID riid, void **pso)
{
    D3D12_PIPELINE_STATE_STREAM_DESC copy;
    SIZE_T offset = 0, stages[16], topology = 0, stream_output = 0, cached_pso = 0;
    const char *census_names[16] = { 0 };
    D3D12_SHADER_BYTECODE census_bc[16] = { 0 };
    UINT stage_count = 0;
    BOOL miss = FALSE;
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
            if (stage_count < ARRAYSIZE(stages))
            {
                census_names[stage_count] = subobj_layouts[type].stage;
                census_bc[stage_count] = *bc;
            }
            miss |= shader_cache_substitute(bc, bc, subobj_layouts[type].stage) == SHADER_MISS;
            if (stage_count < ARRAYSIZE(stages))
                stages[stage_count++] = offset;
        }
        else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY)
            topology = offset + subobj_layouts[type].offset;
        else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT)
            stream_output = offset + subobj_layouts[type].offset;
        else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO)
            cached_pso = offset + subobj_layouts[type].offset;
        offset += subobj_layouts[type].size;
    }
    shader_cache_census("stream", census_names, census_bc, stage_count,
                        cached_pso ? (const D3D12_CACHED_PIPELINE_STATE *)(stream + cached_pso) : NULL);

    if (miss && fallback_enabled())
    {
        for (UINT i = 0; i < stage_count; i++)
        {
            UINT type = *(const UINT *)(stream + stages[i]);
            D3D12_SHADER_BYTECODE *bc = (D3D12_SHADER_BYTECODE *)(stream + stages[i] + subobj_layouts[type].offset);

            if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS)
                *bc = fallback_vs_bc;
            else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS)
                *bc = bc->BytecodeLength ? fallback_ps_bc : no_shader;
            else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS)
                *bc = fallback_cs_bc;
            else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS)
                *bc = fallback_ms_bc;
            else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS)
                *bc = fallback_as_bc;
            else
                *bc = no_shader; /* HS, DS, GS */
        }
        if (stream_output)
            memset(stream + stream_output, 0, sizeof(D3D12_STREAM_OUTPUT_DESC));
        if (topology && *(UINT *)(stream + topology) == D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH)
            *(UINT *)(stream + topology) = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        note_fallback("stream");
    }

    copy.SizeInBytes = desc->SizeInBytes;
    copy.pPipelineStateSubobjectStream = stream;
    hr = orig_create_pipeline(device, &copy, riid, pso);
    if (cached_pso && retry_without_cache(hr, (D3D12_CACHED_PIPELINE_STATE *)(stream + cached_pso)))
    {
        memset(stream + cached_pso, 0, sizeof(D3D12_CACHED_PIPELINE_STATE));
        hr = orig_create_pipeline(device, &copy, riid, pso);
    }
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
