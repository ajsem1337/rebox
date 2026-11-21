# Console shaders: design

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

1. **Real heap addresses.** `GetGPUDescriptorHandleForHeapStart` returns a fake handle
   (`unique << 33`). Console shaders dereference descriptor table pointers. In console mode, return
   the descriptor buffer's real GPU VA, and accept such VAs wherever handles are taken. With our
   layer that address is also the CPU pointer, so `heap_base = 0` and the shader's loads go straight
   to the heap.
2. **Console pipelines.** At PSO creation, a blob whose DXIL part is a NOBC stub around FSSM is a
   console shader:
   - look up `<stage>/<fssm hash>.spv` in the shader directory;
   - build the pipeline with the root signature's layout plus a console push-constant block
     (s0-s31, `heap_base`, `code_base`) and the bindless sets the contract names;
   - set the required subgroup size from the shader's wave mode;
   - with no translation, fall back as `d3d12_xs` does today.
3. **SGPR packing at draw and dispatch.** From vkd3d's root-parameter state, write s0-s31 in root
   order: one dword per 32-bit constant, two per root descriptor or descriptor table (its real
   VA). This is Leonida's default mapping. It is an assumption until the exe shows the real one;
   keep it table-driven so a measured map replaces it.
4. **Image and sampler lookup.** Whenever a descriptor is written into a shader-visible heap
   (`Create*View`, `CopyDescriptors`), insert its raw bytes into the contract's hash table with
   the bindless index of the same entry. In vkd3d's heap that index is `offset / 32`, so the
   bindless arrays are vkd3d's own mutable heap array and no second copy is needed.

## Open questions, and which need the exe

| Question | Why it matters | Needs the exe? |
|---|---|---|
| Real root-signature to SGPR map (register vs memory ABI, spills) | patch 3 | yes: trace a draw |
| Does the game write descriptor bytes itself, in console GFX10.3 format? | if so, the bytes in the heap aren't RADV's, and the hash keys and V# decoding see console layouts | yes |
| GFX10.3 vs GFX11 descriptor differences (formats, MIP fields, V# swizzle bit) | the 890M writes GFX11 descriptors, while translated shaders decode V#s with console field positions | partly; RADV source has both layouts |
| Heaps below 4 GiB | Leonida reports shaders that zero a pointer's high dword. Our GPU window is at 0x200000000000 | yes; may need a second window below 4 GiB |
| Do PSOs carry the same DXBC as `the shader archive`? | the lookup key | yes |
| Wave mode per shader | 21,545 shaders are wave64. RADV runs wave64 for compute and pixel shaders by default, but vertex shaders vary | no; FSSM header |
| BVH builder shaders (Xbox DXIL with `@xbox.*` intrinsics) | a separate lowering, not FSSM | no |

## Order of work

1. ~~Build vkd3d-proton from source in the flake (`.#vkd3d-proton-rebox`), unpatched, and switch
   `rebox-run` to it.~~ Done: v3.0.1 cross-built with mingw (static winpthreads); the smoke test
   passes unchanged on it.
2. Patch 1 with a test: a shader reads a descriptor through the table VA it was given.
3. Patch 2 and 3 with a hand-written SPIR-V "console" shader that writes its s0-s31 to a UAV, to
   check the packing against root parameters.
4. Patch 4 and a translated compute shader from Leonida's output (needs the user's `the shader archive`;
   run on the 890M with `tests/manual-test.sh`).
5. With the exe: measure the open questions above, then graphics.

Meanwhile `d3d12_xs` keeps its fallback shaders, so a title runs with effects missing rather than
failing. The `.dxil` cache path stays for PC-format shaders the console title may carry, such as
the BVH builder's once lowered.
