#!/usr/bin/env bash
# GPU checks that need real hardware (an AMD GPU with RADV). Builds the patched
# RADV and the identity layer, runs the native probe and the Wine smoke test
# through them, prints a summary and keeps the full output in
# build/manual-test.log.
#
#   tests/manual-test.sh
set -uo pipefail

cd "$(dirname "$0")/.."
log=build/manual-test.log
mkdir -p build
: > "$log"
nixx() { nix --extra-experimental-features 'nix-command flakes' "$@"; }
section() { printf '\n===== %s\n' "$1" | tee -a "$log"; }

section "build patched RADV (.#radv-rebox)"
nixx build .#radv-rebox --out-link result-radv >> "$log" 2>&1 || { echo "build failed, see $log"; exit 1; }

section "build layer, probe, shims, smoke test"
nixx develop -c make layer build/gpuva_probe all build/smoke.exe >> "$log" 2>&1 || { echo "build failed, see $log"; exit 1; }

export VK_ICD_FILENAMES=$PWD/result-radv/share/vulkan/icd.d/radeon_icd.x86_64.json
export VK_ADD_LAYER_PATH=$PWD/build
export VK_INSTANCE_LAYERS=VK_LAYER_REBOX_gpuva_identity

section "native probe through the layer"
build/gpuva_probe >> "$log" 2>&1
sed -n '/native probe through the layer/,$p' "$log" | grep -E 'device:|ICD:|gpuva\]|^(import|placed|replay-low|replay-high|plain):'

section "Wine smoke test through the layer"
nixx develop -c tests/run-smoke.sh >> "$log" 2>&1
smoke_status=$?
sed -n '/Wine smoke test through the layer/,$p' "$log" \
    | grep -E 'gpuva\]|identity-mapped|heap GPU start|descriptor table set|real GPU addresses|console shader|console SGPR|console VS/PS|shadow|T#|V#|S#|AnyPS5|binding [0-9]|maps back|deferred|bindless sets|compiled a variant|s0:s1|s2-s5|s6:s7|PresentX|presented frame|XMA2|XAudio2|xaudio2_9|FFX|ffx stub|upscale|R11G11B10|mesh|^== |^(FAIL|PASSED|FAILED)' | uniq
echo "run-smoke.sh exit status: $smoke_status"

section "heap slot 10 with REBOX_HEAP_VA=real (RADV descriptor buffers)"
REBOX_HEAP_VA=real SMOKE_ONLY=fake.bin nixx develop -c tests/run-smoke.sh >> "$log" 2>&1
echo "run-smoke.sh (real heap addresses) exit status: $?"
sed -n '/REBOX_HEAP_VA=real/,$p' "$log" | grep -E 'heap GPU start|report real|SGPR table|descriptor table set|^(FAIL|PASSED|FAILED)' | uniq

echo
echo "Full output: $log"
