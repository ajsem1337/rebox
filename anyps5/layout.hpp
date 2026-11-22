// Moved from xsx-shader-lab (rdna-run --analyse). GPL-2.0-only.
// Descriptor table layout of an RDNA2 compute shader, inferred from its code.
//
// Scarlett compute shaders receive descriptor tables as 32-bit pointers in user SGPRs: the shader
// forms a 64-bit address as {sU, 0} and loads T#, S#, V# and constant buffer descriptors from fixed
// offsets in the table. Following those loads to the instructions that consume the loaded
// registers tells which table dword holds which kind of resource.
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace RdnaRun {

// Dwords reserved per descriptor table when several tables share one block of memory.
inline constexpr std::uint32_t TableSpan = 4096;

struct Slot {
    std::string kind; // texture, storage, sampler, buffer, cbuffer
    std::uint32_t size = 0;
    std::set<std::string> uses; // sample, load, store, atomic, cbuffer, sampler
    std::string dim;            // images: 1D, 2D, 3D, CUBE, 1D_ARRAY, 2D_ARRAY, 2D_MSAA, 2D_MSAA_ARRAY
    bool idxen = false;         // buffers: accessed with an index (structured)
    bool formatted = false;     // buffers: typed access
    bool hasBufferFlags = false;
    std::map<std::uint32_t, std::uint32_t> reads; // constant buffers: dword offset -> dwords read there
    bool dynamicReads = false;                    // constant buffers: also read at offsets computed at run time
};

struct Layout {
    std::uint32_t wave = 32;
    std::vector<std::uint32_t> tableSgprs;
    std::map<std::uint32_t, Slot> slots; // table dword -> resource
    std::set<std::string> notes;
    std::uint32_t maxDword = 0;
    std::vector<std::uint32_t> rootConstants; // user SGPRs read as values (not table pointers)
};

// `code` starts at the first instruction (no ID prologue).
Layout Analyse(std::span<const std::uint32_t> code, std::uint32_t userSgprs);

std::string ToJson(const Layout& layout);

}
