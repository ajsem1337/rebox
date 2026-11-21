// rebox_anyps5.dll: see rebox_anyps5.h. GPL-2.0-only.
#define REBOX_ANYPS5_BUILD
#include "rebox_anyps5.h"

#include "BdaAbi.hpp"
#include "Recompiler.hpp"

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

// A conservative Vulkan 1.2-class target. TODO: fill it from vkd3d-proton's
// VkPhysicalDevice (subgroup size, limits, capabilities, extensions).
SpirvTarget DefaultTarget(std::uint32_t wave)
{
    static constexpr std::array<std::uint32_t, 5> capabilities{
        spv::CapabilityInt64, spv::CapabilityPhysicalStorageBufferAddresses, spv::CapabilityStorageBuffer8BitAccess,
        spv::CapabilityClipDistance, spv::CapabilityInt64Atomics,
    };
    static constexpr std::array<std::string_view, 4> extensions{
        "SPV_EXT_descriptor_indexing", "SPV_KHR_8bit_storage", "SPV_KHR_float_controls", "SPV_KHR_physical_storage_buffer",
    };
    return {0x00402000u, 0x00010500u, wave ? wave : 32u, BdaAbi::Version, capabilities, extensions, false,
            {1024, 1024, 64}, 1024, 65536, {}, {}};
}

void SetError(char* error, std::size_t size, const char* message)
{
    if (error && size)
        std::snprintf(error, size, "%s", message);
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

        auto* result = new sa5_result{Recompile(compile), {}};
        const auto words = result->result.spirv.Words();
        result->spirv.assign(words.begin(), words.end());
        *out = result;
        return 0;
    } catch (const std::exception& e) {
        SetError(error, error_size, e.what());
        return -1;
    } catch (...) {
        SetError(error, error_size, "unknown exception");
        return -1;
    }
}

SA5_API const std::uint32_t* sa5_result_spirv(const sa5_result* result, std::uint32_t* word_count)
{
    if (word_count)
        *word_count = result ? static_cast<std::uint32_t>(result->spirv.size()) : 0u;
    return result ? result->spirv.data() : nullptr;
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

SA5_API void sa5_result_free(sa5_result* result)
{
    delete result;
}

}  // extern "C"
