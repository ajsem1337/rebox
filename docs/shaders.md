# Console shaders: design

> **Superseded translator choice.** The project uses AnyPS5 for shader compilation; see
> [anyps5.md](anyps5.md). The vkd3d-proton patches below (real heap addresses, SGPR packing,
> lookup table) carry over; the Leonida contract, `SCSV` packing and `translate-shaders.sh` do not.

Status: design, nothing below is implemented yet. Written 2026-11-21 after reading the two projects
that translate the target title's own shaders, [Leonida](https://github.com/ItsNotRexed/Leonida) (GPLv3) and
[americas-recomp](https://github.com/realdody/americas-recomp/tree/xbox-series) (GPLv2, an AnyPS5
fork), and vkd3d-proton 3.0.1's source. Facts only; no code from either project.

## The problem

Our plan so far was: translate the console shaders offline with AnyPS5, store the result in the
`d3d12_xs` shader cache as `.dxil`, and let vkd3d-proton compile it like any PC shader
(`tools/anyps5_cache.py`, README step 3). That doesn't work, for two reasons:

1. **The translators emit SPIR-V, not DXIL.** vkd3d-proton accepts only DXBC/DXIL. Its only SPIR-V
   input is a debugging override (`VKD3D_SHADER_OVERRIDE`, by blob hash). That path drops wave size
   and workgroup metadata, and the module must match vkd3d's internal binding layout.
2. **Console shaders don't bind resources the D3D12 way.** A console shader gets user SGPRs (root
   constants and 64-bit pointers), loads raw hardware descriptors from memory with
   `s_load_dwordx8` (T#: images, S#: samplers, V#: buffers), and decodes them itself. A translated
   shader has to be given those SGPR values and has to turn descriptor bytes into Vulkan resources.
   Neither of those is something vkd3d-proton does.

So shaders need a runtime contract and an integration point. Both are decided here.

## What the two translators expect

| | Leonida | americas-recomp (AnyPS5) |
|---|---|---|
| Coverage of the 26,503 shaders | all pass `spirv-val`; correctness checked on synthetic tests only | compute 96.7%, pixel 58% strict, vertex 61%, mesh 96% |
| When it compiles | fully offline | offline compile, but bindings and success depend on descriptor bytes in memory |
| SGPRs | 144-byte push constants: s0-s31, `heap_base`, `code_base` | push data and a ShaderData buffer with `userData[128]` |
| Buffers (V#) | decoded in the shader, accessed by buffer device address | resolved on the CPU into VkBuffers per draw |
| Images and samplers (T#, S#) | the shader hashes the raw 8 dwords and looks them up in a GPU hash table that indexes bindless arrays | resolved on the CPU per draw from a memory snapshot; runtime-indexed tables through a CPU-built map buffer |
| Host runtime needed | hash-table maintenance, bindless arrays, root-signature to SGPR packing | AnyPS5's AGC driver: about 7,600 lines of binding code |
| Vertex input | PSO input layout, location = D3D input register | PSO input layout plus vertex-buffer V#s and the fetch subroutine |

Both key shaders by the FSSM payload, not by the DXBC blob. The container is DXBC → DXIL part
with an `XDAT` table → `XBCH` header plus zlib → `FSSM` (code at u32@+0xC0 + 0x28). Our cache
hashes the whole blob, so that needs changing anyway.

**Choice: Leonida's contract.** It is the only one that compiles fully offline and keeps the per-draw
runtime small: pack SGPRs, keep a table of descriptors. AnyPS5's contract needs its driver at draw
time, which is GPL code we would have to embed in our shims. Both translators stay separate
offline programs run by the user. Their SPIR-V output is data that our runtime loads, so no GPL
code ends up in our DLLs. The contract is a small, stable interface, so the translator can be
swapped or compared later.

## Integration point: inside vkd3d-proton

There are two places this could live:

- **Beside vkd3d-proton:** `d3d12_xs` records console draws itself into vkd3d's command buffer
  (`ID3D12GraphicsCommandListExt::GetVulkanHandle`). vkd3d-proton only supports that for work
  outside its own render passes (DLSS-style compute). Graphics draws would fight its lazy
  render-pass, barrier and dynamic-state tracking. Rejected.
- **Inside vkd3d-proton (chosen):** build vkd3d-proton from source in the flake with patches, as we
  already do for RADV. Most of what we need is already there:
  - root CBVs, SRVs and UAVs are buffer device addresses in push constants on RADV;
  - shader-visible heaps are descriptor buffers holding **raw RADV hardware descriptors** with a
    32-byte CBV/SRV/UAV stride, the same stride as the console's;
  - heap memory is host-visible, so our identity layer already maps it at GPU VA == CPU VA;
  - wave size control exists per shader.

### The patches

1. **Real heap addresses** (done). `GetGPUDescriptorHandleForHeapStart` returns a fake handle
   (`unique << 33`). Console shaders dereference descriptor table pointers, so the title must see
   the descriptor buffer's real GPU VA. Rather than change vkd3d-proton's handle encoding,
   `patches/vkd3d-proton/0001` only exports `ReboxGetDescriptorHeapVA` from `d3d12core.dll`:
   the real address and stride of a shader-visible heap. That works only with embedded mutable
   descriptors (descriptor buffers), which RADV uses but lavapipe doesn't. `d3d12_xs`'s heap slot
   10 returns that address and records the range. The command-list slots that take a GPU handle map
   it back to vkd3d-proton's handle: root descriptor tables (Xbox 35/36) and UAV clears (Xbox 53/54).
   `REBOX_REAL_HEAP_VA=0` keeps the virtual handles. Whether the address equals the CPU pointer
   (`heap_base = 0`) depends on the layer identity-mapping the descriptor heap's memory type.
2. **Console pipelines** (compute and graphics done, `patches/vkd3d-proton/0002`). A translated
   shader reaches vkd3d-proton as an `SCSV` blob in place of DXBC: a 44-byte header (magic `SCSV`,
   version 1, Vulkan stage bit, wave size 0/32/64, workgroup size, SPIR-V offset and size, DXBC
   offset and size) followed by the SPIR-V and, for graphics stages, the original console DXBC
   container. vkd3d-proton reads I/O signatures from that container, so vertex attribute locations
   are the input signature's registers (as Leonida's contract expects) and stage I/O is validated
   as usual; the translated stages must agree on varying locations themselves. vkd3d-proton uses that SPIR-V as is (entry point `main`) and requests the wave size.
   Every root signature's push-constant range is widened to at least 144 bytes. Console pipelines
   therefore share the root signature's layout and stay compatible with its descriptor sets.
   `d3d12_xs` passes `SCSV` blobs through its shader cache unchanged. Still to do: producing
   `SCSV` from translator output in the cache, and tessellation/mesh stages (untested).
   Originally planned:
   - look up `<stage>/<fssm hash>.spv` in the shader directory;
   - build the pipeline with the root signature's layout plus a console push-constant block
     (s0-s31, `heap_base`, `code_base`) and the bindless sets the contract names;
   - set the required subgroup size from the shader's wave mode;
   - with no translation, fall back as `d3d12_xs` does today.
3. **SGPR packing at draw and dispatch** (done, in 0002; at draws it uses the graphics or mesh
   layout; the push follows vkd3d's own and invalidates it so ordinary pipelines get their data
   back). From vkd3d's root-parameter state, write s0-s31 in root
   order: one dword per 32-bit constant, two per root descriptor or descriptor table (its real
   VA). This is Leonida's default mapping. It is an assumption until the exe shows the real one;
   keep it table-driven so a measured map replaces it.
4. **Descriptor lookup** (done: `d3d12_xs` `lookup.c`, `patches/vkd3d-proton/0003`). Each
   shader-visible CBV/SRV/UAV or sampler heap gets a GPU-visible table mapping descriptor bytes to
   heap index. Layout, in dwords, as Leonida's translator reads it: `[0]` mask (capacity - 1), then
   entries `{ key[8], index, pad }` from dword 1, index `0xffffffff` when empty. The key is the
   descriptor as it sits in the heap: 32 bytes for CBV/SRV/UAV, or 16 bytes for samplers padded
   with `0xffffffff`. Hash: FNV-1a over the 8 dwords (`h = 2166136261; h = (h ^ k) * 16777619`);
   probe `(h + p) & mask` for p = 0..7. `d3d12_xs` adds entries after every descriptor write
   through the API (device slots 17-19 and 22-24), reading the bytes back from the heap.
   vkd3d-proton passes the bound heaps' table addresses in the push block after `code_base`:
   resource table at dwords 36-37, sampler table at 38-39, 160 bytes in all. Not handled:
   descriptor bytes a title writes into heap memory itself, which needs the exe to settle.
   Because keys are RADV's own bytes, a translated shader that decodes descriptor fields (V#
   base address, T# format) still sees GFX11 layouts, not the console's GFX10.3 (open question
   below).

## Open questions, and which need the exe

| Question | Why it matters | Needs the exe? |
|---|---|---|
| Real root-signature to SGPR map (register vs memory ABI, spills) | patch 3 | yes: trace a draw |
| Does the game write descriptor bytes itself, in console GFX10.3 format? | if so, the bytes in the heap aren't RADV's, and the hash keys and V# decoding see console layouts | yes |
| GFX10.3 vs GFX11 descriptor differences (formats, MIP fields, V# swizzle bit) | the 890M writes GFX11 descriptors, while translated shaders decode V#s with console field positions | partly; RADV source has both layouts |
| Heaps below 4 GiB (on RADV, heaps sit at `0xffff8000_xxxxxxxx`: low dword varies, high dword is RADV's 32-bit range) | Leonida reports shaders that zero a pointer's high dword. Our GPU window is at 0x200000000000 | yes; may need a second window below 4 GiB |
| Do PSOs carry the same DXBC as `the shader archive`? | the lookup key | yes |
| Wave mode per shader | 21,545 shaders are wave64. RADV runs wave64 for compute and pixel shaders by default, but vertex shaders vary | no; FSSM header |
| BVH builder shaders (Xbox DXIL with `@xbox.*` intrinsics) | a separate lowering, not FSSM | no |

## Order of work

1. ~~Build vkd3d-proton from source in the flake (`.#vkd3d-proton-rebox`), unpatched, and switch
   `rebox-run` to it.~~ Done: v3.0.1 cross-built with mingw (static winpthreads); the smoke test
   passes unchanged on it.
2. ~~Patch 1 with a test~~ Done: a compute shader writes through a UAV in a descriptor table set from
   heap slot 10's address. On lavapipe this covers the virtual-handle fallback; the real-address
   path runs on RADV (`tests/manual-test.sh`).
3. ~~Patch 2 and 3 with a hand-written SPIR-V "console" shader~~ Done for compute: the shader
   (GLSL in `tools/gen_fallback_shaders.sh`) writes s0-s31 to the root UAV in s0:s1. The smoke test
   checks the UAV address, the root constants, and the table's real heap address (0 on lavapipe,
   the real address on RADV). Graphics too: an SCSV VS draws a fullscreen triangle and the PS
   writes its SGPRs, set through the graphics root slots. Verified on the Radeon 890M (RADV,
   2026-11-21): all three SGPR checks pass for dispatch and draw, with table addresses equal to
   heap slot 10's start plus the table offset.
4. ~~Patch 4~~ Done: a console shader reads the raw descriptor at its table address and finds
   its heap index through the lookup table, for a descriptor written straight into the heap and one
   copied in (RADV only; lavapipe has no descriptor buffers and skips). Verified on the Radeon 890M
   (2026-11-21): indices 2 and 3 found, in 0 and 1 probes. Next: a translated compute shader from
   Leonida's output (needs the user's `the shader archive`).
5. With the exe: measure the open questions above, then graphics.

Meanwhile `d3d12_xs` keeps its fallback shaders, so a title runs with effects missing rather than
failing. The `.dxil` cache path stays for PC-format shaders the console title may carry, such as
the BVH builder's once lowered.

## First run of Leonida's translator on the game's shaders (2026-11-21)

`the shader archive` (412 MB) carves into 26,503 DXBC containers, all SM 6.6 with `ISG1`/`OSG1` and no
`RTS0`. They unpack to FSSM and disassemble with LLVM 21 (`gfx1013`): 25,030 clean, 1,473 with
some invalid words. Leonida's `rdna2spirv.py` translates them, run as a separate program from a
local clone (GPLv3; its output is data, nothing of it is in this repo). All 26,503 translate and
pass `spirv-val` (25 minutes across all cores). 25,865 of them (97.6%; every mesh, compute but 5,
and geometry shader) truncate descriptor-table pointers to 32 bits. Wave64 that needs real
64-wide subgroups: 867 compute and all 9,282 mesh shaders (pixel, vertex and geometry wave64 is
emulated as 2x32). No shader uses a binding above 19. Its modules use set 0
only: binding 1 samplers, 2 the descriptor hash table (a storage buffer), every other binding
image or storage-image arrays. The push block is s0-s31 plus `heap_base`/`code_base`, as ours.

Integration so far:

- the lookup table now uses Leonida's layout and is one table for all heaps (T# and S# lookups
  share it);
- `patches/vkd3d-proton/0004` remaps an `SCSV` module's set 0 binding 0 (resource heap) and
  binding 1 (sampler heap) to the device's real bindless sets, so a glue step only has to
  normalise bindings and turn the hash-table storage buffer into a pointer from push dwords 36-37.

**Blocker: descriptor-table pointers are 32-bit.** The game's shaders read a table pointer's low
dword and zero the high one (the translator reports `truncates_table_ptr_high`), then load from
`heap_base + pointer`. On the console, descriptor heaps sit below 4 GiB. RADV puts descriptor
buffers in its 32-bit window at `0xffff8000_xxxxxxxx`, and `heap_base` can't absorb that, because
Leonida adds it to every address, absolute buffer addresses included. Proposed fix: extend the
RADV patch so 32-bit allocations come from the kernel's low 32-bit VA range (no
`AMDGPU_VA_RANGE_HIGH`) with `address32_hi = 0`. Heaps then sit below 4 GiB and `heap_base`
stays 0.

### Translating the game's shaders (`tools/translate-shaders.sh`)

`tools/translate-shaders.sh [path/to/the shader archive]` runs the whole chain and writes
`build/console-shaders` (local, git-ignored): carve, unpack FSSM, disassemble with LLVM 21,
translate with Leonida's `rdna2spirv.py` (cloned into `build/shader-work/Leonida` at run time,
latest `origin/main` unless `LEONIDA_REV` is set), then `scsv_pack.py`. Rerun it whenever
Leonida's translator gets fixes; it records the revision it used in
`build/console-shaders/LEONIDA_REV`. These shaders come from Leonida, not AnyPS5: AnyPS5's output
(americas-recomp) resolves descriptors on the CPU per draw, a different runtime contract.

### Packing translated shaders (`tools/scsv_pack.py`)

`scsv_pack.py --dxbc CARVE_DIR --spirv TRANSLATED_DIR --out CACHE [--stage cs] [--validate]`
retargets the translator's SPIR-V to this contract and writes `<fnv1a64 of the DXBC>.scsv`,
which `d3d12_xs` looks up before `.dxil`:

- set 0 binding 1 (samplers) stays (sampler heap); every other set 0 binding except 2 becomes
  binding 0 (CBV/SRV/UAV heap, aliased); vkd3d-proton 0004 maps both to its real sets;
- binding 2, the hash table, becomes a pointer built from two new push-constant members at bytes
  144/148 (the resource heap's lookup table), so its loads are PhysicalStorageBuffer loads;
- wave size 64 only where the translator says wave64 is not emulated.

All 2,689 compute shaders pack and pass `spirv-val`. `tests/run-pso-check.sh CACHE` (through
`smoke.exe --pso-check`) creates a compute PSO from each with an empty root signature: on
lavapipe 147 of the first 200 succeed, and the other 53 need 64-wide subgroups (lavapipe has 8).
`tests/manual-test.sh` runs it on the 890M when `build/console-shaders/` exists (local only).
