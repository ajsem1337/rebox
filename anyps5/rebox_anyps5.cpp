// rebox_anyps5.dll: see rebox_anyps5.h. GPL-2.0-only.
#define REBOX_ANYPS5_BUILD
#include "rebox_anyps5.h"

#include "BdaAbi.hpp"
#include "Optimization/RequestMemoryView.hpp"
#include "Optimization/ResourceProgram.hpp"
#include "Recompiler.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <exception>
#include <optional>
#include <span>
#include <spirv/unified1/spirv.hpp>
#include <string_view>
#include <vector>

using namespace ShaderRecompiler;

#include "sa5_internal.hpp"

namespace {

// Every stage (graphics and compute): through AnyPS5's resource capture and result memo, as its own driver draws. A
// draw whose captured resources match an earlier one's reuses that result with only its per-draw
// data patched in (buffer tables, the flattened SRT, shader data), instead of materializing and
// emitting the variant again (a UE5 title draws the same shaders with new descriptor tables every
// draw).
std::shared_ptr<const RecompileResult> RecompileMemoized(const RecompileRequest& request,
                                                         const std::vector<MemoryRegion>& regions)
{
    if (!request.useCache) return std::make_shared<const RecompileResult>(Recompile(request));
    RequestMemoryView memory{std::span<const MemoryRegion>(regions)};
    const auto runtime = memory.MakeRuntime(request.context.userData, request.shader.codeAddress);
    const auto capture = CaptureResources(request, runtime);
    return Recompile(request, *capture);
}

// A conservative Vulkan 1.2-class target. TODO: fill it from vkd3d-proton's
// VkPhysicalDevice (subgroup size, limits, capabilities, extensions).
SpirvTarget DefaultTarget(std::uint32_t wave)
{
    // Capabilities AnyPS5 gates on, limited to ones RADV and lavapipe both expose and vkd3d-proton
    // enables. MinLod: without it image_sample_d_cl fails (americas-recomp, 29 pixel shaders of one title).
    static constexpr std::array<std::uint32_t, 16> capabilities{
        spv::CapabilityInt64, spv::CapabilityPhysicalStorageBufferAddresses, spv::CapabilityStorageBuffer8BitAccess,
        spv::CapabilityClipDistance, spv::CapabilityInt64Atomics, spv::CapabilityMinLod, spv::CapabilityImageQuery,
        spv::CapabilityImageGatherExtended, spv::CapabilityDerivativeControl,
        spv::CapabilityStorageImageReadWithoutFormat, spv::CapabilityStorageImageWriteWithoutFormat,
        spv::CapabilityGroupNonUniformVote, spv::CapabilityGroupNonUniformArithmetic,
        spv::CapabilityGroupNonUniformBallot, spv::CapabilityGroupNonUniformShuffle, spv::CapabilityGroupNonUniformQuad,
    };
    static constexpr std::array<std::string_view, 4> extensions{
        "SPV_EXT_descriptor_indexing", "SPV_KHR_8bit_storage", "SPV_KHR_float_controls", "SPV_KHR_physical_storage_buffer",
    };
    return {0x00402000u, 0x00010500u, wave ? wave : 32u, BdaAbi::Version, capabilities, extensions, false,
            {1024, 1024, 64}, 1024, 65536, {}, {}};
}

void SetError(char* error, std::size_t size, const char* message)
{
    // Not snprintf: AnyPS5's messages can be huge (the serialized request), past what its int
    // count holds.
    if (error && size) {
        const std::size_t n = std::min(std::strlen(message), size - 1);
        std::memcpy(error, message, n);
        error[n] = 0;
    }
}

}  // namespace

extern "C" {

SA5_API int sa5_compile_compute(const sa5_compute_request* request, sa5_result** out, char* error, std::size_t error_size)
{
    if (!request || !out || !request->code || !request->code_dwords) {
        SetError(error, error_size, "bad request");
        return -1;
    }
    *out = nullptr;
    try {
        std::vector<MemoryRegion> regions;
        for (std::uint32_t i = 0; i < request->region_count; i++)
            regions.push_back({request->regions[i].address,
                               std::span<const std::byte>(static_cast<const std::byte*>(request->regions[i].data),
                                                          request->regions[i].size)});

        // COMPUTE_PGM_RSRC2: TGID_X/Y/Z_EN bits 7-9, TIDIG_COMP_CNT 11-12, LDS_SIZE 15-23 (512-byte units).
        const std::uint32_t rsrc1 = request->rsrc1, rsrc2 = request->rsrc2;
        const ShaderComputeStageInfo compute{
            {request->threads[0], request->threads[1], request->threads[2]},
            ((rsrc2 >> 15u) & 0x1ffu) * 512u / 4u,
            {((rsrc2 >> 7u) & 1u) != 0u, ((rsrc2 >> 8u) & 1u) != 0u, ((rsrc2 >> 9u) & 1u) != 0u},
            false,
            ((rsrc2 >> 11u) & 3u) + 1u,
        };
        // COMPUTE_PGM_RSRC1: FLOAT_MODE bits 12-19, DX10_CLAMP 21, IEEE_MODE 23.
        ShaderFloatMode floatMode;
        floatMode.floatMode = (rsrc1 >> 12u) & 0xffu;
        floatMode.dx10Clamp = ((rsrc1 >> 21u) & 1u) != 0u;
        floatMode.ieeeMode = ((rsrc1 >> 23u) & 1u) != 0u;

        RecompileRequest compile{
            {ShaderStage::Compute, request->code_address, std::span<const std::uint32_t>(request->code, request->code_dwords),
             0, {}},
            {request->wave ? request->wave : 32u, 0,
             std::span<const std::uint32_t>(request->user_data, request->user_data_count), compute, std::nullopt,
             std::nullopt, MemoryRegionView(std::span<const MemoryRegion>(regions)), floatMode},
            DefaultTarget(request->wave),
            {0, 0, 0, 128},
        };
        compile.useCache = request->use_cache != 0;

        *out = new sa5_result(RecompileMemoized(compile, regions));
        return 0;
    } catch (const std::exception& e) {
        SetError(error, error_size, e.what());
        return -1;
    } catch (...) {
        SetError(error, error_size, "unknown exception");
        return -1;
    }
}

SA5_API int sa5_compile_pixel(const sa5_pixel_request* request, sa5_result** out, char* error, std::size_t error_size)
{
    if (!request || !out || !request->code || !request->code_dwords) {
        SetError(error, error_size, "bad request");
        return -1;
    }
    *out = nullptr;
    try {
        std::vector<MemoryRegion> regions;
        for (std::uint32_t i = 0; i < request->region_count; i++)
            regions.push_back({request->regions[i].address,
                               std::span<const std::byte>(static_cast<const std::byte*>(request->regions[i].data),
                                                          request->regions[i].size)});
        // Built like americas-recomp's tools/xbox/driver.cpp (GPL-2.0), with the PSO's register
        // state in place of its defaults.
        ShaderPixelStageInfo px{};
        const std::uint32_t ena = request->input_ena;
        const auto has = [&](PixelInput input) { return (ena & PixelInputBit(input)) != 0; };
        px.interpolatorCount = std::min<std::uint32_t>(request->interpolator_count, 32u);
        for (std::uint32_t i = 0; i < px.interpolatorCount; i++) px.interpolatorSettings[i] = request->interpolator_settings[i];
        px.wave32 = request->wave != 64u;
        px.inputAddr = ena;
        px.hasPerspectiveCenterVgpr = has(PixelInput::PerspectiveCenter);
        px.perspectiveCentroid = has(PixelInput::PerspectiveCentroid);
        px.noPerspective = has(PixelInput::LinearCenter);
        px.linearCentroid = has(PixelInput::LinearCentroid);
        px.posX = has(PixelInput::PositionX);
        px.posY = has(PixelInput::PositionY);
        px.posZ = has(PixelInput::PositionZ);
        px.posW = has(PixelInput::PositionW);
        px.frontFace = has(PixelInput::FrontFace);
        px.ancillary = has(PixelInput::Ancillary);
        px.depthExportEnable = request->depth_export != 0;
        for (std::uint32_t i = 0; i < 8; i++) {
            px.targetOutputMode[i] = static_cast<std::uint8_t>((request->col_format >> (4 * i)) & 0xfu);
            px.targetExportMapping[i] = 0xe4;  // identity swizzle
            px.targetExportPacking[i] = ColorExportPacking::None;
        }
        // SPI_SHADER_PGM_RSRC1_PS: FLOAT_MODE bits 12-19, DX10_CLAMP 21, IEEE_MODE 23.
        ShaderFloatMode floatMode;
        floatMode.floatMode = (request->rsrc1 >> 12u) & 0xffu;
        floatMode.dx10Clamp = ((request->rsrc1 >> 21u) & 1u) != 0u;
        floatMode.ieeeMode = ((request->rsrc1 >> 23u) & 1u) != 0u;

        RecompileRequest compile{};
        compile.shader = {ShaderStage::Fragment, request->code_address,
                          std::span<const std::uint32_t>(request->code, request->code_dwords), 0, {}};
        compile.context.waveSize = request->wave ? request->wave : 64u;
        compile.context.userDataBaseRegister = 0;
        compile.context.userData = std::span<const std::uint32_t>(request->user_data, request->user_data_count);
        compile.context.pixel = px;
        compile.context.memory = MemoryRegionView(std::span<const MemoryRegion>(regions));
        compile.context.floatMode = floatMode;
        compile.target = DefaultTarget(compile.context.waveSize);
        compile.layout = {0, 0, request->push_offset, request->push_size ? request->push_size : 128u};
        compile.useCache = request->use_cache != 0;

        *out = new sa5_result(RecompileMemoized(compile, regions));
        return 0;
    } catch (const std::exception& e) {
        SetError(error, error_size, e.what());
        return -1;
    } catch (...) {
        SetError(error, error_size, "unknown exception");
        return -1;
    }
}

SA5_API int sa5_compile_vertex(const sa5_vertex_request* request, sa5_result** out, char* error, std::size_t error_size)
{
    if (!request || !out || !request->code || !request->code_dwords || request->input_count > 32) {
        SetError(error, error_size, "bad request");
        return -1;
    }
    *out = nullptr;
    try {
        std::vector<MemoryRegion> regions;
        for (std::uint32_t i = 0; i < request->region_count; i++)
            regions.push_back({request->regions[i].address,
                               std::span<const std::byte>(static_cast<const std::byte*>(request->regions[i].data),
                                                          request->regions[i].size)});
        ShaderVertexStageInfo vs{};
        vs.resourcesNum = request->input_count;
        for (std::uint32_t i = 0; i < request->input_count; i++) {
            const auto& in = request->inputs[i];
            vs.resources[i].fields = {in.vsharp[0], in.vsharp[1], in.vsharp[2], in.vsharp[3]};
            vs.resourcesDst[i].registerStart = static_cast<std::int32_t>(in.register_start);
            vs.resourcesDst[i].registersNum = static_cast<std::int32_t>(in.register_count);
            vs.resourcesDst[i].attrId = static_cast<std::int32_t>(i);
            vs.resourcesDst[i].fetchIndex = 0;  // per-vertex
        }
        vs.fetchEmbedded = true;  // inputs become Vulkan vertex attributes; the real fetch shader isn't in guest memory
        ShaderFloatMode floatMode;
        floatMode.floatMode = (request->rsrc1 >> 12u) & 0xffu;
        floatMode.dx10Clamp = ((request->rsrc1 >> 21u) & 1u) != 0u;
        floatMode.ieeeMode = ((request->rsrc1 >> 23u) & 1u) != 0u;

        RecompileRequest compile{};
        compile.shader = {ShaderStage::Vertex, request->code_address,
                          std::span<const std::uint32_t>(request->code, request->code_dwords), 0, {}};
        compile.context.waveSize = request->wave ? request->wave : 64u;
        compile.context.userDataBaseRegister = 0;
        compile.context.userData = std::span<const std::uint32_t>(request->user_data, request->user_data_count);
        compile.context.vertex = vs;
        compile.context.memory = MemoryRegionView(std::span<const MemoryRegion>(regions));
        compile.context.floatMode = floatMode;
        compile.target = DefaultTarget(compile.context.waveSize);
        compile.layout = {0, 0, request->push_offset, request->push_size ? request->push_size : 128u};
        compile.useCache = request->use_cache != 0;

        *out = new sa5_result(RecompileMemoized(compile, regions));
        return 0;
    } catch (const std::exception& e) {
        SetError(error, error_size, e.what());
        return -1;
    } catch (...) {
        SetError(error, error_size, "unknown exception");
        return -1;
    }
}

// A console mesh shader through AnyPS5's mesh path (rebox_anyps5.h): its primitive shader over a
// non-indexed point list, one point per lane, group_stride points per workgroup, so that lane n of
// workgroup g gets v5 = g * group_stride + n as from the console's fast launch. AnyPS5's own driver
// builds the same MeshConfiguration from VGT_SHADER_STAGES_EN and the GE registers for a geometry
// program (Graphics/src/State.cpp); a console mesh program has none of those, only its group size
// and output limits.
SA5_API int sa5_compile_mesh(const sa5_mesh_request* request, sa5_result** out, char* error, std::size_t error_size)
{
    if (!request || !out || !request->code || !request->code_dwords || !request->user_data
            || request->user_data_count < 8 || request->user_data_count > 64 || !request->threads
            || !request->max_vertices || !request->max_primitives || request->max_vertices > 256
            || request->max_primitives > 256 || request->primitive_vertices < 1 || request->primitive_vertices > 3) {
        SetError(error, error_size, "bad request");
        return -1;
    }
    *out = nullptr;
    try {
        std::vector<MemoryRegion> regions;
        for (std::uint32_t i = 0; i < request->region_count; i++)
            regions.push_back({request->regions[i].address,
                               std::span<const std::byte>(static_cast<const std::byte*>(request->regions[i].data),
                                                          request->regions[i].size)});
        const std::uint32_t wave = request->wave ? request->wave : 64u;
        const std::uint32_t stride = request->group_stride ? request->group_stride
                : std::max({request->threads, request->max_vertices, request->max_primitives});
        const std::uint32_t lanes = std::max(request->threads, stride);
        // s[4:7]: the index buffer V# AnyPS5's mesh path loads through, never read here (no draw is
        // indexed): a null buffer that isn't all zeros, so no host takes it for a descriptor the
        // title has yet to write.
        std::vector<std::uint32_t> user(request->user_data, request->user_data + request->user_data_count);
        user[4] = user[5] = user[6] = 0;
        user[7] = (77u << 12) | 0xfacu;  // 32_32_32_32_FLOAT, identity swizzle

        ShaderVertexStageInfo vs{};
        vs.paClVsOutCntl = request->clip_cntl;
        ShaderFloatMode floatMode;
        floatMode.floatMode = (request->rsrc1 >> 12u) & 0xffu;
        floatMode.dx10Clamp = ((request->rsrc1 >> 21u) & 1u) != 0u;
        floatMode.ieeeMode = ((request->rsrc1 >> 23u) & 1u) != 0u;

        // The points: stride per group, each its own primitive; ESGS items don't exist (no vertex
        // half), but AnyPS5 wants a size for the input offsets it hands out in v0-v1.
        MeshConfiguration mesh{};
        mesh.inputPrimitive = 1;  // DI_PT_POINTLIST
        mesh.primitivesPerGroup = stride;
        mesh.verticesPerGroup = stride;
        mesh.maxVertices = request->max_vertices;
        mesh.maxPrimitives = request->max_primitives;
        mesh.threadsPerGroup = (lanes + wave - 1u) / wave * wave;
        mesh.ldsSizeDwords = (request->lds_bytes + 3u) / 4u;
        mesh.provokingVertex = 0;
        mesh.esgsItemSize = 1;
        mesh.outputPrimitiveSize = request->primitive_vertices;

        const std::uint32_t pushSize = request->push_size ? request->push_size : MeshDrawPushOffsetBytes - request->push_offset;
        if (request->push_offset + pushSize > MeshDrawPushOffsetBytes)
            throw std::runtime_error("a mesh stage's push data must end before its draw parameters (byte 104)");

        RecompileRequest compile{};
        compile.shader = {ShaderStage::Mesh, request->code_address,
                          std::span<const std::uint32_t>(request->code, request->code_dwords), 0, {}};
        compile.context.waveSize = wave;
        compile.context.userDataBaseRegister = 0;
        compile.context.userData = std::span<const std::uint32_t>(user);
        compile.context.vertex = vs;
        compile.context.memory = MemoryRegionView(std::span<const MemoryRegion>(regions));
        compile.context.floatMode = floatMode;
        compile.target = DefaultTarget(wave);
        // VK_EXT_mesh_shader's guaranteed limits (RADV gives more); AnyPS5 reads them only for
        // emulated barycentrics, which aren't asked for.
        compile.target.mesh = MeshTargetLimits{{128, 128, 128}, 128, 28672, 256, 256, 128, 32768, 1, 1};
        compile.layout = {0, 0, request->push_offset, pushSize};
        GraphicsCompileContext graphics{};
        graphics.firstUserSgpr = 0;
        graphics.mesh = mesh;
        graphics.draw = {0, 0, 0, 1};
        compile.graphics = graphics;
        compile.useCache = request->use_cache != 0;

        *out = new sa5_result(RecompileMemoized(compile, regions));
        return 0;
    } catch (const std::exception& e) {
        SetError(error, error_size, e.what());
        return -1;
    } catch (...) {
        SetError(error, error_size, "unknown exception");
        return -1;
    }
}

SA5_API std::uint32_t sa5_result_vertex_input_count(const sa5_result* result)
{
    return result ? static_cast<std::uint32_t>(result->result.vertexInputs.size()) : 0u;
}

SA5_API int sa5_result_vertex_input(const sa5_result* result, std::uint32_t index, std::uint32_t* location,
                                    std::uint32_t* components, std::uint32_t* fetch_index)
{
    if (!result || index >= result->result.vertexInputs.size())
        return -1;
    const auto& in = result->result.vertexInputs[index];
    if (location) *location = in.location;
    if (components) *components = in.components;
    if (fetch_index) *fetch_index = in.fetchIndex;
    return 0;
}

SA5_API const std::uint32_t* sa5_result_spirv(const sa5_result* result, std::uint32_t* word_count)
{
    if (word_count)
        *word_count = result ? static_cast<std::uint32_t>(const_cast<sa5_result*>(result)->Spirv().size()) : 0u;
    return result ? const_cast<sa5_result*>(result)->Spirv().data() : nullptr;
}

SA5_API std::uint32_t sa5_result_binding_count(const sa5_result* result)
{
    return result ? static_cast<std::uint32_t>(result->result.bindings.size()) : 0u;
}

SA5_API int sa5_result_binding(const sa5_result* result, std::uint32_t index, sa5_binding* binding)
{
    if (!result || !binding || index >= result->result.bindings.size())
        return -1;
    const auto& b = result->result.bindings[index];
    binding->kind = static_cast<std::uint32_t>(b.kind);
    binding->set = b.descriptorSet;
    binding->binding = b.binding;
    binding->count = b.count;
    binding->word_count = static_cast<std::uint32_t>(b.guestDescriptor.size());
    binding->words = b.guestDescriptor.data();
    binding->image_shape = b.imageShape ? static_cast<std::int32_t>(*b.imageShape) : -1;
    binding->role = static_cast<std::uint32_t>(b.role);
    return 0;
}

SA5_API const void* sa5_result_push_constants(const sa5_result* result, std::uint32_t* size)
{
    if (size)
        *size = result ? static_cast<std::uint32_t>(result->result.pushConstants.size()) : 0u;
    return result ? result->result.pushConstants.data() : nullptr;
}

SA5_API std::uint32_t sa5_result_subgroup_size(const sa5_result* result)
{
    return result ? result->result.hostSubgroupSize : 0u;
}

SA5_API void sa5_result_identity(const sa5_result* result, std::uint64_t identity[2])
{
    const auto id = result ? result->result.Identity() : PipelineIdentity{};
    identity[0] = id.low;
    identity[1] = id.high;
}

SA5_API std::uint32_t sa5_fault_buffer_bytes(void)
{
    return static_cast<std::uint32_t>(BdaAbi::FaultBufferBytes);
}

// Pm4::GdsBytes in AnyPS5's driver, which isn't built here.
SA5_API std::uint32_t sa5_gds_bytes(void)
{
    return 0x10000u;
}

// What AnyPS5 holds in memory, for d3d12_xs's memory log: its result memo (APS5_RESULT_MEMO_MB
// bounds it) and its resource plans.
SA5_API void sa5_memory_stats(std::uint64_t* memo_bytes, std::uint64_t* plan_bytes)
{
    *memo_bytes = ResultMemoBytes();
    *plan_bytes = ResourcePlanTotals().heldBytes;
}

SA5_API void sa5_result_free(sa5_result* result)
{
    delete result;
}

// The tessellation stages that draw a rect list with these vertex and pixel results (AnyPS5's
// BuildRectListShaders): each 3-vertex patch is a rectangle, its fourth corner made after the vertex
// shader. Limits are Vulkan's guaranteed minimums (RADV and lavapipe give more).
SA5_API int sa5_rect_list_stages(const sa5_result* vertex, const sa5_result* pixel, sa5_result** control,
                                 sa5_result** evaluation, char* error, size_t error_size)
{
    static constexpr std::array<std::uint32_t, 17> capabilities{
        spv::CapabilityInt64, spv::CapabilityPhysicalStorageBufferAddresses, spv::CapabilityStorageBuffer8BitAccess,
        spv::CapabilityClipDistance, spv::CapabilityInt64Atomics, spv::CapabilityMinLod, spv::CapabilityImageQuery,
        spv::CapabilityImageGatherExtended, spv::CapabilityDerivativeControl,
        spv::CapabilityStorageImageReadWithoutFormat, spv::CapabilityStorageImageWriteWithoutFormat,
        spv::CapabilityGroupNonUniformVote, spv::CapabilityGroupNonUniformArithmetic,
        spv::CapabilityGroupNonUniformBallot, spv::CapabilityGroupNonUniformShuffle, spv::CapabilityGroupNonUniformQuad,
        spv::CapabilityTessellation,
    };
    if (!vertex || !pixel || !control || !evaluation) {
        SetError(error, error_size, "null argument");
        return -1;
    }
    try {
        SpirvTarget target = DefaultTarget(32);
        target.supportedCapabilities = capabilities;
        target.tessellation = TessellationTargetLimits{32, 64, 64, 120, 2048, 64, 64};
        auto shaders = BuildRectListShaders(vertex->result, pixel->result, target);
        *control = new sa5_result(std::make_shared<const RecompileResult>(std::move(shaders.control)));
        *evaluation = new sa5_result(std::make_shared<const RecompileResult>(std::move(shaders.evaluation)));
        return 0;
    } catch (const std::exception& e) {
        SetError(error, error_size, e.what());
        return -1;
    }
}

}  // extern "C"
