/* ReboxBlit: a bilinear scale recorded into a title's command list.
 *
 * The FidelityFX stub uses it in place of an upscaler (the console FSR
 * shaders are precompiled ISA we can't run). It samples the src_w x src_h
 * corner of src and writes dst_w x dst_h of dst with a compute shader
 * (generated/blit_shaders.h), so src must be readable as an SRV and dst be
 * in UNORDERED_ACCESS, the states FSR itself requires.
 *
 * Like a real FSR dispatch it changes the list's compute root signature and
 * pipeline; the descriptor heaps the title had set are restored. */

#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS
#include <windows.h>
#include <d3d12.h>

#include "../common/rebox_log.h"
#include "../../generated/blit_shaders.h"
#include "gpu_memory.h"
#include "proxy.h"

#define RING 256 /* blits in flight; two descriptors each */

static ID3D12RootSignature *root_signature;
static ID3D12PipelineState *pipeline;
static ID3D12DescriptorHeap *heap;
static UINT increment;
static LONG next_slot;

static BOOL CALLBACK init_blit(INIT_ONCE *once, void *param, void **ctx)
{
    ID3D12Device *device = rebox_shared_device();
    D3D12_DESCRIPTOR_RANGE ranges[] = {
        { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 },
        { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1 },
    };
    D3D12_ROOT_PARAMETER params[2] = {
        { .ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE, .DescriptorTable = { 2, ranges } },
        { .ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS, .Constants = { 0, 0, 6 } },
    };
    D3D12_STATIC_SAMPLER_DESC sampler = {
        .Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR, .AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        .AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP, .AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        .MaxLOD = D3D12_FLOAT32_MAX,
    };
    D3D12_ROOT_SIGNATURE_DESC rs_desc = { 2, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, RING * 2,
                                             D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc = { 0 };
    PFN_D3D12_SERIALIZE_ROOT_SIGNATURE serialize;
    ID3D10Blob *blob = NULL;
    HMODULE d3d12;
    HRESULT hr;

    if (!device || !(d3d12 = GetModuleHandleA("d3d12.dll"))
        || !(serialize = (PFN_D3D12_SERIALIZE_ROOT_SIGNATURE)(void *)GetProcAddress(d3d12, "D3D12SerializeRootSignature")))
        return TRUE;
    if (FAILED(hr = serialize(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, NULL))
        || FAILED(hr = ID3D12Device_CreateRootSignature(device, 0, ID3D10Blob_GetBufferPointer(blob),
                ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)&root_signature)))
        goto fail;
    pso_desc.pRootSignature = root_signature;
    pso_desc.CS.pShaderBytecode = blit_cs;
    pso_desc.CS.BytecodeLength = sizeof(blit_cs);
    if (FAILED(hr = ID3D12Device_CreateComputePipelineState(device, &pso_desc, &IID_ID3D12PipelineState,
                                                            (void **)&pipeline))
        || FAILED(hr = ID3D12Device_CreateDescriptorHeap(device, &heap_desc, &IID_ID3D12DescriptorHeap, (void **)&heap)))
        goto fail;
    increment = ID3D12Device_GetDescriptorHandleIncrementSize(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    ID3D10Blob_Release(blob);
    return TRUE;

fail:
    rebox_log("d3d12_xs: blit setup failed: %#lx", hr);
    if (blob)
        ID3D10Blob_Release(blob);
    pipeline = NULL;
    return TRUE;
}

/* Views need a typed format; UAVs can't be sRGB. */
static DXGI_FORMAT view_format(DXGI_FORMAT f, BOOL uav)
{
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return uav ? DXGI_FORMAT_R8G8B8A8_UNORM : f;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return uav ? DXGI_FORMAT_B8G8R8A8_UNORM : f;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R32G32B32_TYPELESS: return DXGI_FORMAT_R32G32B32_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    default: return f;
    }
}

HRESULT WINAPI ReboxBlit(void *list_in, ID3D12Resource *src, UINT src_w, UINT src_h, ID3D12Resource *dst,
        UINT dst_w, UINT dst_h)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    ID3D12Device *device = rebox_shared_device();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = { .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
        .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, .Texture2D = { 0, 1, 0, 0.0f } };
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = { .ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D };
    D3D12_RESOURCE_BARRIER barrier = { .Type = D3D12_RESOURCE_BARRIER_TYPE_UAV };
    D3D12_CPU_DESCRIPTOR_HANDLE cpu;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu;
    D3D12_RESOURCE_DESC sd, dd;
    ID3D12DescriptorHeap *saved[2];
    ID3D12GraphicsCommandList *list;
    UINT saved_count, slot;
    struct
    {
        float inv_dst_w, inv_dst_h, uv_scale_x, uv_scale_y;
        UINT dst_w, dst_h;
    } constants;

    InitOnceExecuteOnce(&once, init_blit, NULL, NULL);
    if (!pipeline || !src || !dst || !(list = proxy_list(list_in, saved, &saved_count)))
        return E_FAIL;
    sd = ID3D12Resource_GetDesc(src);
    dd = ID3D12Resource_GetDesc(dst);
    if (sd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || dd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
        return E_INVALIDARG;
    if (!src_w || src_w > sd.Width) src_w = (UINT)sd.Width;
    if (!src_h || src_h > sd.Height) src_h = sd.Height;
    if (!dst_w || dst_w > dd.Width) dst_w = (UINT)dd.Width;
    if (!dst_h || dst_h > dd.Height) dst_h = dd.Height;

    slot = (UINT)InterlockedIncrement(&next_slot) % RING;
    cpu = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(heap);
    gpu = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(heap);
    cpu.ptr += (SIZE_T)slot * 2 * increment;
    gpu.ptr += (UINT64)slot * 2 * increment;
    srv.Format = view_format(sd.Format, FALSE);
    uav.Format = view_format(dd.Format, TRUE);
    ID3D12Device_CreateShaderResourceView(device, src, &srv, cpu);
    cpu.ptr += increment;
    ID3D12Device_CreateUnorderedAccessView(device, dst, NULL, &uav, cpu);

    constants.inv_dst_w = 1.0f / dst_w;
    constants.inv_dst_h = 1.0f / dst_h;
    constants.uv_scale_x = (float)src_w / (float)sd.Width;
    constants.uv_scale_y = (float)src_h / (float)sd.Height;
    constants.dst_w = dst_w;
    constants.dst_h = dst_h;

    ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, &heap);
    ID3D12GraphicsCommandList_SetComputeRootSignature(list, root_signature);
    ID3D12GraphicsCommandList_SetPipelineState(list, pipeline);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(list, 0, gpu);
    ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(list, 1, 6, &constants, 0);
    ID3D12GraphicsCommandList_Dispatch(list, (dst_w + 7) / 8, (dst_h + 7) / 8, 1);
    barrier.UAV.pResource = dst;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &barrier);
    if (saved_count)
        ID3D12GraphicsCommandList_SetDescriptorHeaps(list, saved_count, saved);
    return S_OK;
}
