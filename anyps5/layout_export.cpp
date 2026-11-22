// sa5_analyse: layout.cpp's analysis behind the C API. GPL-2.0-only.
#define REBOX_ANYPS5_BUILD
#include "rebox_anyps5.h"

#include "layout.hpp"

#include <cstdio>
#include <cstring>
#include <exception>

extern "C" SA5_API int sa5_analyse(const uint32_t* code, uint32_t code_dwords, uint32_t user_sgprs, sa5_layout* layout,
                                   sa5_slot* slots, uint32_t slot_capacity, char* error, size_t error_size)
{
    static const char* const dims[] = {"", "1D", "2D", "3D", "CUBE", "1D_ARRAY", "2D_ARRAY", "2D_MSAA", "2D_MSAA_ARRAY"};
    static const char* const kinds[] = {"texture", "storage", "sampler", "buffer", "cbuffer"};
    try {
        const auto l = RdnaRun::Analyse(std::span<const std::uint32_t>(code, code_dwords), user_sgprs);
        std::memset(layout, 0, sizeof(*layout));
        layout->wave = l.wave;
        for (const auto sgpr : l.tableSgprs)
            if (layout->table_count < 16) layout->table_sgprs[layout->table_count++] = sgpr;
        for (const auto& [dword, slot] : l.slots) {
            if (layout->slot_count >= slot_capacity) break;
            sa5_slot& s = slots[layout->slot_count++];
            std::memset(&s, 0, sizeof(s));
            s.table = dword / RdnaRun::TableSpan;
            s.dword = dword % RdnaRun::TableSpan;
            for (std::uint32_t k = 0; k < 5; k++)
                if (slot.kind == kinds[k]) s.kind = k;
            for (std::uint32_t d = 1; d < 9; d++)
                if (slot.dim == dims[d]) s.dim = d;
            s.size = slot.size;
            s.stored = slot.uses.count("store") || slot.uses.count("atomic");
            s.formatted = slot.formatted;
            s.indexed = slot.idxen;
        }
        for (const auto& note : l.notes) {
            if (note.rfind("unresolved", 0) == 0) layout->notes |= 1u;
            if (note == "dynamic table offset") layout->notes |= 2u;
            if (note == "undecodable instruction") layout->notes |= 4u;
        }
        return 0;
    } catch (const std::exception& e) {
        if (error && error_size) std::snprintf(error, error_size, "%s", e.what());
        return -1;
    }
}
