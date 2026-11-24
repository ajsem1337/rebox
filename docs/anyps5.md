# Console shaders with AnyPS5

Console shaders are RDNA2 machine code. Rebox compiles them to SPIR-V with AnyPS5's recompiler
(`rebox_anyps5.dll`, built from AnyPS5 `fc973e0189` with `patches/anyps5/`) when they are first
dispatched or drawn, and binds the result through vkd3d-proton ([shaders.md](shaders.md) has the
vkd3d-proton side).

## What AnyPS5 needs from a host

| | AnyPS5 |
|---|---|
| Compile input | code words, stage, user SGPR values, compute stage info (from `RSRC1`/`RSRC2`: user SGPR count, TGID enables, thread-id components, LDS size, float mode), and **guest memory**: a snapshot region or a read callback (`SrtRuntime::readMemory`) |
| Why memory | the recompiler walks the descriptor tables the SGPRs point at, resolves each T#/V#/S# and specialises on them (formats, dimensions, compare/gather) |
| Result | SPIR-V plus specialisation constants; `bindings[]` with kind, set/binding (`stage*65 + local`, set 0, or 1 for fragment) and the descriptor **by value** (4 or 8 raw words); 128-byte push data; ShaderData (16 KB: user data, buffer offsets, image/sampler metadata); a BDA page table and a fault buffer; required subgroup size |
| Per draw | a fresh resource snapshot. A cached result is patched when only V# words, user data or the flattened SRT change; new T#/S# values or V# fields that feed specialisation need a new result (possibly a new module) |

So compiles happen at the first draw or dispatch that uses a pipeline, when the real descriptors
are known, and results are cached by AnyPS5's own variant key. AnyPS5's own driver (its Vulkan
device, texture cache and descriptor decoding) isn't used: vkd3d-proton owns the device and every
resource the title created, and each binding maps to the vkd3d-proton descriptor the title made.

## The shadow heap

AnyPS5 reads descriptors as the console writes them (GFX10 layout); vkd3d-proton's heaps hold
what RADV writes (GFX11 on the 890M). `d3d12_xs` keeps a **shadow heap** per CBV/SRV/UAV and
sampler heap, in console format (`shims/d3d12_xs/shadow.c`, encoders in `console_desc.c`):

- On every descriptor write through the API (device slots 17-19, 22-24), `d3d12_xs` encodes the
  view as a GFX10 T#/V#/S# into the shadow at the same index, in the field layout AnyPS5's
  decoders read. Buffers carry their GPU address; textures a synthetic address unique per
  resource. `CopyDescriptors*` copy shadow bytes, so CPU-only heaps have shadows too. The stride
  is the increment the title gets: at least 32 bytes (16 for samplers).
- The address the title sees for a heap (heap slot 10) is the shadow's title address, in a 32-bit
  range from `0x10000000`, 64 KiB aligned (`REBOX_HEAP_VA`, README). The title's shaders truncate
  descriptor-table pointers to 32 bits, and AnyPS5 resolves them on the CPU through the read
  callback.
- Descriptors a title writes into heap memory itself land in the shadow in console format, as on
  the console.

A binding maps back to a real descriptor through a CPU hash over every shadow slot, keyed on its
console words: `ReboxShadowFind(words)` gives the vkd3d-proton CPU handle written alongside them
(shader-visible heaps first). The exports for the dispatch path and the tests are
`ReboxShadowHeap(cpu_start, &title_va, &data, &stride, &count)`, `ReboxShadowRead(title_va, out,
size)` (fails unless the range lies in one heap) and `ReboxShadowFind(words, word_count, sampler,
&cpu)`. Shadows sit in GPU-visible memory (`ReboxGpuAlloc`).

## Console containers

A console shader arrives as DXBC whose `DXIL` part is an `XBCH` header and a zlib stream (a `NOBC`
stub where LLVM bitcode would be) holding **FSSM**: GFX10.3 code plus its register setup
(`console_fssm.c`; zlib from zlib's `contrib/puff`, `shims/d3d12_xs/puff.c`). The compute header
fields were measured on 2,555 compute shaders of one title against its PS09 cache, matched by the
24-byte shader ID at 0x150:

| Offset | Field | Agreement with PS09 |
|---|---|---|
| 0x104 | float mode | 2,472 |
| 0x138 | `COMPUTE_PGM_RSRC2` | user SGPR count in all; the enables and LDS describe this code, PS09 the driver's recompile |
| 0x13C, 0x140, 0x144 | thread-group size | all |
| 0xD4 | bit 5: wave32 (clear: wave64), from americas-recomp | of 2,598 matched, the 1,464 with it set are all wave32 by `sa5_analyse`, 828 of the rest wave64 |
| 0x168 | code (u32 at 0xC0 plus 0x28) | |

`RSRC1` isn't in the header; AnyPS5 needs only its float mode and clamp bits, which PS09 has as
0x402c03xx throughout, so it is built as 0x402c0000 plus the float mode. A shorter layout (Unreal
Engine 5 titles, SDK 0x20002; its code at 0x158) has the same fields 8 bytes earlier: float mode
at 0xFC, `RSRC2` at 0x130, the thread-group size at 0x134-0x13C, the shader ID at 0x140; its wave
size comes from `sa5_analyse`, since the analysis can't tell a wave64 shader with no wave-wide
instructions from a wave32 one where the container says.

Graphics FSSMs (the shorter layout) carry less register state: the code at the u32 at 0xC0 plus
0x28 (its size in bytes at 0xC8), `SPI_SHADER_PGM_RSRC1` at 0x108, the float mode at 0xFC. The
rest comes from the code:

- **vertex**: the first instruction calls the fetch shader (`s_swappc_b64 s[4:5], s[4:5]`) the
  console driver builds from the PSO's input layout, which leaves input r in `v[4+4r..7+4r]`; the
  inputs are the consecutive slots from v4 read before being written. No call, no inputs
  (full-screen passes). 32 user SGPRs, which `RSRC2` holds with `USER_SGPR_MSB` (bit 27).
- **pixel**: 24 user SGPRs (the prologue's `s_mov_b32 m0, s24` takes the primitive mask from the
  SGPR after them); interpolants from `v_interp_*` (flat: only `v_interp_mov`); export formats
  from `exp` (packed: FP16, else the 32-bit format of the channels written); depth from an MRTZ
  export.
- **interpolants** are linked as the console driver links them: `v_interp attrN` and
  `exp paramN` name signature registers numbered by what each shader uses. The FSSM's interface
  tables give them (xcl, GPL-2.0-only: sub-version at +0x10; pixel records at +0x130 with
  `SPI_PS_INPUT_ENA`, vertex records at +0x1D4), and `SPI_PS_INPUT_CNTL_n` is the VS param whose
  OSG1 register has the semantic of the PS's ISG1 register for attribute n.

The `FLAT_SCRATCH` setup many console shaders open with (`s_setreg_imm32_b32` or `s_setreg_b32`
to `HW_REG_FLAT_SCR_LO/HI`) becomes same-size `s_nop`s before each compile. Only the scalar
prologue is scanned, up to the first vector or memory instruction.

The console driver's own container, **FSPX** in a DXBC part `ACCC`, is read too: AMD FidelityFX's
console upscaler creates its compute passes from it, with no DXIL (format from xcl). After `FSPX`,
a version, an entry count and a size come 16-byte entries (type, offset, size, 0) from +0x10. Entry
0 holds the thread-group size (three u16 at +0x0C) and `COMPUTE_PGM_RSRC1`/`RSRC2` (+0x14, +0x18);
entry 6 the code behind a 0x24-byte header, up to its last `s_endpgm`.

## Compute

1. **PSO creation** (patch 0005). A console compute container becomes a deferred console shader,
   an `SCRD` blob (`shims/d3d12_xs/console_compile.h`): header, `RSRC1`/`RSRC2`, wave size,
   thread-group size, code address, then the code. vkd3d-proton keeps the blob with the PSO and
   creates no pipeline. `REBOX_CONSOLE_COMPUTE=0` turns this off.
2. **Dispatch.** User SGPRs are packed in the console's layout (patch 0007: root parameters from
   s4, one SGPR per descriptor table, two per root descriptor, one per constant), with descriptor
   tables pointing at the shadow heaps' title addresses (recorded at `SetDescriptorHeaps` through
   `ReboxShadowHeap`). vkd3d-proton calls `d3d12_xs`'s `ReboxConsoleCompile` with the blob, the
   SGPRs, the bound heaps and, for each AnyPS5 descriptor kind, the bindless set and binding that
   holds it.
3. **Background compile.** A program's first compile (by its blob's hash) goes to a worker
   thread, and the call fails at once (`E_PENDING`): vkd3d-proton skips the dispatch, or draws
   with the PSO's fallback. Once compiled, AnyPS5's memo and the caches here make later calls
   quick, and they compile in the calling thread; a call that would wait for the worker's lock is
   skipped too. `REBOX_CONSOLE_ASYNC=0` compiles in the calling thread every time.
4. **Compile** (`console_compile.c`). AnyPS5 compiles against the shadow heaps and `d3d12_xs`'s
   GPU memory chunks (at their GPU VAs) through its in-memory cache, which parses the code once
   and makes a variant per new set of descriptors. Each guest binding's elements are found in the
   bound heap's shadow, which gives heap slots. `sa5_finalize` (`anyps5/spirv_remap.cpp`) then
   freezes the specialisation constants, moves each guest descriptor array onto the bindless set
   as a runtime array indexed by those slots, turns host buffers into BDA pointers read from push
   bytes 128-191 (up to 8), and validates the module. Modules are cached by AnyPS5's pipeline
   identity plus the slots.
5. **Bind.** vkd3d-proton makes one pipeline per key with the root signature's own layout, so its
   bindless sets are already bound. After its descriptors are updated it pushes the 192 bytes from
   the compile and marks its own root parameters dirty for the next ordinary pipeline.

How each AnyPS5 binding role is served:

| AnyPS5 binding role | Here |
|---|---|
| Guest buffers (storage buffers: every V#, constant buffers included) | a table of {address, length} built from the V# words, read through BDA (buffers' GPU VAs are device addresses). vkd3d-proton keeps CBVs as uniform-buffer descriptors, which a storage-buffer binding can't read, and V#s from memory have no heap slot |
| Guest images and samplers (set 0, `stage*65 + local`) | the vkd3d-proton descriptor at the slot where the shadow holds the same words, if that slot holds the matching kind of view (the shadow records each slot's kind). AnyPS5 may rewrite a T#'s `DST_SEL` and gives samplers point-filtered twins, so matching ignores those fields |
| Samplers in no heap (built from literals in the code, or a twin) | decoded from the S# into a real sampler in the heap's hidden tail: every shader-visible heap has 256 extra slots the title doesn't see (`GetDesc` hides them) |
| A storage image also bound as a sampled image (size queries) | an SRV of the same texture and view in the hidden tail |
| FlattenedSrt (binding 61): every dword the shader's scalar loads read through user SGPRs | its words, in GPU memory shared by content |
| BDA page table (59) | the memory that exists: `ReboxGpuAlloc` chunks and the buffers the title made views of, at their GPU VAs, plus each shadow heap's title address mapped to its GPU copy, so a stray address becomes an AnyPS5 fault record instead of a GPU page fault. One table for the process (16,384 ranges, the unused ones empty at the top of the address space; AnyPS5 binary-searches it and checks its length is 4 + 8 × count words), at a fixed address, rewritten in place when what it maps changes |
| Fault buffer (60), GDS (58) | one of each for the process. The fault record and the SRT guard word (set when a speculated descriptor turns out wrong) are read at each compile, logged (the first 20 of each) and cleared. Faults come from earlier dispatches, so the log doesn't name the shader |
| ShaderData (62) | its words, as FlattenedSrt |
| A graphics stage's T# that is empty when the draw is recorded | the dummy texture, and its address (the T# the code's image instructions use n-th, in first-use order as AnyPS5 numbers a binding's elements, traced to the `s_load_dwordx8` off a table SGPR that loaded it) noted: at the next submissions the descriptor there is read again and, once the title has written it (the target title fills a per-draw table after recording the draw, as the console GPU reads it only when it runs), its view's heap slot is written into the draw block |
| A graphics stage's V# that is empty when the draw is recorded | an empty buffer (reads zero), and its address (the n-th empty V# the code loads, from its `s_load_dwordx4`s off the table SGPRs) noted: at the next submissions the V# there is read again and its address and length are written into the stage's guest buffer table (a video shader's constants come so, with its T#s; read as zero, every frame was black) |
| Image and sampler tables (261/260/63): T#s and S#s indexed at run time, found by walking tables in guest memory | bound like guest images and samplers, each element at the heap slot holding its words; the map (64) is host data. An empty table is refused with a message |
| LDS spill (set 1), scratch (set 2) | not supported: the compile fails and the dispatch is skipped, logged once per shader |

The `SpirvTarget` lists the capabilities AnyPS5 gates on (MinLod among them:
`image_sample_d_cl` needs it), limited to what RADV and lavapipe both have.

## Graphics

- **PSO creation.** A graphics PSO whose vertex and pixel shaders are console containers gets
  the fallback shaders, and `d3d12_xs` attaches both stages as `SCRD` version 2 blobs
  (`console_fssm_graphics`) plus the input layout, as private data (`REBOX_GRAPHICS_GUID`,
  `struct rebox_graphics`). PSOs made from pipeline streams (`CreatePipelineState`) get the same
  when the stream's only stages are a vertex and a pixel shader; mesh and tessellation pipelines
  keep their fallbacks. `REBOX_CONSOLE_GRAPHICS=0` turns this off.
- **Draw** (patch 0006). At the first draw the PSO's private data is read; its vertex buffers
  join the PSO's mask. At each draw both stages compile through `ReboxConsoleCompile`, with user
  SGPRs in the console's graphics layout (root parameters from s8, one SGPR per descriptor
  table, two per root descriptor, then the start of each heap the root signature indexes directly,
  resource then sampler, one SGPR each (patch 0012); the fetch shader's address at s[4:5]). The vertex stage goes
  through `sa5_compile_vertex`, whose attributes (location r = input layout element r) make the
  vertex input. Push data is the vertex stage's 128 bytes then the pixel stage's: AnyPS5's 112
  bytes, then the address of the stage's **draw block** (`sa5_finalize_draw`): the addresses of
  the host buffers whose contents change per draw (guest buffer table, flattened SRT, shader
  data, image table maps; up to 8), then the heap slot of every element of every remapped
  descriptor array. Draw blocks come from a 128 MiB GPU ring. Nothing that changes per draw is in
  the module, so a shader keeps one module and one pipeline when the title copies its descriptors
  to new heap slots every draw. The process-wide host buffers (fault buffer, GDS, page table) are
  baked into the modules (`sa5_host_buffer.address`).
- **Memo.** Graphics stages compile through AnyPS5's resource capture and result memo
  (`CaptureResources`, then `Recompile(request, capture)`): a draw whose captured resources match
  an earlier one's reuses that result with its per-draw data patched in. Results are held by
  pointer and their SPIR-V words made only for a new module.
- **Pipeline.** vkd3d-proton's own variant with those two modules and that vertex input, cached
  per PSO by both compiles' keys and the pipeline key. A failed compile draws with the fallback
  pipeline. `REBOX_GRAPHICS_DEBUG` (README) swaps in magenta pixel shaders or full-screen vertex
  shaders to see whether draws reach the image.

## Tests

The smoke test runs the whole path on lavapipe:

- the encoders field by field, then a hand-assembled RDNA2 shader (`s_load` of a T#, V# and S#
  from two descriptor tables, `image_sample_lz`, `buffer_store`) compiled through
  `rebox_anyps5.dll` against the shadow heaps: each binding AnyPS5 makes maps back to the
  vkd3d-proton descriptor;
- `test_console_dispatch`: a deferred PSO from RDNA2 code that stores `0x1234abcd` through the V#
  at the start of its descriptor table, dispatched and read back; again with the V# behind a
  pointer in root constants into `ReboxGpuAlloc` memory; and again from a console container
  created as an ordinary compute PSO;
- `test_console_draw`: a console vertex shader that calls the fetch and exports input 0 as the
  position, scaled by a constant buffer read through descriptor table 0 (s8), and a pixel shader
  exporting red, in real containers, draw a full-screen triangle.

The dispatch tests also pass on RADV (Radeon 890M), whose bindless sets differ from lavapipe's
(samplers 0/0, typed views and images 1/1, raw buffers and CBVs 2/0; lavapipe 1/2 for typed views
and images). `REBOX_DUMP_SPIRV=DIR` writes each finalized module.

`anyps5/layout.cpp` (behind `sa5_analyse`) infers a shader's descriptor tables and wave size from
its code; `tools/ps09.py` reads the PS09 PSO cache. `sa5_check.exe` compiles shaders through the
DLL outside a title: a built-in one, a dumped one (`CODE.bin SKIP RSRC1 RSRC2 X,Y,Z`), or the
vertex or pixel shaders listed in a plan file (`vs`/`ps PLAN`).

## AnyPS5 patches

Patch 0004's speculation of descriptors and table pointers it can't evaluate is on by default:
`d3d12_xs` sets `APS5_SPECULATE_DESCRIPTORS` when it loads, before anything loads AnyPS5, and
`REBOX_ANYPS5_SPECULATE=0` turns it off. A wrong guess sets the fault buffer, which `d3d12_xs`
reads back and logs. The other patches are listed in
[patches/anyps5/README.md](../patches/anyps5/README.md).

## Where AnyPS5 runs

The recompiler (`core/shader/recompiler`, without AnyPS5's driver) is built as a Windows DLL with
mingw (`.#rebox-anyps5`, from a sparse checkout of AnyPS5 and the three submodules it pins:
SPIRV-Headers, SPIRV-Tools, glslang; only the patches' `core/shader/` hunks are applied). The
guest libc is replaced by an include path and two stream functions, and winpthreads is linked
statically. `d3d12_xs` loads it in-process; its C API is `anyps5/rebox_anyps5.h`.

**Licence.** AnyPS5 is GPL-2.0-only, and a `d3d12_xs` that loads it in-process, with the
vkd3d-proton patch that calls it, is a GPL-2 combination: Rebox is GPL-2.0-only for this reason
([LICENSE](../LICENSE)).

## The PS09 cache

Titles that ship a PS09 PSO cache carry the console driver's register state for each pipeline:

- **Binding records** per shader: kind (SRV, UAV, CBV, sampler, root table), register
  (`slot_id`), a dword offset (`byte_offset`) and a packed word: descriptor dword count in bits
  0-7, user SGPR in bits 8-15, register space from bit 16 (TomBox). Against the tables
  `sa5_analyse` infers from the code, 68,991 of 73,512 inferred slots (94%) have a record at
  exactly that user SGPR and dword offset, with matching kinds; the misses are mostly samplers.
  So the console maps registers to SGPRs per shader, not in root-parameter order.
- **Graphics PSO records** (0x70 bytes): head word 2 is a register block, word 3 the input
  signature; seven stage slots (0 pixel, 1 vertex, 2 NGG) each pair a 20-byte blob with that
  stage's binding records. In the 388-byte register blocks, checked against the dumped code:

  | Word | Register (GFX10) | Agrees with the code |
  |---|---|---|
  | 14, 15 | `SPI_SHADER_PGM_RSRC1/2_VS` | all 1,328 dumped VS |
  | 19, 20 | `SPI_SHADER_PGM_RSRC1/2_PS` | all 9,519 dumped PS |
  | 24, 25 | `SPI_SHADER_PGM_RSRC1/2_GS` of NGG pipelines | all 7,498 dumped NGG shaders |
  | 22 | `SPI_PS_INPUT_ENA` | not checked |
  | 23 | `SPI_PS_IN_CONTROL.NUM_INTERP` | 1,149 |
  | 28 | `SPI_SHADER_COL_FORMAT` (a nibble per target; 4 = FP16) | 1,155 |
  | 30 | `CB_SHADER_MASK` | 1,141 |
  | 52-59 | `CB_BLENDn_CONTROL` | by value |
  | last 3 + n, padded to even | `SPI_PS_INPUT_CNTL_0..n-1` | block size = base + these words in every PSO |
  | 64-71 | `SX_MRTn_BLEND_OPT` | by value |

  Blocks of 464-520 bytes (NGG + PS) keep the `RSRC` words in place, but the pixel-output group
  sits 21 words later (`NUM_INTERP` word 44, `COL_FORMAT` word 49). `sa5_compile_pixel` builds
  AnyPS5's `ShaderPixelStageInfo` from these words.
- PS09's own code is the driver's recompile, so a deferred shader built from a container uses
  the container's code.

## Open questions

- **What a title with a PS09 cache passes at PSO creation**: the FSSM container or a PS09 blob.
  `d3d12_xs` logs each PSO's stage bytecode kinds and `CachedPSO` blob (the first PSOs, then at
  powers of two; `REBOX_DUMP_SHADERS=1` dumps them). A cache blob vkd3d-proton didn't make fails
  creation as a mismatch, so `d3d12_xs` retries without it.
- **SGPRs from the PS09 binding records.** Where a title has them, packing user SGPRs from the
  records (for each, the root parameter covering its register and space, plus the offset) would
  replace the root-order layout.
- **Speculated descriptors** are checked through the fault buffer, which is logged; recompiling
  on a mismatch isn't done.
- **NGG and mesh shaders** may not map cleanly onto AnyPS5's mesh path; mesh pipelines keep their
  fallbacks.
- **`SpirvTarget`** has fixed values; it should come from vkd3d-proton's physical device.
