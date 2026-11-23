# textures: d3d12_xs's detiling, checked on the real pools

`texcheck.py` puts a title's own textures through the code d3d12_xs uses for textures placed at
title addresses, with no game exe needed, and compares the result with an independent decoder.
For each sampled texture of a pool (`.ttpf`):

1. the texture's memory image is read from the pool, as DirectStorage would write it: the
   allocation span (for a cubemap, its final full-resolution chunk), decompressed (zlib, raw or
   BCPack, through Leonida's reader);
2. the swizzle mode is picked by `shims/d3d12_xs/title_textures.c`'s rule, and checked against the
   pool entry's own tile mode;
3. the image is detiled by `build/texcheck.exe` (`tools/texcheck.c`: d3d12_xs's `detile.cpp` and
   AMD's addrlib, run under Wine), arrays both mip-major and slice-major;
4. the result is compared byte for byte with Leonida's `tools/ttpf_mips.py` export. `--png DIR`
   also writes both as PNGs (mip 0 of the first slice) to look at.

It runs Leonida's tools (GPLv3), so like `helpers/bcpack` it is GPL-3.0-or-later and a separate
program; nothing of Leonida's is copied here. The dev shell's `LEONIDA_SRC` (the flake's
`leonida-src` pin) is used unless `--leonida` names another checkout.

```sh
nix develop -c make build/texcheck.exe
nix develop -c python3 helpers/textures/texcheck.py path/to/title/data/ttdb
nix develop -c python3 helpers/textures/texcheck.py --shape 2d_array --limit 20 path/to/title/data/ttdb
```

`--every N` and `--limit N` sample each pool, `--max-bytes` skips large textures (Leonida's
decoder is pure Python), and `--shape` checks only 2D textures, arrays, cubemaps or volumes.

Results on 2026-11-22 are in [docs/d3d12x.md](../../docs/d3d12x.md), "Texture tiling".
