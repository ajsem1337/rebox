#!/usr/bin/env python3
# Mesh plan for `sa5_check ms` (docs/anyps5.md) from REBOX_DUMP_SHADERS mesh containers (DXBC > DXIL >
# XBCH + zlib > FSSM), with the offsets of shims/d3d12_xs/console_fssm.c:
#   ms-plan.py DUMP_DIR OUT_DIR [NAME.ms.bin...] > PLAN
# writes each FSSM to OUT_DIR and a plan line per shader to stdout.
import os, struct, sys, zlib

src, out = sys.argv[1], sys.argv[2]
names = sys.argv[3:] if len(sys.argv) > 3 else sorted(f for f in os.listdir(src) if f.endswith('.ms.bin'))
os.makedirs(out, exist_ok=True)
u32 = lambda b, o: struct.unpack_from('<I', b, o)[0]
u64 = lambda b, o: struct.unpack_from('<Q', b, o)[0]
for name in names:
    data = open(os.path.join(src, name), 'rb').read()
    parts = u32(data, 0x1c)
    fssm = None
    for i in range(parts):
        at = u32(data, 0x20 + 4 * i)
        if data[at:at + 4] == b'DXIL':
            body = data[at + 8:at + 8 + u32(data, at + 4)]
            for z in range(len(body) - 2):
                if body[z] == 0x78 and body[z + 1] in (0x01, 0x5e, 0x9c, 0xda):
                    try:
                        fssm = zlib.decompressobj().decompress(body[z:])
                        if fssm[:4] == b'FSSM':
                            break
                        fssm = None
                    except zlib.error:
                        pass
    if not fssm:
        print('#', name, 'no FSSM')
        continue
    payload = 0x10
    rec = payload + u64(fssm, payload + 0x48)
    data_at = rec + 0x54
    info = data_at + 0x448
    code_at = payload + u64(fssm, rec) + 24
    code_bytes = (u64(fssm, rec + 8) - 24) & ~3
    flags = u32(fssm, rec + 0x14)
    threads = struct.unpack_from('<H', fssm, info)[0]
    maxv = struct.unpack_from('<H', fssm, info + 2)[0]
    maxp = struct.unpack_from('<H', fssm, info + 4)[0]
    topo = fssm[info + 7]
    lds = u32(fssm, rec + 0x4c)
    clip = u32(fssm, data_at + 0x194)
    rsrc1 = 0x40000000 | 1 << 21 | (u32(fssm, rec + 0x44) & 0xff) << 12
    wave = 32 if flags & 0x20 else 64
    # FLAT_SCRATCH setup (s_setreg to hwreg 20/21) becomes s_nop, as console_compile.c does.
    fssm = bytearray(fssm)
    at = code_at
    while at < code_at + min(code_bytes, 256):
        word = u32(fssm, at)
        op = (word >> 23) & 0x1f
        if word >> 28 == 0xb and op in (19, 21) and (word & 0x3f) in (20, 21):
            for k in range(2 if op == 21 else 1):
                struct.pack_into('<I', fssm, at + 4 * k, 0xbf800000)
            at += 8 if op == 21 else 4
            continue
        at += 8 if (word >> 28 == 0xb and op == 21) else 4
    path = os.path.join(out, name.replace('.ms.bin', '.fssm'))
    open(path, 'wb').write(fssm)
    print(name.split('.')[0], path, code_at, code_bytes // 4, wave, threads, maxv, maxp, topo + 1, lds,
          '%x' % clip, '%x' % rsrc1)
