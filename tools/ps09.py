#!/usr/bin/env python3
"""Minimal reader for the Scarlett PS\\x00\\x09 PSO cache (layout as reverse-engineered by PercyStar).

Moved from xsx-shader-lab. GPL-2.0-only. compute_meta() is the lab's manifest step: each compute
shader's COMPUTE_PGM_RSRC1/2 (words 5 and 6 of its PSO's metadata blob) and thread-group size
(words 3 and 4).
"""
import struct


class Ps09:
    def __init__(self, path):
        self.raw = open(path, "rb").read()
        r = self.raw
        assert r[:4] == b"PS\x00\x09"
        self.c0, self.c1, self.c2, self.c3, self.meta_size, self.code_size = struct.unpack_from("<6I", r, 0x50)
        self.off_c0 = 0x80
        self.off_c1 = self.off_c0 + self.c0 * 0x70
        self.off_c2 = self.off_c1 + self.c1 * 0x18
        self.off_c3 = self.off_c2 + self.c2 * 8
        self.off_meta = self.off_c3 + self.c3 * 8

    def meta(self, index):
        off, size = struct.unpack_from("<2I", self.raw, self.off_c2 + index * 8)
        return self.raw[self.off_meta + off:self.off_meta + off + size]

    def compute_psos(self):
        """Yield (hash, rsrc_meta, unk_meta, shader_index, bind_meta) for each compute PSO."""
        for i in range(self.c1):
            yield struct.unpack_from("<QIIII", self.raw, self.off_c1 + i * 0x18)

    def graphics_psos(self):
        for i in range(self.c0):
            p = self.off_c0 + i * 0x70
            slots = [struct.unpack_from("<II", self.raw, p + 0x38 + s * 8) for s in range(7)]
            yield struct.unpack_from("<Q", self.raw, p)[0], self.raw[p:p + 0x38], slots


def compute_meta(cache):
    """shader index -> (rsrc1, rsrc2, (x, y, z)), from the first compute PSO that uses it."""
    out = {}
    for _hash, m_rsrc, _m_unk, shader, _m_bind in cache.compute_psos():
        if shader in out:
            continue
        blob = cache.meta(m_rsrc)
        if len(blob) < 28:
            continue
        w = struct.unpack_from("<7I", blob)
        out[shader] = (w[5], w[6], (w[3] & 0xFFFF, w[3] >> 16, w[4] & 0xFFFF))
    return out
