# Console shaders with AnyPS5: design

Status: design for review, nothing implemented. Written 2026-11-21. Supersedes the translator choice
in [shaders.md](shaders.md) (Leonida's contract); most of the plumbing there carries over, as listed
below. Facts about AnyPS5 are from upstream `main` at `fc973e0189` and the user's
`xsx-shader-lab` (`rdna-run`), which compiles 2,579 of the 2,620 PS09 compute shaders with the
user's four patches.

## What AnyPS5 needs from a host

| | AnyPS5 (upstream `main`) |
|---|---|
| Compile input | code words, stage, user SGPR values, compute stage info (from `RSRC1`/`RSRC2`: user SGPR count, TGID enables, thread-id components, LDS size, float mode), and **guest memory**: a snapshot region or a read callback (`SrtRuntime::readMemory`) |
| Why memory | the recompiler walks the descriptor tables the SGPRs point at, resolves each T#/V#/S# and specialises on them (formats, dimensions, compare/gather) |
| Result | SPIR-V plus specialisation constants; `bindings[]` with kind, set/binding (`stage*65 + local`, set 0, or 1 for fragment) and the descriptor **by value** (4 or 8 raw words); 128-byte push data; ShaderData (16 KB: user data, buffer offsets, image/sampler metadata); a BDA page table and a fault buffer; required subgroup size |
| Per draw | a fresh resource snapshot. A cached result is patched when only V# words, user data or the flattened SRT change; new T#/S# values or V# fields that feed specialisation need a new result (possibly a new module) |
| Host binding | AnyPS5's own AGC driver decodes V# into guest buffers, S# into samplers, T# into textures through its texture cache and detiler: about 25k lines (`ShaderResources`, `Texture`, `GuestBufferMemory`, `BdaResources`, `VulkanDevice`), on its own Vulkan device |
| Stages | compute proven in the lab; pixel, vertex, mesh, geometry exist in AnyPS5 but need per-stage info (interpolators and export mapping, vertex-buffer V#s and fetch registers, mesh configuration). The lab hasn't run them |

Two consequences:

1. **Compile at runtime, not offline.** A compile needs the real descriptors, so it happens at the first
   draw or dispatch that uses a pipeline, with results cached by AnyPS5's own variant key.
2. **Don't use AnyPS5's driver.** It owns a Vulkan device and decodes descriptors into its own
   objects; vkd3d-proton already owns the device and every resource the title created through D3D12.
   The host side here maps each binding to the vkd3d-proton descriptor the title created instead.

## The descriptor-format problem, and the shadow heap

AnyPS5 reads descriptors as the console wrote them (GFX10 layout). Our heaps hold what RADV writes
for the 890M (GFX11 layout), so AnyPS5 would misread formats and dimensions. Instead of
reinterpreting those, `d3d12_xs` keeps a **shadow heap** per shader-visible heap, in console
format:

- On every descriptor write the title makes through the API (device slots 17-19, 22-24, already
  hooked for the lookup table), `d3d12_xs` encodes the view as a GFX10 T#/V#/S# into the shadow at
  the same index. The encoders follow Mesa's `ac_descriptors` (MIT): the D3D12 view description
  gives format, dimension, mips, layers, and the resource's GPU address.
- The address the title sees for a heap (heap slot 10, patch 1) becomes the shadow's address, in a
  32-bit range we choose. Descriptor-table pointers are truncated to 32 bits by the game's code,
  and AnyPS5 resolves them on the CPU through our read callback. So the RADV low-window patch is
  not needed on this path: the shadow only has to look like it is below 4 GiB to the callback.
- Descriptors a title writes into heap memory itself land in the shadow in console format, as on
  the console. Turning those into vkd3d-proton views is the one case that needs decoding (later,
  only if the exe shows it).

Mapping a binding back to a real descriptor uses the existing lookup table (patch 4), keyed on the
shadow's console bytes: words → heap index → the vkd3d-proton descriptor at that index.

## Runtime flow (compute first)

1. **PSO creation** (`d3d12_xs`): a console shader (FSSM container) becomes a deferred console
   pipeline: code extracted, the `FLAT_SCRATCH` prologue (`s_setreg_imm32_b32` to
   `HW_REG_FLAT_SCR_LO/HI`, 34 PS09 shaders and much of the `the shader archive` form) overwritten with
   same-size `s_nop`s, `RSRC1`/`RSRC2` and thread-group size from the container metadata.
2. **Dispatch** (vkd3d-proton patch): pack user SGPRs from the root parameters (patch 3's code),
   then ask `d3d12_xs` to compile: code, SGPRs, stage info, and a memory callback over the shadow
   heaps and identity-mapped GPU memory. AnyPS5 returns (or patches a cached) result.
3. **Bind**: a pipeline layout per console pipeline from AnyPS5's binding list (one set; its 65
   bindings per stage). For each binding, the vkd3d-proton descriptor found through the lookup is
   copied into a per-dispatch descriptor-buffer range. V# bindings can instead become buffer
   addresses directly, since buffers are identity-mapped. ShaderData and push data are uploaded
   from the result; the BDA page table is one identity range plus the shadow heaps.
4. **Restore**: vkd3d-proton's own descriptor bindings and push data are re-applied for the next
   ordinary pipeline (patch 3 already invalidates its root state after a console push).

Graphics follows once compute works: the extra stage info comes from the PSO (input layout, render
targets, export formats) and the container's signatures.

## Where AnyPS5 runs

AnyPS5's recompiler (`core/shader/recompiler`, without the AGC driver) built as a Windows DLL with
mingw, loaded by `d3d12_xs` in-process. americas-recomp builds it with MSVC, so it has no Linux-only
dependencies; mingw is the thing to check first. The alternative, a native Linux compile service
that Wine talks to over a pipe, avoids cross-compiling AnyPS5 but adds IPC to every compile.

**Licence.** AnyPS5 is GPL-2.0-only. A `d3d12_xs` that loads it in-process, and the vkd3d-proton
patch that calls it, become a GPL-2 combination. rebox has no licence file yet; this
choice is yours. A separate compile service limits the GPL-2 part to that program.

## What carries over

| Piece | On the AnyPS5 path |
|---|---|
| Patch 1, real heap addresses | kept; the address becomes the shadow heap's |
| Patch 2, `SCSV` pipelines | replaced by deferred console pipelines compiled at dispatch |
| Patch 3, SGPR packing | kept: the SGPR values are AnyPS5's user data |
| Patch 4, lookup table | kept, keyed on shadow (console-format) bytes |
| RADV 32-bit low window | not needed (truncated pointers resolve through the CPU callback) |
| `scsv_pack.py`, `translate-shaders.sh`, Leonida | retired |
| `rdna-run`, lab patches | AnyPS5 build and patch set; its sweep stays the compiler's test bench |

## Order of work

1. Build AnyPS5's recompiler alone (no AGC driver) as a mingw DLL with the lab's patches, and
   compile one PS09 compute shader through it under Wine with a snapshot, as `rdna-run` does.
2. Shadow heap: GFX10 encoders for buffer and 2D views and samplers, and a smoke test that
   AnyPS5's own decoders read them back to the view that was created.
3. Deferred compute pipelines and the dispatch path, with a hand-written compute shader whose
   descriptors are known (the smoke test's UAV write, as RDNA2 code assembled with `llvm-mc`).
4. Real the target title compute shaders on the 890M.
5. Graphics stages, starting with a pixel shader.

## Open questions

- **What does the game pass at PSO creation: the FSSM container or a PS09 cache blob?**
  `d3d12_xs` logs it (the PSO census: each stage's bytecode kind and the `CachedPSO` blob's size and
  first bytes, for the first PSOs and then at powers of two; `REBOX_DUMP_SHADERS=1` also dumps
  the blobs). A blob vkd3d-proton didn't make fails creation as a cache mismatch, so `d3d12_xs`
  retries once without it. PS09 entries carry the `RSRC1`/`RSRC2` AnyPS5 needs; for FSSM they come
  from the container's header.
- **Compile latency at first use.** Each new descriptor combination can mean a compile. AnyPS5's
  variant cache and memo patches limit this; the pipeline cache should persist across runs.
- **Speculated descriptors** (lab patch 0004) are checked at runtime through the fault buffer; the
  host must read it back and recompile on a mismatch.
- **Graphics on Scarlett.** The 7,498 NGG shaders may not map cleanly onto AnyPS5's mesh path.
