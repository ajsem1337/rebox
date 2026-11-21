# rebox

Runs a dumped Xbox Series (Scarlett) GDK title on Linux using:

- **WineGDK** for Wine itself and the GDK runtime (`xgameruntime`: XUser, XTaskQueue, XGameSave and so on)
- **vkd3d-proton** for D3D12 on Vulkan, built from source with our patches (`.#vkd3d-proton-rebox`,
  `patches/vkd3d-proton/`; none yet), plus **DXVK's `dxgi.dll`** for adapter enumeration
- **shims for the console-only DLLs** WineGDK doesn't cover: `xmem.dll` and `d3d12_xs.dll` (D3D12.X)
- **an offline RDNA2 → SPIR-V translator** for the console shaders; running them needs the patched
  vkd3d-proton described in [docs/shaders.md](docs/shaders.md) (not done yet)

It's built for a layout like `../xsx`: loose files plus `MicrosoftGame.Config`. It needs the main
`.exe`, which isn't in that dump yet.

## Layout

```
bin/rebox-run          launcher: prefix setup, DLL install/overrides, runs the exe from MicrosoftGame.Config
shims/xmem/               xmem.dll: XMemAlloc/Free(+Default), allocation hooks, XMemVirtualQuery;
                          GPU-visible memory types come from d3d12_xs
shims/d3d12_xs/           d3d12_xs.dll: Xbox device/queue/command-list/descriptor-heap proxies (proxy.c),
                          GPU memory (gpu_memory.c), shader substitution, forwards to vkd3d-proton
shims/amd_fidelityfx_loader_xbox/  FidelityFX API stub: FSR-style queries, bilinear upscale via d3d12_xs
shims/xaudio2_9/          xaudio2_9.dll: Wine's XAudio2 (via xaudio2_8) with XMA2 voices played as silence
layers/                   VK_LAYER_REBOX_gpuva_identity: host Vulkan layer mapping host-visible
                          memory at the same GPU and CPU address
patches/                  Mesa patch letting RADV place memory at lower-half device addresses
shims/common/             logging + REBOX_STUB macro for generated stubs
generated/                stubs from tools/scan_imports.py --gen-stubs (one-off fill-in exports;
                          dlls/<name>/ for whole DLLs nobody provides) and generated shaders
tools/scan_imports.py     which imports can't be resolved under Wine + shims; generates stubs
tools/anyps5_cache.py     links AnyPS5 output into the runtime cache, keyed by hash
tools/d3d12x_probe.py     recovers D3D12.X IIDs, vtable slots and inlined field accesses from console binaries
tests/                    smoke test (xmem, forwarders, PSO substitution, negative control);
                          gpuva_probe.c checks whether the Vulkan driver can map GPU VA == CPU VA
docs/d3d12x.md            what the console DLLs reveal about D3D12.X and unified memory, and the plan
```

## Usage

```sh
nix run . -- --scan ../xsx    # report imports Wine + vkd3d-proton + shims can't resolve
nix run . -- ../xsx           # run title.exe
nix develop -c bash -c 'make all build/smoke.exe && tests/run-smoke.sh'
```

Once the exe is dumped:

1. **Find what's missing.** `nix run . -- --scan ../xsx`. Expect the exe to import many more
   `xmem`/`d3d12_xs` symbols and some GDK APIs that WineGDK lacks.
2. **Stub the shim gaps.** In `nix develop`: `python3 tools/scan_imports.py ../xsx --provider build
   --provider "$VKD3D_PROTON_DIR" --provider "$DXVK_DIR" --provider "$WINE_DLL_DIR" --shims shims
   --gen-stubs generated`, then `make`. Stubs log once and return `E_NOTIMPL`. To make an entry
   point real, implement it in `shims/<dll>/`, add it to that DLL's `.def`, and regenerate. A DLL
   that nobody provides at all (e.g. `xg_xs.dll`) gets a whole stub DLL in
   `generated/dlls/<name>/`, built to `build/stub/` and installed by `rebox-run`, so the exe
   still loads. Forwarded exports in package DLLs are followed: if one forwards to a DLL nobody
   provides, that DLL gets a stub too, and the report names the forwarder (`via dll.symbol`).
   Import and export tables are read directly, so names pefile would drop (characters
   outside `._?@$()<>`) still count. `tests/run-stubgen.sh` checks all of this end to end.
3. **Shaders: not usable yet.** The plan is AnyPS5, compiling at first use inside the runner
   ([docs/anyps5.md](docs/anyps5.md)). Console shaders translate to SPIR-V that expects console-style
   resources, which vkd3d-proton can't take. [docs/shaders.md](docs/shaders.md) has the design
   (patched vkd3d-proton, Leonida's runtime contract). Until then every console shader gets a
   fallback, so the title runs with effects missing. `anyps5-cache link` still links PC-format
   (DXBC/DXIL) translations into the cache.
4. **Run, then iterate.** Read `misses.txt` in the cache dir (`anyps5-cache misses <cache>`). Set
   `REBOX_STUB_FATAL=1` to stop at the first unimplemented stub the title reaches.

### Environment

| Variable | Meaning |
|---|---|
| `WINE`, `VKD3D_PROTON_DIR`, `DXVK_DIR`, `REBOX_SHIMS_DIR` | Set by the flake wrapper; override to use local builds |
| `WINEPREFIX` | Default `~/.local/share/rebox/<TitleId>` |
| `REBOX_SHADER_CACHE` | Unix path to the cache; default `$WINEPREFIX/shader-cache` |
| `REBOX_DUMP_SHADERS=1` | Write each shader that misses the cache to `<cache>/dump/<hash>.<stage>.bin` |
| `REBOX_CPU_VISIBLE_BUFFERS=0` | Leave DEFAULT-heap buffers GPU-only (they're then not identity-mapped) |
| `REBOX_FFX_STUB=0` | Run the package's own FidelityFX loader instead of the stub |
| `REBOX_SHADER_FALLBACK=0` | Fail PSO creation on a missing translation, instead of using fallback shaders |
| `REBOX_STUB_FATAL=1` | Exit on the first unimplemented stub call |
| `REBOX_REAL_HEAP_VA=0` | Descriptor heaps report vkd3d-proton's virtual GPU handles instead of their real GPU addresses (which console shaders dereference; needs the patched vkd3d-proton and descriptor buffers, as on RADV) |
| `REBOX_CRASH_LOG=N` | Report the first N faults (access violations, illegal instructions, divide by zero, stack overflow) with module+RVA, target address, a GPU-window hint, registers and an unwound stack; default 8, 0 turns it off. Reports are first-chance, so a fault the title handles itself also appears |
| `REBOX_SLOT_STATS=N` | Count calls per D3D12.X proxy slot and log them every N presents and at exit (`c` = our code, `T` = trap, unmarked = forwarded to the PC slot) |
| `REBOX_LOG=/path` | Also append shim logs to this file |
| `REBOX_GPUVA_WINDOW` | `BASE:SIZE` (hex) for identity-mapped GPU memory; default `200000000000:10000000000`; `off` stops xmem reserving it |
| `REBOX_GPU_MEMORY=0` | Give GPU-visible `XMemAlloc` types plain `VirtualAlloc` memory instead of D3D12 heaps |
| `REBOX_STOCK_VULKAN=1` | Don't switch AMD GPUs to `.#radv-rebox` |
| `REBOX_PRESENT=0` | No window or swapchain for `PresentX` (frame dumps still work) |
| `REBOX_VSYNC=N` | `PresentX` sync interval, default 1 |
| `REBOX_FRAME_PACING=0` | Make `SetFrameIntervalX`/`ScheduleFrameEventX`/`WaitFrameEventX` no-ops, so the title runs as fast as `PresentX` allows |
| `REBOX_DUMP_FRAMES=DIR` | Write presented frames to `DIR/frame_<n>.bmp` (8-bit RGBA/BGRA planes) or `.bin` |
| `REBOX_DUMP_FRAMES_AT` | Which frames to dump, comma-separated; default `1,60,600` |

The cache key is the 64-bit FNV-1a hash of the bytecode exactly as the title passes it to
`Create*PipelineState`. **Not yet verified:** whether that matches the raw shader bytes in
`the shader archive`. If everything misses, dump the missed shaders and diff them against the originals.

Console shaders are DXBC containers too, so the shim doesn't treat every `DXBC` blob as PC
bytecode. Only a container with a `SHEX`/`SHDR` part or a `DXIL` part holding LLVM bitcode passes
through. Everything else, including the console's `DXIL` parts (a `NOBC` stub around zlib `FSSM`
GFX10.3 code), is looked up in the cache.

A miss would fail PSO creation, which a title rarely survives. So by default the whole pipeline
gets fallback shaders instead (`generated/fallback_shaders.h`, from `tools/gen_fallback_shaders.sh`):
graphics draws nothing, compute does nothing, and the effect is just missing. Each one is logged.
Mesh pipelines get a mesh shader that emits nothing and an amplification shader that launches
nothing (DXIL from `dxc`). `REBOX_SHADER_FALLBACK=0` restores failing, for hunting
down missing translations.

## Verified so far

- Shims build with mingw. `tests/run-smoke.sh` passes under stock Wine 11.21 + vkd3d-proton 3.0.1
  on lavapipe. Bytes vkd3d-proton can't compile succeed in both `CreateComputePipelineState` and
  `CreatePipelineState` (stream) when a cache entry exists, and fail with `E_FAIL` when it doesn't.
  The same holds for a synthetic console container (`DXIL` part with a `NOBC`/`FSSM` stub). Real
  PC DXBC compiles directly even when a broken cache entry exists for its hash.
- Against the DLLs in `../xsx`, every import resolves under Wine + vkd3d-proton + shims. The only
  gap was `XMemGetWorkingSetStatistics`. It's now implemented for the one selector the AMD
  upscaler passes (`0x20000001`): it returns TRUE and writes private commit to the u64 at `+8`.
  That call shape comes from Xbox2Windows's call-site notes and hasn't been checked against a
  disassembly here.
- `XMemVirtualQuery` takes `(process, address, info)` with a 0x40-byte info struct, as
  XboxBvhBuilder.dll calls it, and the smoke test covers that call.
- The FidelityFX stub answers a render-resolution query as FSR does. It scales a cleared 32x24
  render into a 64x48 output through `ffxDispatch` on an Xbox command list, and the presented
  frame has the render's colour.
- Fallback shaders: a graphics PSO built from untranslated console bytes is created, a draw with
  it leaves the frame untouched, and compute and stream PSOs work too. Strict mode still rejects
  them.
- `PresentX` presents a cleared render target recorded through Xbox slots. Both plane-param
  layouts work (resource, or pointer to it), and the dumped frame has the cleared pixels. This
  was checked headless; the window and swapchain need a display.
- The smoke test drives the Xbox proxies by slot number, as title code does. It creates the
  device, queue, list and heap, records through the shifted command-list slots, executes and
  waits on a fence, and checks the zeroed inline fields, the RAX-returning heap starts and the
  trap on an unknown slot. It passes on lavapipe, also with the identity layer loaded (the layer
  passes through there).
- On a Radeon 890M with `.#radv-rebox` and the identity layer (`tests/manual-test.sh`), the
  Wine smoke test passes and every GPU heap `d3d12_xs` hands out is identity-mapped, including
  heaps allocated right after a free.

## Known gaps (ordered roughly by how soon they'll bite)

- **Console shaders don't render** ([docs/shaders.md](docs/shaders.md)). The translators emit
  SPIR-V that reads user SGPRs and raw hardware descriptors. Running it needs a patched
  vkd3d-proton (real heap addresses, console pipelines, SGPR packing, a descriptor lookup table),
  which isn't built yet. Every console PSO uses fallback shaders.
- **D3D12.X interfaces** ([docs/d3d12x.md](docs/d3d12x.md)). `D3D12XboxCreateDevice` returns
  proxies with the Xbox layouts. Their slot tables are merged from our disassembly and
  [ScarlettX](https://github.com/Freminet/ScarlettX)'s traces. Slots nobody has identified
  log and return `E_NOTIMPL`; `REBOX_STUB_FATAL=1` stops at the first one. `PresentX` shows
  plane 0 in a window through a DXGI swapchain. Not done yet:
  - only plane 0 is shown (no HUD-plane composition yet). Planes in formats a swapchain can't
    take, such as R11G11B10, are converted to RGBA8 with a blit: HDR is clamped, not tone-mapped;
  - frame events come from a CPU clock, not the display: `WaitFrameEventX` follows the period the
    title sets, but isn't locked to the monitor's vblank, so with vsync on a mismatched refresh
    rate can judder;
- **Unified memory** ([docs/d3d12x.md](docs/d3d12x.md)). Titles use GPU VAs as CPU pointers and
  the reverse. GPU-visible `XMemAlloc` memory and command-list embedded data come from
  CPU-visible D3D12 heaps. `rebox-run` loads `VK_LAYER_REBOX_gpuva_identity` and, on AMD,
  `.#radv-rebox`, which together make those heaps identity-mapped. Without them,
  `d3d12_xs` logs that GPU memory is not identity-mapped. Buffers the title creates in DEFAULT
  heaps, and DEFAULT heaps that hold only buffers, are made CPU-visible (custom `WRITE_BACK`/`L0`),
  so they're identity-mapped too (`REBOX_CPU_VISIBLE_BUFFERS=0` opts out). Textures and mixed
  heaps keep ordinary GPU addresses.
- **Audio: XMA2 decoding is untested on real data.** Under WineGDK, FAudio sends XMA2 to Wine's
  WMA decoder, which accepts only WMA formats, so creating an XMA2 source voice fails.
  `shims/xaudio2_9` wraps Wine's `xaudio2_8` (same FAudio code) and turns XMA2 voices into PCM
  voices. Each XMA2 buffer is decoded at submission by FFmpeg's `xma2` decoder: the flake's
  `ffmpeg-xma2`, a static mingw libavcodec with only that decoder. If decoding fails, or the DLL is
  built without `FFMPEG_XMA2`, the voice plays silence of about the buffer's length. A callback
  between the voice and the title keeps callback contexts intact and frees the PCM.
  FFmpeg has no XMA2 encoder, so no test feeds it real XMA2. Bink's audio is decoded to PCM by Bink and is
  unaffected. Wine also reports a mastering voice as created when there's no audio device, after
  which FAudio crashes on the first source voice; that only matters on machines without audio.
- **Upscaling is bilinear.** The console FidelityFX upscaler runs precompiled-ISA shaders, which
  would become no-op fallbacks and leave its output unwritten. `rebox-run` runs the title from
  `$WINEPREFIX/package`, a directory of symlinks to the package, with our
  `amd_fidelityfx_loader_xbox.dll` in place of the package's. That stub answers FSR's queries
  (quality-mode ratios, but no jitter) and scales the colour input into the output with a
  compute blit. There's no temporal anti-aliasing; frame generation reports no provider.
- **Textures written through their GPU VA** get scratch memory (see docs/d3d12x.md): no crash,
  but the texture doesn't see the data. Console tiling (`4KB_S`/`64KB_S`) isn't handled yet.
- **Console root signatures and pipeline subobjects** pass through unchanged. Console root
  signatures set flags `0x03000000`. Windows' runtime rejects those bits, but vkd3d-proton
  ignores them, and the smoke test checks that it still does. Their meaning is unknown. Pipeline
  streams are parsed up to the public type 28 (DEPTH_STENCIL2, RASTERIZER1/2 included). A stream
  with a type beyond that, such as an Xbox-specific one, skips substitution and is logged once,
  because its size is unknown.
- **XALLOC_ATTRIBUTES** decoding follows [SlimEra](https://github.com/XWine1/SlimEra)'s Xbox One
  ERA definition (alignment 2^n, at least 16 bytes), which matches the 32-byte alignment Bink relies
  on. Allocations with 64 KiB or 4 MiB pages start on that boundary. Scarlett's header hasn't been
  seen, so the meaning of graphics memory types beyond "GPU-visible" is unconfirmed. `XMemAlloc` memory is always zeroed, including blocks
  reused from freed ones.
- **WineGDK was archived 2026-11-13.** Its successors are
  [xodus-gaming/wine](https://github.com/xodus-gaming/wine) and
  [xodus-gaming/xgameruntime](https://github.com/xodus-gaming/xgameruntime), but that runtime only
  covers threading so far. Switch the `winegdk-src` input once it catches up.

## Licence

rebox is licensed under the GNU General Public License, version 2 only
([LICENSE](LICENSE)), so it can load AnyPS5's shader recompiler (GPL-2.0-only) in-process.
The patches in `patches/` modify vkd3d-proton (LGPL-2.1+) and Mesa (MIT) and are offered under
those projects' licences too. Game data, dumps and anything derived from them (`build/`) are never
part of the repository.
