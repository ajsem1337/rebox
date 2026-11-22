#!/usr/bin/env bash
# End-to-end check of whole-DLL stub generation. The probe exe imports:
#   - fakexbox.dll, which nobody provides, by name, by ordinal and by a name
#     pefile would drop ("X-Odd");
#   - pkgfwd.dll, a "package" DLL whose exports forward to missingx.dll (by
#     name and by ordinal), which nobody provides either.
#   - api-ms-win-xbox-fake-l1-1-0.dll, an API set Wine's schema doesn't
#     host: GetTickCount (which kernelbase has) and XSetFake (nobody has).
# The scan must generate stub DLLs for fakexbox, missingx and the API set
# (forwarding GetTickCount to kernelbase); once built, the exe must load,
# get E_NOTIMPL from every stub and the real GetTickCount.
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

cat > "$work/apiset.def" <<'DEF'
LIBRARY api-ms-win-xbox-fake-l1-1-0.dll
EXPORTS
    XSetFake
    XGetTickCount == GetTickCount
DEF
x86_64-w64-mingw32-dlltool -d "$work/apiset.def" -l "$work/libapiset.a"

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
__declspec(dllimport) LONG_PTR XSetFake(void);
__declspec(dllimport) DWORD XGetTickCount(void);
int main(void)
{
    LONG_PTR r[] = { XFakeCall(), XFakeOrdinal(), XOdd(), XFwdName(), XFwdOrdinal(), XSetFake() };
    int ok = 1;
    DWORD ticks = XGetTickCount();
    printf("GetTickCount through the API set -> %lu\n", ticks);
    ok &= ticks != 0 && ticks - GetTickCount() < 1000u;
    for (int i = 0; i < 6; i++)
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
x86_64-w64-mingw32-gcc -O1 -o "$work/game/probe.exe" "$work/probe.c" "$work/libfakexbox.a" "$work/libpkgfwd.a" "$work/libapiset.a"

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
set=api-ms-win-xbox-fake-l1-1-0
grep -q '"GetTickCount" = kernelbase.GetTickCount' "$work/gen/dlls/$set/$set.def" 2>/dev/null \
    && grep -q '"XSetFake"' "$work/gen/dlls/$set/stubs.c" && grep -q 'UNKNOWN SET' "$work/scan.txt" \
    || { cat "$work/scan.txt"; fail "an API set Wine doesn't host gets no DLL forwarding to kernelbase"; }
echo "ok   an API set Wine's schema doesn't host gets a DLL: forwards where a core DLL has the name, stubs elsewhere"

make -s -C "$here" GEN="$work/gen" BUILD="$work/build" "$work/build/stub/fakexbox.dll" "$work/build/stub/missingx.dll" \
    "$work/build/stub/$set.dll"
cp "$work/build/stub/fakexbox.dll" "$work/build/stub/missingx.dll" "$work/build/stub/$set.dll" "$work/game/"

export WINEPREFIX="$work/prefix" WINEDEBUG=-all
"$WINE" wineboot -i >/dev/null 2>&1
if "$WINE" "$work/game/probe.exe"; then
    echo "ok   generated stub DLLs load and return E_NOTIMPL by name, ordinal, odd name and through forwarders"
else
    fail "generated stub DLLs"
fi

# A name missing from an API set Wine hosts can't be stubbed by a DLL (the schema wins), so Wine
# binds a stub that aborts when called. xmem.dll, which titles import, repoints those at E_NOTIMPL.
cat > "$work/hosted.def" <<'DEF'
LIBRARY api-ms-win-core-file-l1-1-0.dll
EXPORTS
    XHostedButMissing
DEF
x86_64-w64-mingw32-dlltool -d "$work/hosted.def" -l "$work/libhosted.a"
x86_64-w64-mingw32-dlltool -d "$here/shims/xmem/xmem.def" -l "$work/libxmem.a"
cat > "$work/fixup.c" <<'C'
#include <stdio.h>
#include <windows.h>
__declspec(dllimport) LONG_PTR XHostedButMissing(void);
__declspec(dllimport) void *XMemAlloc(SIZE_T, ULONGLONG);
int main(void)
{
    void *volatile keep = XMemAlloc;
    LONG_PTR r = XHostedButMissing();
    printf("XHostedButMissing -> %#llx\n", (unsigned long long)r);
    return keep && (ULONG)r == 0x80004001u ? 0 : 1;
}
C
x86_64-w64-mingw32-gcc -O1 -o "$work/game/fixup.exe" "$work/fixup.c" "$work/libhosted.a" "$work/libxmem.a"
make -s -C "$here" BUILD="$work/build" "$work/build/xmem.dll"
cp "$work/build/xmem.dll" "$work/game/"
if "$WINE" "$work/game/fixup.exe"; then
    echo "ok   a name missing from an API set Wine hosts returns E_NOTIMPL instead of aborting"
else
    fail "xmem's import fixup"
fi

# The same for a DLL loaded later with LoadLibrary, before its DllMain runs.
cat > "$work/late.c" <<'C'
#include <windows.h>
__declspec(dllimport) LONG_PTR XHostedButMissing(void);
LONG_PTR late_result;
__declspec(dllexport) LONG_PTR late_call(void) { return XHostedButMissing(); }
BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, void *r)
{
    if (reason == DLL_PROCESS_ATTACH) late_result = XHostedButMissing();
    return TRUE;
}
C
x86_64-w64-mingw32-gcc -O1 -shared -o "$work/game/late.dll" "$work/late.c" "$work/libhosted.a"
cat > "$work/lateload.c" <<'C'
#include <stdio.h>
#include <windows.h>
__declspec(dllimport) void *XMemAlloc(SIZE_T, ULONGLONG);
int main(void)
{
    void *volatile keep = XMemAlloc;
    HMODULE m = LoadLibraryA("late.dll");
    LONG_PTR (*call)(void) = m ? (void *)GetProcAddress(m, "late_call") : NULL;
    LONG_PTR r = call ? call() : 0;
    printf("late.dll XHostedButMissing -> %#llx\n", (unsigned long long)r);
    return keep && (ULONG)r == 0x80004001u ? 0 : 1;
}
C
x86_64-w64-mingw32-gcc -O1 -o "$work/game/lateload.exe" "$work/lateload.c" "$work/libxmem.a"
if "$WINE" "$work/game/lateload.exe"; then
    echo "ok   the same holds for a DLL loaded later, including calls from its DllMain"
else
    fail "xmem's import fixup for a DLL loaded later"
fi
