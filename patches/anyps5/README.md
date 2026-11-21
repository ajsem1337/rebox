# AnyPS5 patches

Applied, in name order, to AnyPS5 at `fc973e0189` (upstream `main`) to build `anyps5/`.
They come from the user's `xsx-shader-lab`, where `upstream/` holds their pull requests:

| Patch | Upstream |
| --- | --- |
| `0001-lab-only-accept-mip-point-preclamp` | not proposed: accepts `MIP_POINT_PRECLAMP` without modelling it (AnyPS5's driver only, so it changes nothing in rebox_anyps5) |
| `0002-...-MIMG-...-VCC` | boykopovar/AnyPS5#24796 |
| `0003-...-fold-XOR-register-swaps` | boykopovar/AnyPS5#24794 |
| `0004-...-speculate-descriptor-table-pointers` | not yet submitted |

The flake applies only the hunks under `core/shader/` (the recompiler); hunks for AnyPS5's driver,
tests and docs are skipped. Drop a patch here once its pull request is merged and the pin moves past it.
