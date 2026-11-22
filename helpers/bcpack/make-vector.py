#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Writes a synthetic BC4 BCPack chunk and the blocks it decodes to, for tests/run-bcpack.sh:
#   make-vector.py LEONIDA_DIR OUT_DIR   ->  OUT_DIR/bc4.bcpack, OUT_DIR/bc4.expected
# Built with the encoder in Leonida's tests/test_bcpack.py (two blocks: one from the base
# predictor, one referring back to it), the same vector that test checks.
import os
import sys

leonida, out = sys.argv[1], sys.argv[2]
sys.path.insert(0, os.path.join(leonida, "tests"))
sys.path.insert(0, os.path.join(leonida, "tools"))
import bcpack_tables as tables  # noqa: E402
import test_bcpack as t  # noqa: E402

inv_pair = {p: i for i, p in enumerate(tables.BC4_PAIR)}
b0 = sum(((r + p) & 7) << (3 * (4 * r + p)) for r in range(4) for p in range(4))
b1 = sum(((7 - r - p) & 7) << (3 * (4 * r + p)) for r in range(4) for p in range(4))
expected = bytes([130, 120]) + b0.to_bytes(6, "little") + bytes([140, 110]) + b1.to_bytes(6, "little")
lanes = [[] for _ in range(12)]
for d, base, (a0, a1), bits in [(0, (128, 128), (130, 120), b0), (1, (130, 120), (140, 110), b1)]:
    lanes[0].append(t._zig(d))
    lanes[1].append(0)
    lanes[2].append(t._zig(a0 - base[0]))
    lanes[3].append(t._zig(a1 - base[1]))
    for r in range(4):
        row = (bits >> (12 * r)) & 0xFFF
        lanes[4 + 2 * r].append(inv_pair[(row & 7, (row >> 3) & 7)])
        lanes[4 + 2 * r + 1].append(inv_pair[((row >> 6) & 7, (row >> 9) & 7)])
with open(os.path.join(out, "bc4.bcpack"), "wb") as f:
    f.write(t.encode_lanes(lanes, 4))
with open(os.path.join(out, "bc4.expected"), "wb") as f:
    f.write(expected)
