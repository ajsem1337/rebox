// Shared between rebox_anyps5.cpp and spirv_remap.cpp. GPL-2.0-only.
#pragma once

#include "Recompiler.hpp"

#include <cstdint>
#include <memory>
#include <vector>

// A result is shared with AnyPS5's result memo (graphics stages reuse one across draws whose
// captured resources match), so it is held by pointer; its SPIR-V words are made only when asked
// for (a new module), not copied at every draw.
struct sa5_result
{
    std::shared_ptr<const ShaderRecompiler::RecompileResult> held;
    const ShaderRecompiler::RecompileResult& result;
    std::vector<std::uint32_t> spirv;

    explicit sa5_result(std::shared_ptr<const ShaderRecompiler::RecompileResult> r) : held(std::move(r)), result(*held) {}

    const std::vector<std::uint32_t>& Spirv()
    {
        if (spirv.empty()) {
            const auto words = result.spirv.Words();
            spirv.assign(words.begin(), words.end());
        }
        return spirv;
    }
};
