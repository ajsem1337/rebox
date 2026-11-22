// Moved from xsx-shader-lab (rdna-run --analyse). GPL-2.0-only.
#include "layout.hpp"

#include "RdnaDecoder/RdnaInstructionDecoder.hpp"
#include <algorithm>
#include <optional>
#include <sstream>

namespace RdnaRun {

using ShaderRecompiler::RdnaInstruction;
using ShaderRecompiler::RdnaInstructionFamily;
using ShaderRecompiler::RdnaOpcode;
using ShaderRecompiler::RdnaOperand;
using ShaderRecompiler::RdnaOperandKind;

namespace {

constexpr std::uint32_t CodeEnd = 0xbf9f0000u;

std::optional<std::uint32_t> ScalarRegister(const RdnaOperand& operand) {
    switch (operand.kind) {
        case RdnaOperandKind::ScalarRegister: return operand.reg;
        case RdnaOperandKind::VccLo: return 106u;
        case RdnaOperandKind::VccHi: return 107u;
        case RdnaOperandKind::M0: return 124u;
        case RdnaOperandKind::ExecLo: return 126u;
        case RdnaOperandKind::ExecHi: return 127u;
        default: return std::nullopt;
    }
}

bool IsZero(const RdnaOperand& operand) {
    return (operand.kind == RdnaOperandKind::IntegerInlineConstant && operand.signedVal == 0) ||
           (operand.kind == RdnaOperandKind::LiteralConstant && operand.value == 0);
}

std::vector<RdnaInstruction> Decode(std::span<const std::uint32_t> code, std::set<std::string>& notes) {
    std::vector<RdnaInstruction> out;
    for (std::uint32_t at = 0; at < code.size();) {
        if (code[at] == CodeEnd) break;
        try {
            auto instruction = ShaderRecompiler::DecodeRdnaInstruction(at * 4u, code, at);
            at += instruction.wordCount != 0 ? instruction.wordCount : 1u;
            out.push_back(std::move(instruction));
        } catch (const std::exception&) {
            notes.insert("undecodable instruction");
            switch (ShaderRecompiler::GetRdnaInstructionFamily(code[at])) {
                case RdnaInstructionFamily::SOP1:
                case RdnaInstructionFamily::SOP2:
                case RdnaInstructionFamily::SOPK:
                case RdnaInstructionFamily::SOPC:
                case RdnaInstructionFamily::SOPP:
                case RdnaInstructionFamily::VOP1:
                case RdnaInstructionFamily::VOP2:
                case RdnaInstructionFamily::VOPC: at += 1; break;
                default: at += 2; break;
            }
        }
    }
    return out;
}

// gfx10 SOP1 opcodes that save or write a 64-bit exec mask.
bool IsExec64Sop1(std::uint32_t opcode) {
    return (opcode >= 36u && opcode <= 43u) || (opcode >= 55u && opcode <= 58u);
}

// The PSO metadata does not say whether a compute shader runs wave32 or wave64. Code that handles
// exec as 64 bits is the reliable wave64 signal.
std::uint32_t WaveOf(const std::vector<RdnaInstruction>& program) {
    for (const auto& instruction : program) {
        const bool sop = instruction.family == RdnaInstructionFamily::SOP1 || instruction.family == RdnaInstructionFamily::SOP2;
        if (!sop) continue;
        if (instruction.family == RdnaInstructionFamily::SOP1 && IsExec64Sop1(instruction.opcodeId)) return 64;
        if (instruction.dataDwordCount != 2) continue;
        if (instruction.destination.kind == RdnaOperandKind::ExecLo) return 64;
        if (instruction.op == RdnaOpcode::SMovB64 && instruction.source0.kind == RdnaOperandKind::ExecLo) return 64;
    }
    return 32;
}

// User SGPRs the shader treats as descriptor table pointers: copied next to a zero high half.
std::vector<std::uint32_t> TableSgprsOf(const std::vector<RdnaInstruction>& program, std::uint32_t userSgprs) {
    std::set<std::uint32_t> found;
    for (std::size_t i = 0; i + 1 < program.size(); ++i) {
        const auto& low = program[i];
        const auto& high = program[i + 1];
        if (low.op != RdnaOpcode::SMovB32 || high.op != RdnaOpcode::SMovB32) continue;
        if (low.destination.kind != RdnaOperandKind::ScalarRegister || low.source0.kind != RdnaOperandKind::ScalarRegister) continue;
        if (high.destination.kind != RdnaOperandKind::ScalarRegister || !IsZero(high.source0)) continue;
        if (high.destination.reg != low.destination.reg + 1u || low.source0.reg >= userSgprs) continue;
        found.insert(low.source0.reg);
    }
    if (found.empty()) return {userSgprs != 0 ? userSgprs - 1u : 0u};
    return {found.begin(), found.end()};
}

bool IsScalarLoad(std::uint32_t opcode) { return opcode <= 4u; }
bool IsScalarBufferLoad(std::uint32_t opcode) { return opcode >= 8u && opcode <= 12u; }

enum class ImageUse { Store, Atomic, Sample, Load, Bvh };

ImageUse ImageUseOf(std::uint32_t opcode) {
    if (opcode >= 0x08u && opcode <= 0x0bu) return ImageUse::Store;
    if (opcode >= 0x0fu && opcode <= 0x1fu) return ImageUse::Atomic;
    if ((opcode >= 0x20u && opcode <= 0x7fu) || (opcode >= 0xa2u && opcode <= 0xbfu)) return ImageUse::Sample;
    if (opcode == 0xe6u || opcode == 0xe7u) return ImageUse::Bvh;
    return ImageUse::Load;
}

const char* DimName(std::uint32_t dim) {
    static constexpr const char* names[] = {"1D", "2D", "3D", "CUBE", "1D_ARRAY", "2D_ARRAY", "2D_MSAA", "2D_MSAA_ARRAY"};
    return names[dim & 7u];
}

std::string BufferUseOf(const RdnaInstruction& instruction) {
    const auto op = instruction.opcodeId;
    if (instruction.family == RdnaInstructionFamily::MTBUF) return (op & 4u) != 0 ? "store" : "load";
    if ((op >= 4u && op <= 7u) || (op >= 24u && op <= 31u) || op == 39u || (op >= 132u && op <= 135u)) return "store";
    if (op >= 48u && op <= 94u) return "atomic";
    return "load";
}

bool BufferFormatted(const RdnaInstruction& instruction) {
    const auto op = instruction.opcodeId;
    return instruction.family == RdnaInstructionFamily::MTBUF || op <= 7u || op == 38u || op == 39u || (op >= 128u && op <= 135u);
}

bool IsBufferCacheControl(const RdnaInstruction& instruction) {
    return instruction.family == RdnaInstructionFamily::MUBUF && (instruction.opcodeId == 113u || instruction.opcodeId == 114u);
}

// gfx10 SOPK opcodes without a scalar destination: s_cmpk_*, s_setreg_b32, s_setreg_imm32_b32.
bool SopkWritesDestination(std::uint32_t opcode) {
    return !(opcode >= 3u && opcode <= 14u) && opcode != 19u && opcode != 21u;
}

class Analyser {
public:
    Analyser(Layout& layout, const std::map<std::uint32_t, std::uint32_t>& tables) : layout(layout), tables(tables) {}

    void Run(const std::vector<RdnaInstruction>& program) {
        for (const auto& instruction : program) Step(instruction);
    }

private:
    Layout& layout;
    const std::map<std::uint32_t, std::uint32_t>& tables;
    std::map<std::uint32_t, std::uint32_t> tablePointer; // SGPR with a table pointer's low half -> table base dword
    std::map<std::uint32_t, std::uint32_t> slotOf;       // SGPR -> table dword it was loaded from

    void Forget(std::uint32_t reg, std::uint32_t count, bool pointers) {
        for (std::uint32_t i = 0; i < count; ++i) {
            slotOf.erase(reg + i);
            if (pointers) tablePointer.erase(reg + i);
        }
    }

    Slot* Resource(std::uint32_t reg, std::uint32_t size, const std::string& kind, const std::string& use) {
        const auto first = slotOf.find(reg);
        if (first == slotOf.end()) {
            layout.notes.insert("unresolved " + kind);
            return nullptr;
        }
        for (std::uint32_t i = 0; i < size; ++i) {
            const auto it = slotOf.find(reg + i);
            if (it == slotOf.end() || it->second != first->second + i) {
                layout.notes.insert("split " + kind);
                return nullptr;
            }
        }
        auto [it, inserted] = layout.slots.try_emplace(first->second);
        auto& slot = it->second;
        if (inserted) {
            slot.kind = kind;
            slot.size = size;
        } else if (slot.kind != kind) {
            const std::set<std::string> images{"texture", "storage"};
            if (images.contains(slot.kind) && images.contains(kind)) slot.kind = "storage";
            else layout.notes.insert("slot " + std::to_string(first->second) + " used as " + slot.kind + " and " + kind);
        }
        slot.uses.insert(use);
        return &slot;
    }

    void Step(const RdnaInstruction& instruction) {
        const auto family = instruction.family;
        if (instruction.op == RdnaOpcode::SMovB32 && instruction.destination.kind == RdnaOperandKind::ScalarRegister &&
            instruction.source0.kind == RdnaOperandKind::ScalarRegister) {
            if (const auto table = tables.find(instruction.source0.reg); table != tables.end()) {
                tablePointer[instruction.destination.reg] = table->second;
                return;
            }
        }
        if (family == RdnaInstructionFamily::SMEM) {
            ScalarMemory(instruction);
            return;
        }
        if (family == RdnaInstructionFamily::MIMG) {
            Image(instruction);
            return;
        }
        if (family == RdnaInstructionFamily::MUBUF || family == RdnaInstructionFamily::MTBUF) {
            if (IsBufferCacheControl(instruction)) return;
            if (const auto reg = ScalarRegister(instruction.source1)) {
                if (auto* slot = Resource(*reg, 4, "buffer", BufferUseOf(instruction)); slot != nullptr && !slot->hasBufferFlags) {
                    slot->hasBufferFlags = true;
                    slot->idxen = instruction.idxen;
                    slot->formatted = BufferFormatted(instruction);
                }
            }
            return;
        }
        if (family == RdnaInstructionFamily::FLAT) layout.notes.insert("flat/global memory");
        const bool scalarAlu = family == RdnaInstructionFamily::SOP1 || family == RdnaInstructionFamily::SOP2 ||
                               (family == RdnaInstructionFamily::SOPK && SopkWritesDestination(instruction.opcodeId));
        if (scalarAlu) {
            const auto reg = ScalarRegister(instruction.destination);
            if (!reg) return;
            const auto size = instruction.dataDwordCount != 0 ? instruction.dataDwordCount : 1u;
            // `s_and_b32 s15, s15, ...` patches a descriptor in place (a structured buffer turned
            // raw): the register still holds the same table slot.
            const bool inPlace = size == 1 && slotOf.contains(*reg) &&
                                 (ScalarRegister(instruction.source0) == reg || ScalarRegister(instruction.source1) == reg);
            if (!inPlace) Forget(*reg, size, true);
            return;
        }
        if (instruction.op == RdnaOpcode::VReadlaneB32 || instruction.op == RdnaOpcode::VReadfirstlaneB32) {
            if (const auto reg = ScalarRegister(instruction.destination)) Forget(*reg, 1, false);
        }
    }

    void ScalarMemory(const RdnaInstruction& instruction) {
        const auto dst = ScalarRegister(instruction.destination);
        const auto base = ScalarRegister(instruction.source0);
        const auto count = instruction.dataDwordCount != 0 ? instruction.dataDwordCount : 1u;
        if (IsScalarBufferLoad(instruction.opcodeId)) {
            if (auto* slot = base ? Resource(*base, 4, "cbuffer", "cbuffer") : nullptr; slot != nullptr && slot->kind == "cbuffer") {
                if (instruction.source1.kind != RdnaOperandKind::Null) slot->dynamicReads = true;
                else {
                    auto& count = slot->reads[instruction.memoryOffset / 4u];
                    count = std::max(count, instruction.dataDwordCount != 0 ? instruction.dataDwordCount : 1u);
                }
            }
            if (dst) Forget(*dst, count, false);
            return;
        }
        if (!IsScalarLoad(instruction.opcodeId)) {
            if (dst) Forget(*dst, count, true);
            return;
        }
        if (!dst || !base) return;
        Forget(*dst, count, false);
        const auto pointer = tablePointer.find(*base);
        if (pointer == tablePointer.end()) {
            layout.notes.insert("indirect s_load");
            return;
        }
        if (instruction.source1.kind != RdnaOperandKind::Null) {
            layout.notes.insert("dynamic table offset");
            return;
        }
        const auto first = pointer->second + instruction.memoryOffset / 4u;
        for (std::uint32_t i = 0; i < count; ++i) slotOf[*dst + i] = first + i;
        layout.maxDword = std::max(layout.maxDword, first + count);
    }

    void Image(const RdnaInstruction& instruction) {
        const auto resource = ScalarRegister(instruction.source1);
        if (!resource) return;
        const auto use = ImageUseOf(instruction.opcodeId);
        if (use == ImageUse::Bvh) {
            layout.notes.insert("ray tracing");
            return;
        }
        const auto size = instruction.imageR128 ? 4u : 8u;
        const char* kind = use == ImageUse::Store || use == ImageUse::Atomic ? "storage" : "texture";
        const char* name = use == ImageUse::Store ? "store" : use == ImageUse::Atomic ? "atomic" : use == ImageUse::Sample ? "sample" : "load";
        if (auto* slot = Resource(*resource, size, kind, name); slot != nullptr && slot->dim.empty()) {
            slot->dim = DimName((instruction.rawWords[0] >> 3u) & 7u);
        }
        if (use == ImageUse::Sample) {
            if (const auto sampler = ScalarRegister(instruction.source2); sampler && slotOf.contains(*sampler)) {
                Resource(*sampler, 4, "sampler", "sampler");
            }
        }
    }
};

void AppendString(std::ostringstream& out, const std::string& text) {
    out << '"';
    for (const char c : text) {
        if (c == '"' || c == '\\') out << '\\';
        out << c;
    }
    out << '"';
}

}

std::vector<std::uint32_t> RootConstantsOf(const std::vector<RdnaInstruction>& program, std::uint32_t userSgprs, const std::vector<std::uint32_t>& tables) {
    std::set<std::uint32_t> written;
    std::set<std::uint32_t> read;
    for (const auto& instruction : program) {
        const RdnaOperand* sources[] = {&instruction.source0, &instruction.source1, &instruction.source2, &instruction.source3};
        for (const auto* source : sources) {
            // an image instruction's sampler field is only an operand when it samples
            if (instruction.family == RdnaInstructionFamily::MIMG && source == &instruction.source2 && !instruction.imageSampler) continue;
            if (source->kind != RdnaOperandKind::ScalarRegister || source->reg >= userSgprs || written.contains(source->reg)) continue;
            if (std::find(tables.begin(), tables.end(), source->reg) == tables.end()) read.insert(source->reg);
        }
        for (const auto* destination : {&instruction.destination, &instruction.destination2}) {
            if (destination->kind != RdnaOperandKind::ScalarRegister) continue;
            const auto count = instruction.family == RdnaInstructionFamily::SMEM || instruction.family == RdnaInstructionFamily::SOP1 || instruction.family == RdnaInstructionFamily::SOP2
                                   ? std::max(1u, instruction.dataDwordCount)
                                   : 1u;
            for (std::uint32_t i = 0; i < count; ++i) written.insert(destination->reg + i);
        }
    }
    return {read.begin(), read.end()};
}

Layout Analyse(std::span<const std::uint32_t> code, std::uint32_t userSgprs) {
    Layout layout;
    const auto program = Decode(code, layout.notes);
    layout.wave = WaveOf(program);
    layout.tableSgprs = TableSgprsOf(program, userSgprs);
    std::map<std::uint32_t, std::uint32_t> tables;
    for (std::size_t i = 0; i < layout.tableSgprs.size(); ++i) tables[layout.tableSgprs[i]] = static_cast<std::uint32_t>(i) * TableSpan;
    Analyser(layout, tables).Run(program);
    layout.rootConstants = RootConstantsOf(program, userSgprs, layout.tableSgprs);
    return layout;
}

std::string ToJson(const Layout& layout) {
    std::ostringstream out;
    out << "{\"wave\":" << layout.wave << ",\"table_sgprs\":[";
    for (std::size_t i = 0; i < layout.tableSgprs.size(); ++i) out << (i ? "," : "") << layout.tableSgprs[i];
    out << "],\"root_constants\":[";
    for (std::size_t i = 0; i < layout.rootConstants.size(); ++i) out << (i ? "," : "") << layout.rootConstants[i];
    out << "],\"max_dword\":" << layout.maxDword << ",\"slots\":{";
    bool firstSlot = true;
    for (const auto& [dword, slot] : layout.slots) {
        out << (firstSlot ? "" : ",") << '"' << dword << "\":{\"kind\":";
        firstSlot = false;
        AppendString(out, slot.kind);
        out << ",\"size\":" << slot.size << ",\"uses\":[";
        bool firstUse = true;
        for (const auto& use : slot.uses) {
            out << (firstUse ? "" : ",");
            firstUse = false;
            AppendString(out, use);
        }
        out << "]";
        if (!slot.dim.empty()) {
            out << ",\"dim\":";
            AppendString(out, slot.dim);
        }
        if (slot.kind == "cbuffer") {
            out << ",\"reads\":[";
            bool firstRead = true;
            for (const auto& [offset, count] : slot.reads) {
                out << (firstRead ? "" : ",") << "[" << offset << "," << count << "]";
                firstRead = false;
            }
            out << "],\"dynamic\":" << (slot.dynamicReads ? "true" : "false");
        }
        if (slot.hasBufferFlags) out << ",\"idxen\":" << (slot.idxen ? "true" : "false") << ",\"formatted\":" << (slot.formatted ? "true" : "false");
        out << "}";
    }
    out << "},\"notes\":[";
    bool firstNote = true;
    for (const auto& note : layout.notes) {
        out << (firstNote ? "" : ",");
        firstNote = false;
        AppendString(out, note);
    }
    out << "]}";
    return out.str();
}

}
