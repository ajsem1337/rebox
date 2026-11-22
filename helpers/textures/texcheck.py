#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Checks d3d12_xs's texture detiling on the target title's real texture pools, without the game: each
# texture's tiled memory image is read from its pool (as DirectStorage would write it), the swizzle
# mode is picked by the same rule as shims/d3d12_xs/title_textures.c, and the image is detiled by
# d3d12_xs's own code (build/texcheck.exe: detile.cpp and addrlib, run under Wine). The result is
# compared with Leonida's independent decoder (tools/ttpf_mips.py, GPLv3, which is why this runs as
# a separate program like helpers/bcpack). See helpers/textures/README.md.
#
#   texcheck.py --leonida DIR [--texcheck build/texcheck.exe] [--every N] [--limit N]
#               [--max-bytes N] [--shape 2d|2d_array|cubemap|volume] [--png DIR] POOL_OR_DIR...

import argparse
import collections
import os
import struct
import subprocess
import sys
import tempfile

DETILE_LINEAR, DETILE_4KB_S, DETILE_64KB_D = 0, 1, 2
MODE_NAMES = {DETILE_LINEAR: "linear", DETILE_4KB_S: "SW_4KB_S", DETILE_64KB_D: "SW_64KB_D"}


TILE_4K = {1: (64, 64), 2: (64, 32), 4: (32, 32), 8: (32, 16), 16: (16, 16)}  # elements per 4 KiB tile


def our_mode(shape, width, height, layers, mips, block, element_bytes):
    """title_textures.c's REBOX_TEXTURE_SWIZZLE=auto rule (single_texture_mode for 2D)."""
    if shape in ("volume", "cubemap") or (shape != "2d" and layers == 6):
        return DETILE_4KB_S
    if layers > 1:
        return DETILE_64KB_D
    bw, bh = (width + block - 1) // block, (height + block - 1) // block
    if bw * bh * element_bytes >= 1 << 20:
        return DETILE_64KB_D
    tw, th = TILE_4K[element_bytes]
    padded = -(-bw // tw) * tw * -(-bh // th) * th * element_bytes
    return DETILE_64KB_D if mips == 1 and padded > 65536 else DETILE_4KB_S


def jobs_for(tm, pool, index, a):
    """(label, job header, tiled bytes) per detiling to try for one texture."""
    d, layout = a["descriptor"], a["layout"]
    shape = a["shape"]
    block = 4 if d["format"] in tm.BC_FORMATS else 1
    eb = layout["element_bytes"]
    layers = d["depth"]
    mode = our_mode(shape, d["width"], d["height"], layers, d["mips"], block, eb)
    cache = {}
    if shape == "cubemap":
        tiled = pool.read_range(d["base"] + layout["data_offset"], 6 * layout["face_size"], cache,
                                bcpack_mode="provisional")
    else:
        tiled = pool.read_range(d["base"], layout["size"], cache, bcpack_mode="provisional")
    volume = shape == "volume"
    mips = 1 if volume else d["mips"]

    def header(mip_major):
        return struct.pack("<8I", mode, d["format"], d["width"], d["height"], layers, mips, int(volume),
                           int(mip_major))

    if shape == "2d_array":
        return mode, [("slice-major", header(False), tiled), ("mip-major", header(True), tiled)]
    return mode, [("", header(False), tiled)]


def detile_batch(texcheck, wine, jobs):
    with tempfile.TemporaryDirectory() as tmp:
        jpath, opath = os.path.join(tmp, "jobs"), os.path.join(tmp, "out")
        with open(jpath, "wb") as f:
            f.write(struct.pack("<I", len(jobs)))
            for _, hdr, data in jobs:
                f.write(hdr + struct.pack("<Q", len(data)) + data)
        env = dict(os.environ, WINEDEBUG="-all")
        subprocess.run([wine, texcheck, jpath, opath], check=True, env=env,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        out = []
        with open(opath, "rb") as f:
            for _ in jobs:
                ok, size = struct.unpack("<IQ", f.read(12))
                out.append(f.read(size) if ok else None)
        return out


def main():
    ap = argparse.ArgumentParser(description="compare d3d12_xs's detiling with Leonida's on real pools")
    ap.add_argument("--leonida", default=os.environ.get("LEONIDA_SRC"), help="Leonida checkout")
    ap.add_argument("--texcheck", default="build/texcheck.exe")
    ap.add_argument("--wine", default=os.environ.get("WINE", "wine"))
    ap.add_argument("--every", type=int, default=50, help="every Nth texture of each pool")
    ap.add_argument("--limit", type=int, default=40, help="textures per pool at most")
    ap.add_argument("--max-bytes", type=int, default=4 << 20, help="skip textures larger than this")
    ap.add_argument("--shape", choices=["2d", "2d_array", "cubemap", "volume"],
                    help="only this shape (every texture is looked at, --every ignored)")
    ap.add_argument("--png", help="write ours_/leonida_ PNGs (mip 0, first slice) here")
    ap.add_argument("pools", nargs="+")
    args = ap.parse_args()
    if not args.leonida:
        ap.error("--leonida (or LEONIDA_SRC) is required")
    sys.path.insert(0, os.path.join(args.leonida, "tools"))
    import ttpf_mips as tm
    import dds_png

    paths = []
    for p in args.pools:
        paths += tm.find_pools(p)
    tally = collections.Counter()
    mode_mismatch = collections.Counter()
    examples = []
    if args.png:
        os.makedirs(args.png, exist_ok=True)

    for path in paths:
        pool = tm.Pool(path)
        batch, meta = [], []
        picked = 0
        for index in range(0, pool.textures, 1 if args.shape else args.every):
            if picked >= args.limit:
                break
            if args.shape:
                d = pool.descriptor(index)
                kind = {2: "cubemap", 3: "volume"}.get(d["dimension"], "2d_array" if d["depth"] > 1 else "2d")
                if kind != args.shape:
                    continue
            a = tm.analyze(pool, index)
            if "blocked" in a or not a.get("full_chain") or a["layout"]["size"] > args.max_bytes:
                continue
            try:
                ref = tm.texture_dds(pool, a, allow_provisional=True)
                mode, jobs = jobs_for(tm, pool, index, a)
            except Exception as e:  # a texture Leonida can't export isn't a check
                tally["skipped (" + type(e).__name__ + ")"] += 1
                continue
            picked += 1
            shape = a["shape"]
            theirs = DETILE_64KB_D if a["tile"] == 65536 else DETILE_4KB_S
            if mode != theirs:
                mode_mismatch[f"{shape}: ours {MODE_NAMES[mode]}, pool {MODE_NAMES[theirs]}"] += 1
            for label, hdr, data in jobs:
                batch.append((label, hdr, data))
                meta.append((path, index, shape, label, ref))
        if not batch:
            continue
        results = detile_batch(args.texcheck, args.wine, batch)
        for (path, index, shape, label, ref), ours in zip(meta, results):
            key = shape + (f" ({label})" if label else "")
            payload = ref[148:]
            if ours is None:
                tally[key + ": detile failed"] += 1
            elif ours == payload:
                tally[key + ": identical"] += 1
            else:
                same = sum(1 for i in range(0, min(len(ours), len(payload)), 16) if ours[i:i + 16] == payload[i:i + 16])
                tally[key + ": differs"] += 1
                if len(examples) < 12:
                    examples.append(f"{os.path.basename(path)} #{index} {key}: {len(ours)} vs {len(payload)} bytes, "
                                    f"{100 * same // max(1, len(payload) // 16)}% of 16-byte runs equal")
            if args.png and ours is not None and (label in ("", "slice-major")):
                stem = f"{os.path.splitext(os.path.basename(path))[0]}_{index}_{shape}"
                for who, body in (("ours", ours), ("leonida", payload)):
                    try:
                        with open(os.path.join(args.png, f"{who}_{stem}.png"), "wb") as f:
                            f.write(dds_png.dds_to_png_bytes(ref[:148] + body, 512))
                    except Exception as e:
                        tally[f"png not written ({type(e).__name__})"] += 1
        print(f"{os.path.basename(path)}: {picked} textures", flush=True)

    print("\nmode rule vs the pools' own tile mode:")
    print("  all agree" if not mode_mismatch else "\n".join(f"  {k}: {v}" for k, v in sorted(mode_mismatch.items())))
    print("detiled by d3d12_xs vs Leonida:")
    for k, v in sorted(tally.items()):
        print(f"  {k}: {v}")
    for e in examples:
        print("  e.g.", e)


if __name__ == "__main__":
    main()
