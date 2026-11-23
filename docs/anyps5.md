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
   pipeline.

   A console compute shader in the game's own container (DXBC → DXIL part → XBCH header + zlib
   FSSM + NOBC stub) becomes one when its PSO is created (`console_fssm.c`; on by default,
   `REBOX_CONSOLE_COMPUTE=0` turns it off). The FSSM header fields were measured on all 2,555
   compute shaders of `the shader archive` against the PS09 cache, matched by the 24-byte shader ID at
   0x150:

   | Offset | Field | Agreement with PS09 |
   |---|---|---|
   | 0x104 | float mode | 2,472 |
   | 0x138 | `COMPUTE_PGM_RSRC2` | user SGPR count in all; the enables and LDS describe this code, PS09 the driver's recompile |
   | 0x13C, 0x140, 0x144 | thread-group size | all |
   | 0xD4 | bit 5: wave32 (clear: wave64), from americas-recomp | of the 2,598 matched, the 1,464 with it set are all wave32 by `sa5_analyse` and 828 of the rest wave64; the analysis defaulted the other 306 to wave32 |
   | 0x168 | code (u32 at 0xC0 plus 0x28) | |

   `RSRC1` isn't in the header. AnyPS5 needs only its float mode and clamp bits, which PS09 has
   as 0x402c03xx throughout, so it is rebuilt as 0x402c0000 plus the float mode. The wave size
   comes from 0xD4; SCRDs without one (wave 0) are inferred with `sa5_analyse`. The console-cs
   plan takes it from the game's containers too (`tools/console-cs-plan.py DUMP CONTAINERS`),
   since the analysis can't tell a wave64 shader with no wave-wide instructions from a wave32 one. zlib comes
   from zlib's `contrib/puff` (zlib licence, `shims/d3d12_xs/puff.c`).

   Pixel-shader containers (facts from Leonida, GPLv3, `docs/design.md` rows "Pixel-shader
   interpolants" and "PS interpolation modes", 2026-11-22; our own check agrees on the code): code
   also starts at the u32 at 0xC0 plus 0x28. From 0x138, 8-byte input records run while their first
   byte is 6; a record's position is the `v_interp` attribute number, bits 0-1 of byte 7 are the
   mode (1 flat, 2 perspective-centre; 0 and 3 never occur) and bit 4 marks a 16-bit interpolant.
   That gives `SPI_PS_INPUT_CNTL` (offset = position, `FLAT_SHADE`, `FP16_INTERP_MODE`). There is no
   `SPI_PS_INPUT_ENA` word: every interpolated input is `PERSP_CENTER`, so the enable comes from the
   code. Graphics containers don't carry the PS09 shader ID at 0x150 (only CS and RT matched, 2,676),
   and PS09's PS code is the driver's recompile, so the dump's bytes don't sit at that offset; a
   deferred PS built from the container uses the container's own code. RSRC1/2 for PS aren't
   located yet.

   `smoke.exe --fssm-check DIR` converts every compute container in a carve: all 2,573 in
   `the shader archive` convert. Whether the exe passes these containers at PSO creation, or PS09 cache
   blobs, is still open question 1. The `FLAT_SCRATCH` setup many console shaders open with
   (`s_setreg_imm32_b32` or `s_setreg_b32` to `HW_REG_FLAT_SCR_LO/HI`) becomes same-size
   `s_nop`s before each compile. Only the scalar prologue is scanned (up to the first vector or memory instruction: a UE5 title's pixel shaders move M0 first), and the first time is logged.
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
   - turns host buffers into BDA pointers read from push bytes 128-191 (up to 8).
   It validates the module, which is then cached by AnyPS5's pipeline identity plus the slots.
4. **Bind**. vkd3d-proton makes one pipeline per key, with the root signature's own layout, so
   its bindless sets are already bound. After its descriptors are updated, it pushes the
   192 bytes from the compile (AnyPS5's 128 bytes of push data, then the host-buffer addresses)
   and marks its own root parameters dirty for the next ordinary pipeline.

What AnyPS5's driver would do and this host does instead, from its sources:

| AnyPS5 binding role | Here |
|---|---|
| Guest buffers (storage buffers: every V#, constant buffers included) | not descriptors: a table of {address, length} built from the V# words, read through BDA, so each element is a pointer to its buffer (buffers' GPU VAs are device addresses). vkd3d-proton keeps CBVs as uniform-buffer descriptors with descriptor buffers, which a storage-buffer binding can't read, and V#s from memory have no heap slot at all |
| Guest images and samplers (set 0, `stage*65 + local`) | the vkd3d-proton descriptor at the slot where the shadow holds the same words, if that slot holds the matching kind of view (the shadow records each slot's: a mismatch would read the wrong descriptor type from a mutable set, and crashed lavapipe). AnyPS5 may rewrite a T#'s `DST_SEL` and gives samplers point-filtered twins, so matching can ignore those fields |
| Samplers in no heap (built from literals in the code, or a twin) | decoded from the S# into a real sampler in the heap's hidden tail: every shader-visible heap gets 256 extra slots the title doesn't see (`GetDesc` hides them; patch 0005 raises vkd3d-proton's sampler-heap limit by 256) |
| A storage image also bound as a sampled image (size queries) | an SRV of the same texture and view in the hidden tail |
| FlattenedSrt (binding 61): every dword the shader's scalar loads read through user SGPRs | its words, in GPU memory shared by content |
| BDA page table (59) | only memory that exists: `ReboxGpuAlloc` chunks and the buffers the title made views of, at their GPU VAs, plus each shadow heap's title address mapped to its GPU copy. A stray address then becomes an AnyPS5 fault record ("unmapped address") instead of a GPU page fault. AnyPS5 checks the table's length in words is exactly 4 + 8 × count |
| Fault buffer (60), GDS (58) | one of each for the process, zeroed GPU memory as AnyPS5's driver keeps them. The fault record and the SRT guard word (set when a speculated descriptor turns out wrong at run time) are read at each compile, logged (the first 20 of each) and cleared. Faults come from earlier dispatches, so the log doesn't name the shader |
| ShaderData (62) | its words, as FlattenedSrt (compute normally uses push data instead) |
| Image and sampler tables (261/260/63): T#s and S#s a shader indexes at run time, which AnyPS5 finds by walking tables in guest memory | bound like guest images and samplers, each element at the heap slot holding its words; the map (64) is host data like the flattened SRT. Untested with real entries: with the lab's synthetic inputs AnyPS5 finds none, and an empty table is refused with a message |
| LDS spill (set 1), scratch (set 2) | not yet: the compile fails and the dispatch is skipped, logged once per shader |

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

## Tests moved from xsx-shader-lab

These came from the user's lab and are GPL-2.0-only here:
- the layout analysis (`rdna-run --analyse`, now `anyps5/layout.cpp` behind `sa5_analyse`);
- the PS09 reader and manifest step (`tools/ps09.py`);
- the hand-written gfx1030 repros (`tests/rdna/*.s`, assembled into
  `generated/repro_shaders.h` by `tools/gen_repro_shaders.sh`).

The smoke test runs the repros through the deferred path:
- the XOR-swapped table pointer (patch 0003);
- three speculated pointers and descriptors (patch 0004);
- two exec-masked loops through LDS, whose output is checked.

The loop repro is a known lavapipe-only miscompare (8-wide subgroups split a guest wave; exact on
RADV), so on lavapipe it is reported, not failed. The lab keeps its native runners (`xsxrun`,
`rdna-run`), its per-shader Python models and the shader-3094 bisection. That bisection is
disassembled game code, so it can't be committed.

Patch 0004's speculation is on by default: `d3d12_xs` sets `APS5_SPECULATE_DESCRIPTORS` when it
loads, before anything loads AnyPS5. The patch reads it once, into a static.
`REBOX_ANYPS5_SPECULATE=0` turns it off. A wrong guess sets the fault buffer, which `d3d12_xs`
reads back and logs. The smoke test checks that the repro whose clobbering path runs reports one
fault, and that the others report none.

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
   shaders: nothing on the container side (console compute containers become `SCRD`s at PSO
   creation). Heap
   slot 10 now returns title addresses, for pointers the title stores in memory itself. Then
   guest memory beyond the shadow heaps for SRT walks (identity-mapped GPU memory). Then the
   roles marked "not yet", fault-buffer readback, and `SpirvTarget` from the device.
4. Real the target title compute shaders: `tests/run-console-cs.sh DUMP [FIRST [COUNT]]` runs every compute
   shader in PercyStar's dump of `the shader archive` (2,684, from the PS09 cache) through the whole path
   under Wine (`smoke.exe --console-cs`):
   - `tools/console-cs-plan.py` reads each shader's `RSRC1`/`RSRC2` and thread-group size from
     the PS09 cache (`tools/ps09.py`).
   - `smoke.exe` infers its descriptor tables with `sa5_analyse` (`anyps5/layout.cpp`).
   - It rebuilds the tables in real D3D12 heaps from synthetic resources, passes every user SGPR
     as a root constant, and gives table pointers as title addresses. Table pointers are single
     32-bit SGPRs, sometimes adjacent, which root-order packing can't express.

   Inputs are synthetic, so this checks compiling, binding and running without faults, not output
   values. Skipped: shaders that bind a slot the inferred layout left empty, and one whose layout
   puts a buffer where it reads an image. Both are artefacts of the inference. The largest shaders
   (around 400k SPIR-V words) take llvmpipe about a minute each, so a lavapipe sweep takes hours.
   The first 110 on lavapipe: 102 dispatched, 8 skipped. The 890M (2026-11-22), first run: 232 of
   the first 290 dispatched, 57 skipped, then a device loss on `sh_01093`, a BDA-path shader. The
   page table then mapped the whole address space and had a wrong length, so every access through
   it was invalid. Both are fixed, not yet re-run on hardware. Skips there: 31 bind a slot the
   inferred layout left empty, 15 have a buffer V# where an image is read (layout artefacts), and
   11 need role 10 (image table maps: runtime-indexed texture tables, not supported yet). lavapipe
   segfaults on `sh_01092`, which RADV runs. `smoke.exe --console-cs` reports each shader's AnyPS5
   faults on its line.
5. Graphics stages (2026-11-23): deferred console vertex and pixel shaders, compiled at each draw.
   - **PSO creation**: a graphics PSO whose vertex and pixel shaders are console containers still
     gets the fallback shaders, and `d3d12_xs` attaches both stages as `SCRD` version 2 blobs
     (`console_fssm_graphics`: code, RSRC1 and what the code says, as `tools/fssm-*-plan.py` do)
     plus the input layout, as private data (`REBOX_GRAPHICS_GUID`, `struct rebox_graphics`).
     `REBOX_CONSOLE_GRAPHICS=0` turns it off.
   - **Draw** (vkd3d-proton patch 0006): at the first draw the PSO's private data is read; its
     vertex buffers join the PSO's mask and it pushes the console block. At each draw both stages
     are compiled through `ReboxConsoleCompile` with user SGPRs as TomBox reads the console's
     graphics layout (root parameters from s8, one SGPR per descriptor table, two per root
     descriptor: a UE5 title's vertex shaders load their constant buffer's V# through s[12:13] with
     s13 = 0; the fetch shader's address at s[4:5]). Vertex shaders get 32 user SGPRs, which
     RSRC2 holds with `USER_SGPR_MSB` (bit 27): the 5-bit field alone read 0, so AnyPS5 saw only
     s0-s5 and every constant buffer resolved to a null V# (a UE5 title's runs drew nothing). The vertex
     stage via `sa5_compile_vertex`, whose attributes (location r = input layout element r) make
     the vertex input; push data is the vertex stage's 128 bytes then the pixel stage's: AnyPS5's 112
     bytes, then the addresses of up to two host buffers whose contents change per draw (the guest
     buffer table, the flattened SRT, shader data, image table maps; `sa5_host_buffer.push_base`).
     The process-wide host buffers (fault buffer, GDS, page table) are baked into the modules
     (`sa5_host_buffer.address`). Baking everything made a module and a pipeline per draw, and
     a UE5 title's render thread stopped presenting while it compiled them. The pipeline is
     vkd3d-proton's own variant with those two modules and that vertex input, cached per PSO by
     both compiles' keys and the pipeline key. A failed compile draws with the fallback pipeline.
   - **Interpolants** are linked as the console driver links them: `v_interp attrN` and `exp paramN`
     name signature registers, numbered by what each shader uses, not elements. The FSSM's interface
     tables give them (xcl's layout, GPL-2.0-only: sub-version at +0x10; pixel records at +0x130
     with SPI_PS_INPUT_ENA, vertex records at +0x1D4), and `d3d12_xs` sets
     `SPI_PS_INPUT_CNTL_n` to the VS param whose OSG1 register has the semantic of the PS's ISG1
     register for attribute n. Taking attr n = param n made a UE5 title's Slate pixel shader (COLOR0 as
     attr0, TEXCOORD1 as attr1) sample its textures at the wrong coordinates: a white panel, no text.
   - `test_console_draw` (smoke): a console vertex shader that calls the fetch and exports input 0
     as the position, scaled by a constant buffer it reads through descriptor table 0 (s8), and a
     pixel shader exporting red, in real containers, draw a full-screen triangle on lavapipe.

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
- **The real user-SGPR map: per shader, in the PS09 records.** Each shader in the PS09 cache
  carries binding records: kind (SRV, UAV, CBV, sampler, root table), register (`slot_id`), a
  dword offset (`byte_offset`, despite the name), and a packed word. TomBox read the packed word
  as descriptor dword count in bits 0-7, user SGPR in bits 8-15 and register space from bit 16.
  Checked across the corpus (2026-11-22) against the tables `sa5_analyse` infers from the code:
  68,991 of 73,512 inferred slots (94%) have a record at exactly that user SGPR and dword
  offset. The kinds agree: textures are SRVs, storage images UAVs, constant buffers CBVs or SRVs.
  The misses are mostly samplers, which seem to be described elsewhere. Records also exist for
  bindings the code never touches. So the console maps registers to SGPRs per shader, not in
  root-parameter order. Once we know how a PSO gets its shader (open question 1), patch 0005
  should pack SGPRs from these records: for each record, the root parameter whose range covers
  its register and space, its table address plus the offset. That replaces the root-order guess.
- **Graphics register state is in the PS09 cache.** Each graphics PSO record (0x70 bytes)
  names metadata blobs:
  - its head word 2 is a register block, its word 3 the input signature (semantic names such
    as `POSITION`, `COLOR`, `TEXCOORD`);
  - its seven stage slots each pair a 20-byte blob with that stage's binding records.

  americas-recomp guesses all of this register state (`inputAddr`, export formats, float mode,
  mesh configuration). Measured 2026-11-22 on the 1,161 PSOs whose register block is 388 bytes
  (the other sizes, 396-480, hold more sections and shift the offsets), against the dumped PS
  code:

  | Word | Register (GFX10) | Agrees with the code |
  |---|---|---|
  | 14, 15 | `SPI_SHADER_PGM_RSRC1/2_VS` | all 1,328 dumped VS: user-SGPR count covers every binding record's SGPR, float mode 0xC0/0xCC |
  | 19, 20 | `SPI_SHADER_PGM_RSRC1/2_PS` | all 9,519 dumped PS, as above |
  | 24, 25 | `SPI_SHADER_PGM_RSRC1/2_GS` of NGG pipelines | all 7,498 dumped NGG shaders, as above |
  | 22 | `SPI_PS_INPUT_ENA` (0x2 perspective centre, bit 15 fixed-point position) | not yet checked |
  | 23 | `SPI_PS_IN_CONTROL.NUM_INTERP` | 1,149 |
  | 28 | `SPI_SHADER_COL_FORMAT` (a nibble per target; 4 = FP16, matching `compr` exports) | 1,155 |
  | 30 | `CB_SHADER_MASK` | 1,141 |
  | 52-59 | `CB_BLENDn_CONTROL` (0x80000000 = no blend) | by value |
  | last 3 + n, padded to even | `SPI_PS_INPUT_CNTL_0..n-1` (n = `NUM_INTERP`): offset, `FLAT_SHADE` (bit 10), `FP16_INTERP_MODE` (19), `ATTR0/1_VALID` (24, 25) | block size = base + these words in every PSO (380 + 8 per 2 interpolants; 464 in the NGG family) |
  | 64-71 | `SX_MRTn_BLEND_OPT` | by value |

  Stage slots: 0 is the pixel shader, 1 the vertex shader, 2 the NGG (mesh/primitive) shader.
  The block sizes form two families:
  - 388/396 bytes (VS + PS) use the offsets above;
  - 464-520 bytes (NGG + PS) keep the `RSRC` words in place, but the pixel-output group moves
    21 words later: `NUM_INTERP` is word 44 and `COL_FORMAT` word 49. `NUM_INTERP` is at least
    the number of interpolants the PS reads in all 6,756 such PSOs with a dumped PS. It counts
    what the earlier stage outputs, so it can exceed what the PS reads; equality holds where they
    match, e.g. 1,491 of 1,542 472-byte blocks. The size differences come after word 49.

  `sa5_compile_pixel` (`rebox_anyps5.dll`) builds AnyPS5's `ShaderPixelStageInfo` from these
  words. `tools/ps-plan.py` reads them for every dumped PS, and `sa5_check ps PLAN` compiles each
  and runs `spirv-val`. The sweep doesn't give AnyPS5 guest memory, so shaders that walk
  descriptors fail there; a dispatch-time compile has the memory. Still from the code, not
  registers: the wave size (`sa5_analyse`) and depth export (an `mrtz` export). Next: the NGG
  `MeshConfiguration`, then graphics pipelines at draw time.
- **a UE5 title's pixel shaders (no PS09 cache).** Its graphics FSSM (older layout) has the code at
  the u32 at 0xC0 plus 0x28 (0xF10 in all its pixel shaders), the code size in bytes at 0xC8,
  `SPI_SHADER_PGM_RSRC1_PS` at 0x108 and the float mode at 0xFC; pointers at 0xE0, 0xE8, 0x118
  and 0x128 (plus 0x28) lead to binding records, an input/output record table and the entry
  name (e.g. `GaussianBlurMain`). The rest of the pixel state comes from the code
  (`tools/fssm-ps-plan.py`): 24 user SGPRs (the prologue's `s_mov_b32 m0, s24` takes the
  primitive mask from the SGPR after them), interpolants from `v_interp_*`, flat ones from
  `v_interp_mov`, export formats from `exp` (packed: FP16, else the 32-bit format of the channels
  written), depth from an MRTZ export. 2026-11-23: 32 of the 33 dumped from a run compile with
  `sa5_check ps` and pass `spirv-val`; the other writes a hardware register field with
  `s_setreg_imm32_b32` that AnyPS5 doesn't handle (only MODE's round and denormal fields).
- **a UE5 title's vertex shaders.** Each opens with `s_swappc_b64 s[4:5], s[4:5]` into the fetch
  shader the console driver builds from the PSO's input layout (not in the container), which leaves
  input r in `v[4+4r..7+4r]`; the code reuses `s4` as scratch right after. `sa5_compile_vertex`
  (`rebox_anyps5.dll`) gives AnyPS5 the inputs (a V# and those VGPRs each) in its embedded-fetch
  mode, so each becomes a Vulkan vertex attribute (`sa5_result_vertex_input`), and s[4:5] a fetch
  address outside the program. AnyPS5 patch 0006 accepts a fetch call that links into its own
  pointer pair and lets the code reuse it. `tools/fssm-vs-plan.py` reads the input count from the
  code; `sa5_check vs PLAN` compiles each with synthetic RGBA32F buffers (`SA5_USER` and
  `SA5_MEMORY` give a draw's user SGPRs and descriptors, as `d3d12_xs` logs them, and list the
  bindings AnyPS5 resolves). 2026-11-23: all 11
  dumped from a run compile and pass `spirv-val` (1-5 inputs each).
- **Graphics on Scarlett.** The 7,498 NGG shaders may not map cleanly onto AnyPS5's mesh path.
