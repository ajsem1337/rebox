#!/usr/bin/env python3
# Pixel plan for `sa5_check ps` (docs/anyps5.md) from REBOX_DUMP_SHADERS pixel containers (DXBC > DXIL >
# XBCH + zlib > FSSM), with the register state shims/d3d12_xs/console_fssm.c gives a pixel stage:
#   ps-plan.py DUMP_DIR OUT_DIR [--draw] [ID|NAME.ps.bin...] > PLAN
# writes each shader's code to OUT_DIR (its FLAT_SCRATCH setup made s_nops, as console_compile.c does)
# and a plan line per shader to stdout, named by the id d3d12_xs logs it under ("console shader ID"):
# FNV-1a of the code. IDs pick shaders by that id; with none, every *.ps.bin.
#
# --draw also lays out guest memory as a draw gives it: each user SGPR the FSSM's resource records
# name gets a table (at 0x20000000 + 0x4000 * SGPR, in OUT_DIR/ID.mem), each record's slot a
# descriptor of its kind (a T#, V# or S#; V#s name 4 KiB of zeros after the tables), and the line
# gets "u=" and "m=" fields. MEM_T=w0,...,w7
# and MEM_V=w0,...,w3 (hex) put those words in every T# or V# slot instead: what a draw's tables
# held when it was refused ("runtime image descriptor is invalid: ... T# ...").
import os, struct, sys, zlib

u32 = lambda b, o: struct.unpack_from('<I', b, o)[0]
TABLES = 0x20000000
# Zeros after the tables, which the V#s below name: a shader that reads a pointer through one reads 0.
BUFFERS = 0x80000
# A 2D 8_8_8_8 texture, a raw buffer, a point sampler: what a slot holds when the draw is right.
GOOD_T = [0x002e0000, 0xc3800080, 0x021bc3bf, 0x90000fac, 0, 0, 0, 0]
GOOD_V = [TABLES + BUFFERS, 0, 0x1000, (77 << 12) | 0xfac]
GOOD_S = [0, 0, 0, 0]


def fssm_of(data):
    start = data.find(b'XBCH')
    for z in range(max(start, 0), len(data) - 2):
        if start < 0:
            break
        if data[z] == 0x78 and ((data[z] << 8) | data[z + 1]) % 31 == 0:
            try:
                out = zlib.decompressobj().decompress(data[z:])
            except zlib.error:
                continue
            if out[:4] == b'FSSM':
                return out
    return None


def fnv(data, h=0xcbf29ce484222325):
    for x in data:
        h = ((h ^ x) * 0x100000001b3) & 0xffffffffffffffff
    return h


def size(code, i):
    """GFX10 instruction size in dwords (console_fssm.c's gfx10_size)."""
    w = code[i]
    top9, top6 = w >> 23, w >> 26
    lit = lambda x: x == 0xff
    if top9 == 0x17f:
        return 1
    if top9 == 0x17d:
        return 1 + lit(w & 0xff)
    if top9 == 0x17e:
        return 1 + (lit(w & 0xff) or lit((w >> 8) & 0xff))
    if w >> 28 == 0xb:
        return 1 + (((w >> 23) & 0x1f) == 0x15)
    if w >> 30 == 2:
        return 1 + (lit(w & 0xff) or lit((w >> 8) & 0xff))
    if top6 in (0x35, 0x33):
        s = code[i + 1] if i + 1 < len(code) else 0
        return 2 + ((s & 0x1ff) == 0xff or ((s >> 9) & 0x1ff) == 0xff or ((s >> 18) & 0x1ff) == 0xff)
    if top6 in (0x3d, 0x36, 0x3a, 0x38, 0x37, 0x3e):
        return 2
    if top6 == 0x3c:
        n = (w >> 1) & 3
        return 2 + ((n * 4 + 3) // 4 if n else 0)
    if top6 == 0x32:
        return 1
    return 1 + ((w & 0x1ff) in (0xff, 0xf9, 0xfa, 0xe9, 0xea))


def nop_flat_scratch(code):
    i = scanned = 0
    while i < len(code) and scanned < 32:
        scanned += 1
        w = code[i]
        op = w >> 23
        if op in (0x175, 0x173) and (w & 0x3f) in (20, 21):
            code[i] = 0xbf800000
            i += 1
            if op == 0x175 and i < len(code):
                code[i] = 0xbf800000
                i += 1
        elif op in (0x17d, 0x17e) or w >> 28 == 0xb or (w >> 30 == 2 and op != 0x17f) or w >> 16 in (0xbfa0, 0xbf80):
            i += size(code, i)
        else:
            break


def pixel_state(fssm, code):
    """(rsrc1, rsrc2, ena, interpolants, settings, col_format, depth) as console_fssm_graphics makes them."""
    user, used, flat_only, ninterp, col, depth = None, 0, 0, 0, 0, 0
    i = 0
    while i < len(code):
        w, n = code[i], size(code, i)
        if user is None and w >> 23 == 0x17d and ((w >> 16) & 0x7f) == 0x7c and ((w >> 8) & 0xff) == 3 and (w & 0xff) < 106:
            user = w & 0xff
        if w >> 26 == 0x32:
            attr, op = (w >> 10) & 0x3f, (w >> 16) & 3
            if attr < 32:
                if not used & (1 << attr):
                    flat_only |= 1 << attr
                used |= 1 << attr
                if op != 2:
                    flat_only &= ~(1 << attr)
                ninterp = max(ninterp, attr + 1)
        if w >> 26 == 0x3e and n == 2:
            target, en, compr = (w >> 4) & 0x3f, w & 0xf, (w >> 10) & 1
            if target < 8:
                fmt = 4 if compr else {1: 1, 3: 2, 9: 3}.get(en, 9)
                col = (col & ~(0xf << (4 * target))) | fmt << (4 * target)
            elif target == 8:
                depth = 1
        if w == 0xbf810000:
            break
        i += n
    user = 24 if user is None else user
    settings = [k | (1 << 10 if flat_only & (1 << k) else 0) for k in range(ninterp)]
    ena = 2
    shift = {5: 0, 6: 8}.get(u32(fssm, 0x10))
    if shift is not None:
        base = 0x130 + shift
        n = u32(fssm, base + 0x100) if base + 0x114 <= len(fssm) else 99
        if n <= 32 and all(fssm[base + 8 * k + 3] == k and not fssm[base + 8 * k + 4] for k in range(n)):
            ena = u32(fssm, base + 0x110) or ena
    if u32(fssm, 0x10) == 6:
        rsrc1 = 0x40000000 | 1 << 21 | (u32(fssm, 0x104) & 0xff) << 12
    else:
        rsrc1 = (u32(fssm, 0x108) & ~(0xff << 12)) | (u32(fssm, 0xfc) & 0xff) << 12
    rsrc2 = (user & 0x1f) << 1 | ((user >> 5) & 1) << 27
    return rsrc1, rsrc2, ena, ninterp, settings, col, depth


def records(fssm):
    """The FSSM's resource records (VIctory's UsedResource): kind (0 SRV, 1 sampler, 2 CBV, 3 UAV),
    root type (0 constants, 1 root descriptor, 3 table, 4 static sampler), dword offset in the
    table, register, dwords per descriptor, user-data dword, root parameter."""
    at, n = u32(fssm, 0xf0) + 0x10, u32(fssm, 0xfc)
    out = []
    for k in range(n if at + 20 * n <= len(fssm) else 0):
        e = fssm[at + 20 * k:at + 20 * k + 20]
        out.append((e[0], e[1], u32(e, 4), u32(e, 8), e[16], e[17], e[18]))
    return out


def words_env(name, count, default):
    v = os.environ.get(name)
    return [int(x, 16) for x in v.split(',')][:count] if v else default


def main():
    args = sys.argv[1:]
    draw = '--draw' in args
    args = [a for a in args if a != '--draw']
    src, out, wanted = args[0], args[1], args[2:]
    os.makedirs(out, exist_ok=True)
    names = [f for f in os.listdir(src) if f.endswith('.ps.bin')]
    if not wanted or any(w.endswith('.ps.bin') for w in wanted):
        names = [w for w in wanted if w.endswith('.ps.bin')] or sorted(names)
    ids = {w for w in wanted if not w.endswith('.ps.bin')}
    bad_t, bad_v = words_env('MEM_T', 8, None), words_env('MEM_V', 4, None)
    for name in sorted(names):
        fssm = fssm_of(open(os.path.join(src, name), 'rb').read())
        if not fssm:
            continue
        at, nbytes = u32(fssm, 0xc0) + 0x28, u32(fssm, 0xc8)
        if u32(fssm, 0x10) == 6 and nbytes > 24:
            nbytes -= 24
        nbytes = min(nbytes, len(fssm) - at) & ~3
        raw = fssm[at:at + nbytes]
        sid = '%016x' % fnv(raw)
        if ids and sid not in ids:
            continue
        code = list(struct.unpack('<%dI' % (nbytes // 4), raw))
        nop_flat_scratch(code)
        path = os.path.join(out, sid + '.code')
        open(path, 'wb').write(struct.pack('<%dI' % len(code), *code))
        rsrc1, rsrc2, ena, ninterp, settings, col, depth = pixel_state(fssm, code)
        line = '%s %s 0 %x %x %x %u %x %u %s' % (sid, path, rsrc1, rsrc2, ena, ninterp, col, depth,
                                                ','.join('%x' % s for s in settings) or '-')
        if draw:
            user = [0] * 32
            tables = {}
            for kind, root, offset, _register, dwords, ud, _slot in records(fssm):
                if root != 3 or ud >= 32:
                    continue
                user[ud] = TABLES + 0x4000 * ud
                table = tables.setdefault(ud, {})
                if dwords == 8:
                    words = bad_t or GOOD_T
                elif kind == 1:
                    words = GOOD_S
                else:
                    words = bad_v or GOOD_V
                for k, w in enumerate(words[:dwords]):
                    table[offset + k] = w
            memory = bytearray(BUFFERS + 0x1000)
            for ud, table in tables.items():
                for k, w in table.items():
                    if k < 0x1000:
                        struct.pack_into("<I", memory, 0x4000 * ud + 4 * k, w)
            mem = os.path.join(out, sid + '.mem')
            open(mem, 'wb').write(bytes(memory))
            line += ' u=%s m=%x@%s' % (','.join('%x' % v for v in user[:24]), TABLES, mem)
        print(line)


if __name__ == '__main__':
    main()
