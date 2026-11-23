# Shaders: the vkd3d-proton side

A title's shaders reach `d3d12_xs` at PSO creation. PC bytecode goes to vkd3d-proton unchanged;
console shaders are compiled by AnyPS5 at dispatch and draw time ([anyps5.md](anyps5.md)). This
page covers the vkd3d-proton patches that path rests on, and the shader cache.

## Why inside vkd3d-proton

`d3d12_xs` could record console work into vkd3d-proton's command buffers from outside
(`ID3D12GraphicsCommandListExt::GetVulkanHandle`), but vkd3d-proton supports that only outside its
own render passes; graphics draws would fight its render-pass, barrier and dynamic-state tracking.
So vkd3d-proton 3.0.1 is built from source in the flake (`.#vkd3d-proton-rebox`, mingw, static
winpthreads) with `patches/vkd3d-proton/` applied in name order. Most of what console shaders need
is already there:

- root CBVs, SRVs and UAVs are buffer device addresses in push constants on RADV;
- shader-visible heaps are descriptor buffers holding raw RADV hardware descriptors with a
  32-byte CBV/SRV/UAV stride, the console's stride;
- heap memory is host-visible, so the identity layer maps it at GPU VA == CPU VA;
- wave size can be set per shader.

## The patches

| Patch | What it does |
|---|---|
| `0001-real-descriptor-heap-va` | Exports `ReboxGetDescriptorHeapVA` from `d3d12core.dll`: the real GPU address and stride of a shader-visible heap's descriptors (descriptor buffers only, as on RADV; lavapipe has none). With `REBOX_HEAP_VA=real`, heap slot 10 returns it, and the command-list slots that take a GPU handle (root descriptor tables, Xbox 35/36; UAV clears, 53/54) map it back to vkd3d-proton's handle |
| `0002-console-pipelines` | Accepts an `SCSV` blob in place of DXBC: a 44-byte header (magic `SCSV`, version 1, Vulkan stage bit, wave size 0/32/64, workgroup size, SPIR-V offset and size, DXBC offset and size), the SPIR-V (entry point `main`, used as is, with the wave size requested) and, for graphics stages, a console DXBC container whose I/O signatures give the vertex attribute locations. Every root signature's push-constant range is widened to at least 144 bytes, so console pipelines share its layout. At each draw and dispatch with such a pipeline, the user SGPRs s0-s31 are packed from the root parameters in root order: one dword per 32-bit constant, two per root descriptor or descriptor table |
| `0003-descriptor-lookup-tables` | Per-heap descriptor lookup tables, passed at push dwords 36-39 (resource table, then sampler table). `d3d12_xs` leaves them empty: AnyPS5's bindings resolve on the CPU through the shadow heaps |
| `0004-console-heap-bindings` | Remaps an `SCSV` module's set 0 binding 0 (resource heap) and binding 1 (sampler heap) to the device's bindless sets |
| `0005-deferred-console-pipelines` | Deferred console compute pipelines (`SCRD` blobs), compiled through `d3d12_xs` at each dispatch ([anyps5.md](anyps5.md)); raises the sampler-heap limit by 256 for the heaps' hidden tail |
| `0006-deferred-console-graphics` | Deferred console vertex and pixel stages, compiled at each draw ([anyps5.md](anyps5.md)) |
| `0007-console-compute-user-sgprs` | The console's compute user-SGPR layout for deferred compute shaders: root parameters from s4, one SGPR per descriptor table (a 32-bit pointer), two per root descriptor, one per constant; s0-s3 left 0. `REBOX_CONSOLE_COMPUTE_LAYOUT=pairs` packs as `0002` does, from s0 with two per table, which the smoke test's hand-written programs use |
| `0008-console-breadcrumbs` | With `VKD3D_CONFIG=breadcrumbs`, records each console variant's key before its dispatch or draw, so a lost device's breadcrumb dump names the console shader (`d3d12_xs` logs the key with each compile). The flake builds vkd3d-proton with `enable_trace` for this |

The smoke test covers `0001`-`0004` with hand-written SPIR-V console shaders (GLSL in
`tools/gen_fallback_shaders.sh`): a compute shader writes s0-s31 to the root UAV in s0:s1, an
`SCSV` vertex shader draws a full-screen triangle whose pixel shader writes its SGPRs, and the
table addresses equal heap slot 10's start plus the table's offset on RADV (0 on lavapipe).

## The shader cache

`d3d12_xs` looks each shader that isn't PC bytecode up in `REBOX_SHADER_CACHE` (default
`$WINEPREFIX/shader-cache`) by the 64-bit FNV-1a hash of its bytes exactly as the title passes
them: `<hash>.scsv` first, then `<hash>.dxil`. PC bytecode is a container with a `SHEX`/`SHDR`
part, or a `DXIL` part holding LLVM bitcode; `SCSV` and `SCRD` blobs pass through too. Console
compute and graphics containers become deferred shaders before the cache is asked
([anyps5.md](anyps5.md)). A miss gets fallback shaders (README), and `REBOX_DUMP_SHADERS=1`
writes the missed bytes to `<cache>/dump/<hash>.<stage>.bin`.

## Open questions

| Question | Why it matters |
|---|---|
| Does a title write descriptor bytes into heap memory itself, in console format? | Those land in the shadow heap as on the console; turning them into vkd3d-proton views needs decoding |
| GFX10.3 against GFX11 descriptor fields (formats, mip fields, V# swizzle bit) | RADV writes GFX11 descriptors for the 890M; the shadow heaps hold GFX10 ones for AnyPS5 |
| BVH builder shaders (Xbox DXIL calling `@xbox.*` intrinsics) | They aren't FSSM and need a lowering of their own; their PSOs get fallbacks |
