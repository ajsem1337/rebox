#!/usr/bin/env bash
# End-to-end check of whole-DLL stub generation. The probe exe imports:
#   - fakexbox.dll, which nobody provides, by name, by ordinal and by a name
#     pefile would drop ("X-Odd");
#   - pkgfwd.dll, a "package" DLL whose exports forward to missingx.dll (by
#     name and by ordinal), which nobody provides either.
# The scan must generate stub DLLs for fakexbox and missingx; once built,
# the exe must load and get E_NOTIMPL from every call.
# Needs: WINE, WINE_DLL_DIR, x86_64-w64-mingw32-gcc/dlltool (nix develop).
set -euo pipefail

here=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap '[[ -n ${KEEP:-} ]] || rm -rf "$work"' EXIT
: "${WINE:?}" "${WINE_DLL_DIR:?}"
mkdir -p "$work/game"
fail() { echo "FAIL $*"; exit 1; }

cat > "$work/fake.def" <<'DEF'
LIBRARY fakexbox.dll
EXPORTS
    XFakeCall
    IsDebuggerPresent
    XFakeOrdinal @7 NONAME
    XOdd == "X-Odd"
DEF
x86_64-w64-mingw32-dlltool -d "$work/fake.def" -l "$work/libfakexbox.a"

# The package DLL: two forwarders, built for real so the scan reads them.
cat > "$work/pkgfwd.def" <<'DEF'
LIBRARY pkgfwd.dll
EXPORTS
    XFwdName = missingx.XRealName
    XFwdOrdinal = "missingx.#9"
DEF
echo 'int pkgfwd_unused;' > "$work/pkgfwd.c"
x86_64-w64-mingw32-gcc -shared -nostdlib -Wl,--entry=0 -o "$work/game/pkgfwd.dll" "$work/pkgfwd.c" "$work/pkgfwd.def"
x86_64-w64-mingw32-dlltool -d "$work/pkgfwd.def" -l "$work/libpkgfwd.a"

cat > "$work/probe.c" <<'C'
#include <stdio.h>
#include <windows.h>
__declspec(dllimport) LONG_PTR XFakeCall(void);
__declspec(dllimport) LONG_PTR IsDebuggerPresent_(void) __asm__("IsDebuggerPresent");
__declspec(dllimport) LONG_PTR XFakeOrdinal(void);
__declspec(dllimport) LONG_PTR XOdd(void);
__declspec(dllimport) LONG_PTR XFwdName(void);
__declspec(dllimport) LONG_PTR XFwdOrdinal(void);
int main(void)
{
    LONG_PTR r[] = { XFakeCall(), XFakeOrdinal(), XOdd(), XFwdName(), XFwdOrdinal() };
    int ok = 1;
    for (int i = 0; i < 5; i++)
    {
        printf("call %d -> %#llx\n", i, (unsigned long long)r[i]);
        /* HRESULTs: callers look at the low 32 bits (EAX). */
        ok &= (ULONG)r[i] == 0x80004001u;
    }
    /* A name the Windows headers declare returning BOOL gets 0, not E_NOTIMPL. */
    printf("IsDebuggerPresent -> %#llx\n", (unsigned long long)IsDebuggerPresent_());
    ok &= IsDebuggerPresent_() == 0;
    return ok ? 0 : 1;
}
C
x86_64-w64-mingw32-gcc -O1 -o "$work/game/probe.exe" "$work/probe.c" "$work/libfakexbox.a" "$work/libpkgfwd.a"

python3 "$here/tools/scan_imports.py" "$work/game" --provider "$WINE_DLL_DIR" --shims "$here/shims" \
    --gen-stubs "$work/gen" --prototypes-from x86_64-w64-mingw32-gcc > "$work/scan.txt" 2>&1 || true
grep -q 'REBOX_STUB_ZERO(fakexbox, [a-z0-9_]*, "IsDebuggerPresent")' "$work/gen/dlls/fakexbox/stubs.c" \
    || { cat "$work/scan.txt"; fail "a BOOL function's stub doesn't return 0"; }
grep -q '"X-Odd"' "$work/gen/dlls/fakexbox/fakexbox.def" 2>/dev/null \
    || { cat "$work/scan.txt"; fail "import name pefile rejects is missing from the fakexbox stub"; }
grep -q '"XRealName"' "$work/gen/dlls/missingx/missingx.def" 2>/dev/null && grep -q '@9 NONAME' "$work/gen/dlls/missingx/missingx.def" \
    || { cat "$work/scan.txt"; fail "no stub DLL for the forwarders' target (by name and ordinal)"; }
grep -q 'via pkgfwd.XFwdName' "$work/scan.txt" || { cat "$work/scan.txt"; fail "report doesn't name the forwarder"; }
echo "ok   scan follows forwarders to a missing DLL and keeps names pefile drops"

make -s -C "$here" GEN="$work/gen" BUILD="$work/build" "$work/build/stub/fakexbox.dll" "$work/build/stub/missingx.dll"
cp "$work/build/stub/fakexbox.dll" "$work/build/stub/missingx.dll" "$work/game/"

export WINEPREFIX="$work/prefix" WINEDEBUG=-all
"$WINE" wineboot -i >/dev/null 2>&1
if "$WINE" "$work/game/probe.exe"; then
    echo "ok   generated stub DLLs load and return E_NOTIMPL by name, ordinal, odd name and through forwarders"
else
    fail "generated stub DLLs"
fi
