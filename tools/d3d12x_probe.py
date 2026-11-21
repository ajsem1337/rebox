#!/usr/bin/env python3
"""Recover D3D12.X interface usage from console binaries.

    d3d12x_probe.py iids BIN...
    d3d12x_probe.py slots BIN [--entry ADDR:REG[,REG]]...

`iids` lists every GUID a function loads by RIP-relative address, with its
mingw-header name and the vtable offset of the next indirect call.

`slots` starts from a parameter whose type we know, follows it through
register and stack copies (and into direct callees, up to a small depth), and
reports:
  - vtable slots called on it, next to the PC method in that slot, and
  - fields of the object read or written directly. D3D12.X headers inline some
    methods, so title code touches runtime object internals.
With no --entry, exported C++ functions with ID3D12* parameters are used, the
parameter register taken from the mangled name.

The tracking is flow-insensitive and per-function: it can miss paths and can
over-approximate within a function. Check results by disassembling the sites
it prints (`d3d12x_probe.py dis BIN ADDR`).

GUID and vtable names come from the mingw headers: set MINGW_INCLUDE, or have
x86_64-w64-mingw32-gcc on PATH.
"""

import argparse
import collections
import functools
import os
import pathlib
import re
import struct
import subprocess
import sys
import uuid

import pefile
from capstone import CS_ARCH_X86, CS_MODE_64, Cs
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG, X86_REG_RIP

VOLATILE = {"rax", "rcx", "rdx", "r8", "r9", "r10", "r11"}
ARG_REGS = ("rcx", "rdx", "r8", "r9")
MAX_DEPTH = 6

R64 = {}
for _n in ("ax", "bx", "cx", "dx", "si", "di", "bp", "sp"):
    for _v in ("r" + _n, "e" + _n, _n):
        R64[_v] = "r" + _n
for _k in range(8, 16):
    for _v in (f"r{_k}", f"r{_k}d", f"r{_k}w", f"r{_k}b"):
        R64[_v] = f"r{_k}"


# ---------------------------------------------------------------- headers

def mingw_include():
    if os.environ.get("MINGW_INCLUDE"):
        return pathlib.Path(os.environ["MINGW_INCLUDE"])
    cc = os.environ.get("MINGW_CC", "x86_64-w64-mingw32-gcc")
    try:
        deps = subprocess.run([cc, "-xc", "-M", "-"], input="#include <d3d12.h>\n", text=True,
                              capture_output=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return None
    for tok in deps.split():
        if tok.endswith("/d3d12.h"):
            return pathlib.Path(tok).parent
    return None


@functools.lru_cache(None)
def header_tables():
    """(guid bytes -> name, interface -> [method names in vtable order])"""
    inc = mingw_include()
    guids, vtables = {}, {}
    if not inc:
        print("warning: no mingw headers found, names unavailable", file=sys.stderr)
        return guids, vtables
    for h in sorted(inc.glob("d3d12*.h")) + sorted(inc.glob("dxgi*.h")) + [inc / "unknwn.h"]:
        text = h.read_text(errors="ignore")
        for m in re.finditer(r"DEFINE_GUID\(\s*(\w+)\s*,\s*([^)]*)\)", text):
            try:
                vals = [int(x.strip().rstrip("uUlL"), 0) for x in m.group(2).split(",")]
                guids.setdefault(struct.pack("<IHH8B", *vals), m.group(1))
            except (ValueError, struct.error):
                pass
        for m in re.finditer(r"typedef struct (\w+)Vtbl \{(.*?)\} \1Vtbl;", text, re.S):
            vtables[m.group(1)] = re.findall(r"\(STDMETHODCALLTYPE \*(\w+)\)", m.group(2))
    return guids, vtables


def guid_str(b):
    return "{" + str(uuid.UUID(bytes_le=b)) + "}"


# ---------------------------------------------------------------- image

class Image:
    def __init__(self, path):
        self.pe = pe = pefile.PE(str(path))
        self.base = pe.OPTIONAL_HEADER.ImageBase
        self.mem = pe.get_memory_mapped_image()
        self.md = Cs(CS_ARCH_X86, CS_MODE_64)
        self.md.detail = True
        # .pdata gives function bounds; linear sweep over whole sections stops
        # at the first byte capstone can't decode.
        self.funcs = {}
        for e in getattr(pe, "DIRECTORY_ENTRY_EXCEPTION", []):
            self.funcs[self.base + e.struct.BeginAddress] = self.base + e.struct.EndAddress
        self.imports = {}
        for entry in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
            for imp in entry.imports:
                self.imports[imp.address] = f"{entry.dll.decode()}!{imp.name.decode() if imp.name else imp.ordinal}"

    @functools.lru_cache(None)
    def insns(self, start):
        end = self.funcs[start]
        return list(self.md.disasm(bytes(self.mem[start - self.base:end - self.base]), start))

    def containing(self, va):
        best = max((s for s in self.funcs if s <= va), default=None)
        return best if best is not None and va < self.funcs[best] else None

    def exports(self):
        d = getattr(self.pe, "DIRECTORY_ENTRY_EXPORT", None)
        for sym in getattr(d, "symbols", []):
            if sym.name:
                yield self.base + sym.address, sym.name.decode()

    def rip_target(self, ins, op):
        return ins.address + ins.size + op.mem.disp


def reg(ins, r):
    return R64.get(ins.reg_name(r)) if r else None


def location(ins, op):
    """Register or stack slot an operand names, else None."""
    if op.type == X86_OP_REG:
        return reg(ins, op.reg)
    if op.type == X86_OP_MEM and not op.mem.index and reg(ins, op.mem.base) in ("rsp", "rbp"):
        return f"[{reg(ins, op.mem.base)}{op.mem.disp:+#x}]"
    return None


# ---------------------------------------------------------------- tracking

class Tracker:
    """Follows one object pointer and records what is done with it."""

    def __init__(self, img):
        self.img = img
        self.slots = collections.defaultdict(list)    # vtable offset -> call sites
        self.fields = collections.defaultdict(list)   # (offset, 'R'/'W') -> sites
        self.seen = set()

    def run(self, func, regs, depth=0):
        key = (func, frozenset(regs))
        if key in self.seen or depth > MAX_DEPTH or func not in self.img.funcs:
            return
        self.seen.add(key)
        obj, vtbl, method = set(regs), set(), {}

        def kill(loc):
            obj.discard(loc)
            vtbl.discard(loc)
            method.pop(loc, None)

        def follow(target):
            args = {r for r in ARG_REGS if r in obj}
            if args and target != func:
                self.run(target, args, depth + 1)

        for ins in self.img.insns(func):
            ops = ins.operands
            mn = ins.mnemonic

            # Direct field access on the object (not the vtable pointer).
            for i, op in enumerate(ops):
                if op.type == X86_OP_MEM and reg(ins, op.mem.base) in obj and op.mem.disp and mn != "lea":
                    write = i == 0 and len(ops) == 2 and not mn.startswith(("cmp", "test"))
                    self.fields[(op.mem.disp, "W" if write else "R")].append(ins.address)

            if mn in ("call", "jmp"):
                op = ops[0]
                if op.type == X86_OP_MEM and reg(ins, op.mem.base) in vtbl:
                    self.slots[op.mem.disp].append(ins.address)
                elif op.type == X86_OP_REG and reg(ins, op.reg) in method:
                    self.slots[method[reg(ins, op.reg)]].append(ins.address)
                elif op.type == X86_OP_IMM and op.imm in self.img.funcs:
                    if mn == "call" or op.imm != func:
                        follow(op.imm)
                if mn == "call":
                    for r in VOLATILE:
                        kill(r)
                continue

            if mn == "mov" and len(ops) == 2:
                dst, src = location(ins, ops[0]), ops[1]
                src_loc = location(ins, src)
                new = None
                if src_loc in obj:
                    new = "obj"
                elif src_loc in vtbl:
                    new = "vtbl"
                elif src.type == X86_OP_MEM and not src.mem.index:
                    base = reg(ins, src.mem.base)
                    if base in obj and src.mem.disp == 0:
                        new = "vtbl"
                    elif base in vtbl:
                        new = ("method", src.mem.disp)
                kill(dst)
                if dst and new == "obj":
                    obj.add(dst)
                elif dst and new == "vtbl":
                    vtbl.add(dst)
                elif dst and new:
                    method[dst] = new[1]
            elif ops and ops[0].type == X86_OP_REG and mn not in ("cmp", "test", "push"):
                kill(location(ins, ops[0]))


# ---------------------------------------------------------------- commands

MANGLED_IFACE = re.compile(r"PE(?:A|B)UID3D12(\w+?)@@")


def export_entries(img):
    """(va, name, interface, register) for each exported ID3D12* parameter."""
    for va, name in img.exports():
        if not name.startswith("?"):
            continue
        # ?Method@Class@@QEAA<ret><params>@Z: member functions take `this` in rcx.
        m = re.match(r"\?\w+@\w+@@(Q|U|A)EAA", name) or re.match(r"\?\?[0-9](\w+)@@QEAA@", name)
        if not m:
            continue
        params = name[m.end():]
        if not name.startswith("??"):
            params = params[1:]  # return type, one character for the cases seen
        # Count parameters up to each interface pointer; good enough for the
        # simple signatures these DLLs export.
        idx = 1
        pos = 0
        for tok in re.finditer(r"PE(?:A|B)U\w+?@@|_K|_N|[A-Z]", params):
            if tok.start() < pos:
                continue
            iface = MANGLED_IFACE.match(tok.group())
            if iface and idx < len(ARG_REGS):
                yield va, name, "ID3D12" + iface.group(1), ARG_REGS[idx]
            idx += 1
            pos = tok.end()


def pc_method(vtables, iface, offset):
    methods = vtables.get(iface)
    if methods is None:
        # Newer than the headers: later versions only append, so use the
        # newest version of the same interface family we have.
        family = re.sub(r"\d+$", "", iface)
        same = [v for k, v in vtables.items() if re.fullmatch(re.escape(family) + r"\d*", k)]
        methods = max(same, key=len, default=[])
    slot = offset // 8
    return methods[slot] if slot < len(methods) else f"(past end of PC {iface}, {len(methods)} slots)"


def cmd_slots(args):
    _, vtables = header_tables()
    img = Image(args.binary)
    groups = collections.defaultdict(list)
    if args.entry:
        for spec in args.entry:
            addr, regs = spec.split(":")
            groups[args.interface].append((int(addr, 16), regs.split(","), spec))
    else:
        for va, name, iface, r in export_entries(img):
            groups[iface].append((va, [r], name))
    for iface, entries in groups.items():
        t = Tracker(img)
        for va, regs, _ in entries:
            t.run(va, regs)
        print(f"== {iface} ({len(entries)} entry point{'s' * (len(entries) != 1)})")
        for _, _, label in entries:
            print(f"   from {label}")
        for off in sorted(t.slots):
            sites = sorted(set(t.slots[off]))
            print(f"   slot {off // 8:3d} (+{off:#05x})  PC: {pc_method(vtables, iface, off):40s} "
                  f"x{len(sites)}  {' '.join(hex(s) for s in sites[:3])}")
        if t.fields:
            print("   fields accessed directly:")
            for (off, rw), sites in sorted(t.fields.items()):
                sites = sorted(set(sites))
                print(f"     +{off:#06x} {rw} x{len(sites)}  {' '.join(hex(s) for s in sites[:3])}")


def cmd_iids(args):
    guids, _ = header_tables()
    for path in args.binaries:
        img = Image(path)
        hits = collections.defaultdict(list)
        for func in img.funcs:
            insns = img.insns(func)
            for i, ins in enumerate(insns):
                for op in ins.operands:
                    if op.type != X86_OP_MEM or op.mem.base != X86_REG_RIP:
                        continue
                    off = img.rip_target(ins, op) - img.base
                    if not 0 <= off <= len(img.mem) - 16:
                        continue
                    b = bytes(img.mem[off:off + 16])
                    name = guids.get(b)
                    slot = next((j.operands[0].mem.disp for j in insns[i + 1:i + 20]
                                 if j.mnemonic == "call" and j.operands[0].type == X86_OP_MEM
                                 and j.operands[0].mem.base not in (0, X86_REG_RIP)), None)
                    # Unnamed: only RFC 4122 variant GUIDs loaded with lea right
                    # before a vtable call, to keep noise down.
                    plausible = (ins.mnemonic == "lea" and slot is not None and b[8] & 0xC0 == 0x80
                                 and b[7] >> 4 in (1, 3, 4, 5))
                    if (name and not name.startswith("WPD_") and b != bytes(16)) or (not name and plausible):
                        hits[b].append((ins.address, slot))
        print(f"== {path}")
        for b, uses in hits.items():
            sites = " ".join(f"{a:#x}" + (f"->+{s:#x}" if s is not None else "") for a, s in uses[:4])
            print(f"   {guid_str(b)} {guids.get(b, 'UNKNOWN'):45s} {sites}")


def cmd_dis(args):
    img = Image(args.binary)
    va = int(args.address, 16)
    func = img.containing(va)
    if func is None:
        sys.exit(f"{va:#x} is not inside a .pdata function")
    insns = img.insns(func)
    idx = next(i for i, x in enumerate(insns) if x.address >= va)
    for x in insns[max(0, idx - args.before):idx + args.after]:
        note = ""
        for op in x.operands:
            if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
                note = img.imports.get(img.rip_target(x, op), "")
        print(f"{'>>' if x.address == va else '  '} {x.address:x}: {x.mnemonic} {x.op_str}"
              + (f"    ; {note}" if note else ""))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("iids", help="GUIDs referenced by code")
    p.add_argument("binaries", nargs="+")
    p.set_defaults(fn=cmd_iids)
    p = sub.add_parser("slots", help="vtable slots and fields used on a known-type object")
    p.add_argument("binary")
    p.add_argument("--entry", action="append", help="FUNC_VA:REG[,REG] holding the object at entry")
    p.add_argument("--interface", default="ID3D12GraphicsCommandList10",
                   help="PC interface to name slots against with --entry")
    p.set_defaults(fn=cmd_slots)
    p = sub.add_parser("dis", help="disassemble around an address")
    p.add_argument("binary")
    p.add_argument("address")
    p.add_argument("-B", "--before", type=int, default=25)
    p.add_argument("-A", "--after", type=int, default=5)
    p.set_defaults(fn=cmd_dis)
    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
