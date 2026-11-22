# bcpack: BCPack decoding in a separate process

BCPack is the Xbox Series' hardware texture compression, which titles decode through
DirectStorage. About 14% of the target title's catalogued textures (51,156 of 357,115, per Leonida's texture
notes) need it; the rest are zlib or raw. The decoder that covers all six BCPack codecs is
Leonida's clean-room `tools/bcpack.py` (GPLv3). rebox is GPL-2.0-only, because the
title's process loads AnyPS5's recompiler (GPL-2.0-only), and GPLv3 code can't join that process.
So the decoder runs here, as a separate program the DirectStorage shim talks to over a socket.

## Licence

Everything in this directory is GPL-3.0-or-later (each file says so). It runs Leonida's decoder,
pinned in `flake.nix` (`leonida-src`); nothing of Leonida's is copied into this repository. The
rest of rebox stays GPL-2.0-only: the shim and the helper are separate programs that
communicate over a localhost socket.

The method is covered by Microsoft's patent US 11,234,023 B2 (the rANS entropy layer), whatever
the code's licence.

## How it fits

- `rebox-run` starts `bcpack-helper.py` under PyPy (about five times faster than CPython on
  this decoder: around 3 MB/s of blocks per process), passes its own PID so the helper exits with
  Wine, and exports the port as `REBOX_BCPACK_PORT`. The log is `$WINEPREFIX/bcpack-helper.log`.
- `shims/xcrdapi` sends each BCPack request (codec from the request options, the compressed bytes
  from the file or memory source, the destination size) and copies the blocks back. Each
  DirectStorage worker thread has its own connection, and each connection its own helper
  process, so queues decode in parallel.
- Without the helper, BCPack requests fail with `E_NOTIMPL` and are logged, as before.

BC6H and BC7 chunks often need history entries Leonida marks provisional, because the calibrated
tables are fitted from local data and not in its repository. The helper allows them by default
(the texture may be slightly off rather than missing) and counts them in its log.
`REBOX_BCPACK_HISTORY=PATH` passes calibrated tables (Leonida's `--history`), and
`REBOX_BCPACK_STRICT=1` refuses provisional chunks. `REBOX_BCPACK=0` doesn't start the
helper.

## Test

`tests/run-bcpack.sh` (dev shell): `make-vector.py` builds a synthetic BC4 chunk with the encoder
in Leonida's own tests, and `tests/bcpack_probe.c` sends it through `XCRDAPI.dll` to the helper
and compares the blocks.
