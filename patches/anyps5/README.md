# AnyPS5 patches

Applied, in name order, to AnyPS5 at `fc973e0189` (upstream `main`) to build `anyps5/`.
Each is one commit on a branch of the user's AnyPS5 clone (`../AnyPS5`, remote `puffnfresh`):

| Patch | Upstream |
| --- | --- |
| `0001-lab-only-accept-mip-point-preclamp` | not proposed: accepts `MIP_POINT_PRECLAMP` without modelling it (AnyPS5's driver only, so it changes nothing in rebox_anyps5) |
| `0002-...-MIMG-...-VCC` | `fix/shader-mimg-vcc-descriptor`, boykopovar/AnyPS5#24796 |
| `0003-...-fold-XOR-register-swaps` | `feat/shader-fold-xor-swaps`, boykopovar/AnyPS5#24794 |
| `0004-...-speculate-descriptor-table-pointers` | `feat/shader-speculate-descriptor-pointers`, not yet submitted |

The flake applies only the hunks under `core/shader/` (the recompiler); hunks for AnyPS5's driver,
tests and docs are skipped. Drop a patch here once its pull request is merged and the pin moves past it.

The smoke test checks 0003 and 0004 through the whole deferred path with the lab's hand-written
repros (`tests/rdna/`, docs/anyps5.md).

## Still to move upstream

General recompiler behaviour that rebox works around outside AnyPS5 for now. Each should
become a branch in `../AnyPS5` (its AGENTS.md rules apply) and a patch here until merged:

- `s_setreg` to `FLAT_SCR_LO/HI` (hwreg 20/21) at the start of a shader: `console_compile.c` NOPs
  them before AnyPS5 sees the code. AnyPS5 should treat them as host-owned scratch setup.
- Emulated barycentrics run out of locations for 27 of 9,535 the target title pixel shaders
  (`BarycentricGeometryShader.cpp`, the `sa5_check ps` sweep).
- Wave size from the code when the registers don't give it (`sa5_analyse`): AnyPS5 reads it from
  the PS5 registers, so this is only worth proposing if another frontend needs it.
