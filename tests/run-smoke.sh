#!/usr/bin/env bash
# Runs build/smoke.exe under Wine against the built shims and vkd3d-proton.
# Needs: WINE, VKD3D_PROTON_DIR, DXVK_DIR, REBOX_SHIMS_DIR, vkd3d-compiler on PATH.
# Without a GPU, point VK_ICD_FILENAMES at lavapipe (the flake's devShell does).
set -euo pipefail

here=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

: "${WINE:?}" "${VKD3D_PROTON_DIR:?}" "${DXVK_DIR:?}" "${REBOX_SHIMS_DIR:?}"

# "Console" shader: bytes no D3D12 implementation will accept.
printf 'REBOX-FAKE-CONSOLE-SHADER-%s' "$(date +%s)" > "$work/fake.bin"

# Its "AnyPS5 translation": a real cs_5_0 DXBC blob.
printf '[numthreads(1, 1, 1)] void main() {}\n' > "$work/cs.hlsl"
vkd3d-compiler -x hlsl -b dxbc-tpf -p cs_5_0 -o "$work/translated.dxbc" "$work/cs.hlsl"

# Console shaders arrive as DXBC containers too: a DXIL part holding a NOBC stub around zlib FSSM
# (GFX10.3 code). They must be looked up in the cache, not passed through as PC bytecode.
python3 - "$work/console.dxbc" <<'PY'
import struct, sys, zlib
fssm = b"FSSM" + bytes(0x80) + struct.pack("<2I", 0xBFA00003, 0xBF810000)
part = b"XBCH" + bytes(0x60) + b"NOBC" + bytes(0x14) + zlib.compress(fssm)
part += bytes(-len(part) % 4)
body = b"DXIL" + struct.pack("<I", len(part)) + part
header = b"DXBC" + bytes(16) + struct.pack("<4I", 1, 0x24 + len(body), 1, 0x24)
open(sys.argv[1], "wb").write(header + body)
PY

mkdir -p "$work/cache"
hash() { python3 "$here/tools/anyps5_cache.py" hash "$1" | cut -d' ' -f1; }
for f in fake.bin console.dxbc; do
    cp "$work/translated.dxbc" "$work/cache/$(hash "$work/$f").dxil"
done
# PC bytecode must never be substituted: give it an entry that would break pipeline creation.
printf 'not a shader' > "$work/cache/$(hash "$work/translated.dxbc").dxil"

export WINEPREFIX="$work/prefix" WINEARCH=win64 WINEDEBUG=${WINEDEBUG:--all}
"$WINE" wineboot -i >/dev/null 2>&1
sys32="$WINEPREFIX/drive_c/windows/system32"
install -m 644 "$VKD3D_PROTON_DIR"/d3d12.dll "$VKD3D_PROTON_DIR"/d3d12core.dll "$DXVK_DIR"/dxgi.dll "$sys32"/
install -m 644 "$REBOX_SHIMS_DIR"/xmem.dll "$REBOX_SHIMS_DIR"/d3d12_xs.dll "$REBOX_SHIMS_DIR"/xaudio2_9.dll \
    "$REBOX_SHIMS_DIR"/amd_fidelityfx_loader_xbox.dll "$sys32"/
export WINEDLLOVERRIDES="d3d12,d3d12core,dxgi,xaudio2_9=n"
export REBOX_SHADER_CACHE="Z:${work//\//\\}\\cache"
# PresentX frame dumps, checked by the smoke test.
mkdir -p "$work/frames"
export REBOX_DUMP_FRAMES="Z:${work//\//\\}\\frames" REBOX_DUMP_FRAMES_AT=1,2,3,4

# SMOKE_ONLY=<file> runs just that case (for debugging).
for f in fake.bin console.dxbc translated.dxbc; do
    [[ -n ${SMOKE_ONLY:-} && $SMOKE_ONLY != "$f" ]] && continue
    echo "== $f"
    "$WINE" "$here/build/smoke.exe" "Z:${work//\//\\}\\$f"
done
[[ -n ${SMOKE_ONLY:-} ]] && exit 0

# Negative control: same shader with the cache entry gone must fail when
# fallback shaders are off...
echo "== fake.bin, no cache entry"
rm "$work/cache/$(hash "$work/fake.bin").dxil"
REBOX_SHADER_FALLBACK=0 "$WINE" "$here/build/smoke.exe" "Z:${work//\//\\}\\fake.bin" nocache

# ...and succeed through the fallback shaders by default.
echo "== fake.bin, no cache entry, fallback shaders"
out=$("$WINE" "$here/build/smoke.exe" "Z:${work//\//\\}\\fake.bin" 2>&1) || { echo "$out"; exit 1; }
echo "$out"
grep -q "uses fallback shaders" <<<"$out" || { echo "FAIL fallback shaders were not used"; exit 1; }
