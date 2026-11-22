# Console shaders with AnyPS5: design

Status: steps 1-3 implemented for compute (see Order of work). Written 2026-11-21. Supersedes the translator choice
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
reinterpreting those, `d3d12_xs` keeps a **shadow heap** per CBV/SRV/UAV and sampler heap, in
console format (`shims/d3d12_xs/shadow.c`, encoders in `console_desc.c`):

- On every descriptor write the title makes through the API (device slots 17-19, 22-24),
  `d3d12_xs` encodes the view as a GFX10 T#/V#/S# into the shadow at the same index, using the
  field layout AnyPS5's decoders read. The D3D12 view description gives format, dimension, mips
  and layers; buffers carry their GPU address, and textures (which have none here) a synthetic
  address unique per resource, since the descriptor only has to identify the view.
  `CopyDescriptors*` copy shadow bytes, so CPU-only heaps get shadows too. The stride is the
  increment the title gets, so it needs at least 32 bytes (16 for samplers).
- The address the title sees for a heap (heap slot 10) is the shadow's title address, in a
  32-bit range we choose (default since 2026-11-21; `REBOX_HEAP_VA=real` gives patch 1's real
  address). vkd3d-proton packs console SGPRs from the same base. Descriptor-table pointers are truncated to 32 bits by the game's code,
  and AnyPS5 resolves them on the CPU through our read callback. So the RADV low-window patch is
  not needed on this path: the shadow only has to look like it is below 4 GiB to the callback.
- Descriptors a title writes into heap memory itself land in the shadow in console format, as on
  the console. Turning those into vkd3d-proton views is the one case that needs decoding (later,
  only if the exe shows it).

Mapping a binding back to a real descriptor is a CPU hash over every shadow slot, keyed on its
console words: `ReboxShadowFind(words)` → the vkd3d-proton CPU handle written alongside them
(shader-visible heaps first). AnyPS5 binds by value at compile time, so this replaces the
GPU-visible lookup table of patch 4, which `d3d12_xs` no longer fills.

`d3d12_xs` exports, for the dispatch path and the tests: `ReboxShadowHeap(cpu_start, &title_va,
&data, &stride, &count)`, `ReboxShadowRead(title_va, out, size)` (fails unless the range lies in
one heap) and `ReboxShadowFind(words, word_count, sampler, &cpu)`. Shader-visible heaps get
title addresses from `0x10000000`, 64 KiB aligned; their shadows sit in GPU-visible memory
(`ReboxGpuAlloc`) so shaders could read them directly later.

The smoke test checks the encoders field by field, then compiles a hand-assembled RDNA2 shader
(`s_load` of a T#, V# and S# from two descriptor tables, `image_sample_lz`, `buffer_store`) through
`rebox_anyps5.dll` with the shadow heaps as its memory: AnyPS5 binds a sampled 2D image, a
storage buffer and a sampler whose words are the shadow's, and each maps back to the vkd3d-proton
descriptor. AnyPS5 also adds a fourth binding, a storage buffer holding all three descriptors'
words (16 dwords), which the dispatch path will have to fill.

## Runtime flow (compute; implemented)

1. **PSO creation** (`d3d12_xs` → patch 0005). A deferred console shader is an `SCRD` blob
   (`shims/d3d12_xs/console_compile.h`): header, `RSRC1`/`RSRC2`, wave size, thread-group size,
   code address, then the RDNA code. vkd3d-proton keeps the blob with the PSO and creates no
   pipeline. Only the smoke test makes `SCRD` blobs so far: turning the game's FSSM containers
   (or PS09 cache entries) into them waits on the PSO census (open question 1). The
   `FLAT_SCRATCH` setup many console shaders open with (`s_setreg_imm32_b32` or `s_setreg_b32`
   to `HW_REG_FLAT_SCR_LO/HI`) becomes same-size `s_nop`s before each compile; only the leading
   run is touched, and the first time is logged.
2. **Dispatch** (patch 0005, `d3d12_command_list_update_compute_pipeline`). User SGPRs are packed
   from the root parameters as for SCSV shaders, except that descriptor tables point at the
   shadow heaps' title addresses (recorded at `SetDescriptorHeaps` through `ReboxShadowHeap`).
   vkd3d-proton calls `d3d12_xs`'s `ReboxConsoleCompile` with the blob, the SGPRs, the bound
   heaps and, for each AnyPS5 descriptor kind, the bindless set and binding that holds it (as
   `vkd3d_bindless_state_find_binding` numbers them).
3. **Compile** (`console_compile.c`). AnyPS5 compiles against the shadow heaps (at their title
   addresses) and `d3d12_xs`'s GPU memory chunks (xmem's allocations, at their GPU VAs) through its
   in-memory cache, which parses the code once and makes a variant per new set of descriptors.
   Each guest binding's elements are found in the bound heap's shadow (`shadow_find_index`),
   which gives heap slots. `sa5_finalize` (`anyps5/spirv_remap.cpp`) then:
   - freezes the specialization constants, which AnyPS5 also uses for descriptor indices;
   - moves each guest descriptor array onto the bindless set as a runtime array indexed by
     those slots;
   - turns host buffers into BDA pointers read from push bytes 128-159.
   It validates the module, which is then cached by AnyPS5's pipeline identity plus the slots.
4. **Bind**. vkd3d-proton makes one pipeline per key, with the root signature's own layout, so
   its bindless sets are already bound. After its descriptors are updated, it pushes the
   160 bytes from the compile (AnyPS5's 128 bytes of push data, then the host-buffer addresses)
   and marks its own root parameters dirty for the next ordinary pipeline.

What AnyPS5's driver would do and this host does instead, from its sources:

| AnyPS5 binding role | Here |
|---|---|
| Guest buffers, images, samplers (set 0, `stage*65 + local`) | the vkd3d-proton descriptor at the slot where the shadow holds the same words. AnyPS5 may rewrite a T#'s `DST_SEL` (identity) and gives each sampler a point-filtered twin (`S#` word 2 filters changed), so those fields are ignored when matching |
| FlattenedSrt (binding 61): every dword the shader's scalar loads read through user SGPRs | its words, in GPU memory shared by content |
| BDA page table (59) | identity ranges for GPU memory, plus title address → GPU copy for each shadow heap |
| Fault buffer (60), GDS (58) | zeroed GPU memory (faults are not read back yet) |
| ShaderData (62) | its words, as FlattenedSrt (compute normally uses push data instead) |
| Image and sampler tables, LDS spill (set 1), scratch (set 2) | not yet: the compile fails and the dispatch is skipped, logged once per shader |

Guest memory matters beyond descriptors. americas-recomp found that user-data pointers into
memory the recompiler can't read make the SRT walk throw. Backing them took its the target title sample
from 25 to 198 of 200 valid compiles. Resources the title creates itself are not in the guest
memory yet. The `SpirvTarget` lists the capabilities AnyPS5 gates on (MinLod among them:
`image_sample_d_cl` needs it), limited to what RADV and lavapipe both have.

The smoke test runs this whole path on lavapipe. `test_console_dispatch` creates a deferred PSO from
hand-assembled RDNA2 code that stores `0x1234abcd` through the V# at the start of its descriptor
table (heap slot 1), dispatches it, and reads the value back. A second dispatch gets the same V# through a pointer
in root constants into `ReboxGpuAlloc` memory. Both pass on RADV (Radeon 890M, 2026-11-22),
where the bindless sets differ from lavapipe's: samplers 0/0, typed views and images 1/1, raw
buffers and CBVs 2/0 (lavapipe: 1/2 for typed views and images). `REBOX_DUMP_SPIRV=DIR` writes each
finalized module.

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
| Patch 2, `SCSV` pipelines | replaced by deferred console pipelines compiled at dispatch (patch 0005, which reuses its push range and SGPR packing) |
| Patch 3, SGPR packing | kept: the SGPR values are AnyPS5's user data |
| Patch 4, lookup table | replaced by a CPU lookup over the shadow heaps (`ReboxShadowFind`); `d3d12_xs` no longer sets a table |
| RADV 32-bit low window | not needed (truncated pointers resolve through the CPU callback) |
| `scsv_pack.py`, `translate-shaders.sh`, Leonida | retired |
| `rdna-run`, lab patches | AnyPS5 build and patch set; its sweep stays the compiler's test bench |

## Order of work

1. ~~Build AnyPS5's recompiler alone as a mingw DLL~~ Done: `anyps5/` builds
   `rebox_anyps5.dll` (16 MB, imports only kernel32/msvcrt/ntdll) from an AnyPS5 checkout with
   `patches/anyps5/` applied, using AnyPS5's own `add_shader_recompiler()`. The guest libc is
   replaced by an include path and two stream functions; winpthreads is linked statically. Its C API
   (`anyps5/rebox_anyps5.h`) compiles a compute shader and exposes the SPIR-V, bindings, push
   data and subgroup size. Under Wine, `sa5_check.exe` compiles a built-in shader and the lab's
   `sh_00000_CS` (2,657 words, 4 bindings) with no guest memory. The flake builds it as
   `.#rebox-anyps5` from a sparse checkout of AnyPS5 `fc973e0189` and the three submodules it
   pins (SPIRV-Headers, SPIRV-Tools, glslang), applying only the patches' `core/shader/` hunks;
   the `shims` package and `rebox-run` include the DLL, and `run-smoke.sh` compiles a shader
   through it. Still to do: fill the `SpirvTarget` from vkd3d-proton's physical device instead of
   fixed values.
2. ~~Shadow heap~~ Done: GFX10 encoders for buffer and texture views and samplers, shadow heaps
   with title addresses, the CPU lookup, and the AnyPS5 round trip in the smoke test (above).
   The heap's slot 10 address and vkd3d-proton's SGPR packing still use the real heap VA; they
   move to title addresses with step 3.
3. ~~Deferred compute pipelines and the dispatch path~~ Done (above): patch 0005,
   `console_compile.c`, `sa5_finalize`, and the end-to-end smoke test. Still to do before real
   shaders: `SCRD` blobs from the game's containers (the `FLAT_SCRATCH` prologue is already
   NOPed at compile). Heap
   slot 10 now returns title addresses, for pointers the title stores in memory itself. Then
   guest memory beyond the shadow heaps for SRT walks (identity-mapped GPU memory). Then the
   roles marked "not yet", fault-buffer readback, and `SpirvTarget` from the device.
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
- **The real user-SGPR map.** TomBox reads, from the retail `the shader archive` PS09 cache
  (unconfirmed except for one shader), per-shader metadata records (tag 0x41). Each gives a
  binding's kind (SRV, CBV, UAV, sampler, direct), its dword offset, register, and a packed word
  with the descriptor dword count (bits 0-7), user SGPR (bits 8-15) and register space. If that
  holds, SGPRs come from those records, not root-parameter order (patch 0005 still packs in root
  order).
- **Graphics on Scarlett.** The 7,498 NGG shaders may not map cleanly onto AnyPS5's mesh path.
