#!/usr/bin/env bash
# End-to-end check of whole-DLL stub generation: an exe importing a DLL
# nobody provides (by name and by ordinal) is scanned, the generated stub
# DLL is built, and the exe must then load and get E_NOTIMPL from both.
# Needs: WINE, WINE_DLL_DIR, x86_64-w64-mingw32-gcc/dlltool (nix develop).
set -euo pipefail

here=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
: "${WINE:?}" "${WINE_DLL_DIR:?}"
mkdir -p "$work/game"

cat > "$work/fake.def" <<'DEF'
LIBRARY fakexbox.dll
EXPORTS
    XFakeCall
    XFakeOrdinal @7 NONAME
DEF
x86_64-w64-mingw32-dlltool -d "$work/fake.def" -l "$work/libfakexbox.a"
cat > "$work/probe.c" <<'C'
#include <stdio.h>
#include <windows.h>
__declspec(dllimport) LONG_PTR XFakeCall(void);
__declspec(dllimport) LONG_PTR XFakeOrdinal(void);
int main(void)
{
    LONG_PTR a = XFakeCall(), b = XFakeOrdinal();
    printf("XFakeCall %#llx, ordinal 7 %#llx\n", (unsigned long long)a, (unsigned long long)b);
    /* HRESULTs: callers look at the low 32 bits (EAX). */
    return ((ULONG)a == 0x80004001u && (ULONG)b == 0x80004001u) ? 0 : 1;
}
C
x86_64-w64-mingw32-gcc -O1 -o "$work/game/probe.exe" "$work/probe.c" "$work/libfakexbox.a"

python3 "$here/tools/scan_imports.py" "$work/game" --provider "$WINE_DLL_DIR" --shims "$here/shims" \
    --gen-stubs "$work/gen" >/dev/null || true
[[ -f $work/gen/dlls/fakexbox/fakexbox.def ]] || { echo "FAIL no stub DLL generated"; exit 1; }
make -s -C "$here" GEN="$work/gen" BUILD="$work/build" "$work/build/stub/fakexbox.dll"
cp "$work/build/stub/fakexbox.dll" "$work/game/"

export WINEPREFIX="$work/prefix" WINEDEBUG=-all
"$WINE" wineboot -i >/dev/null 2>&1
if "$WINE" "$work/game/probe.exe"; then
    echo "ok   generated stub DLL loads and returns E_NOTIMPL by name and ordinal"
else
    echo "FAIL generated stub DLL"
    exit 1
fi
