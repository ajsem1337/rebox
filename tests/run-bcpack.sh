#!/usr/bin/env bash
# BCPack through the DirectStorage shim and helpers/bcpack (Leonida's decoder, a separate
# process): a synthetic BC4 chunk decoded by a request on a memory queue, compared with the
# blocks it encodes. Needs WINE and LEONIDA_SRC (the dev shell sets both) and python3.
set -euo pipefail

here=$(cd "$(dirname "$0")/.." && pwd)
: "${WINE:?}" "${LEONIDA_SRC:?}"
make -C "$here" -s build/xcrdapi.dll >/dev/null
work=$(mktemp -d)
helper=
trap '[[ -n $helper ]] && kill $helper 2>/dev/null; rm -rf "$work"' EXIT

x86_64-w64-mingw32-gcc -O1 -o "$work/bcpack_probe.exe" "$here/tests/bcpack_probe.c" -luuid
cp "$here/build/xcrdapi.dll" "$work/XCRDAPI.dll"
python3 -I "$here/helpers/bcpack/make-vector.py" "$LEONIDA_SRC" "$work"

python3 -I "$here/helpers/bcpack/bcpack-helper.py" --leonida "$LEONIDA_SRC" --port-file "$work/port" &
helper=$!
for _ in $(seq 100); do [[ -s $work/port ]] && break; sleep 0.1; done
[[ -s $work/port ]] || { echo "FAIL the helper didn't start"; exit 1; }

export WINEPREFIX="$work/prefix" WINEDEBUG=${WINEDEBUG:--all} REBOX_BCPACK_PORT=$(cat "$work/port")
"$WINE" wineboot -i >/dev/null 2>&1
cd "$work" && "$WINE" bcpack_probe.exe 4 'Z:'"${work//\//\\}"'\bc4.bcpack' 'Z:'"${work//\//\\}"'\bc4.expected'
