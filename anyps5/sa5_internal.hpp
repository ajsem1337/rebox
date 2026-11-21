// Shared between rebox_anyps5.cpp and spirv_remap.cpp. GPL-2.0-only.
#pragma once

#include "Recompiler.hpp"

#include <cstdint>
#include <vector>

struct sa5_result
{
    ShaderRecompiler::RecompileResult result;
    std::vector<std::uint32_t> spirv;
};
