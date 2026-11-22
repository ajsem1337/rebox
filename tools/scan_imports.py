#!/usr/bin/env python3
"""Report which imports of a dumped console title are unresolved under Wine,
and optionally generate logging stubs for the shim DLLs.

    scan_imports.py GAME_DIR --provider DIR [--provider DIR ...]
                    [--shims SHIM_SRC_DIR --gen-stubs OUT_DIR]

--gen-stubs writes two kinds of stubs into OUT_DIR:
  <shim>_stubs.c/.def    exports a shim DLL lacks (built into that shim)
  dlls/<name>/           a whole stub DLL for each imported DLL that nobody
                         provides, so the title still loads; the Makefile
                         builds them and rebox-run installs them

Providers are directories of PE DLLs that will be loadable at runtime: Wine's
lib/wine/x86_64-windows, vkd3d-proton's x64 dir, the built shims. DLLs in
GAME_DIR itself count as providers too.

api-ms-win-*/ext-ms-* imports are resolved against the union of Wine's core
DLLs, which is roughly what Wine's apiset schema forwards to.
"""

import argparse
import collections
import pathlib
import re
import subprocess
import sys

import pefile

APISET_HOSTS = [
    "ntdll", "kernelbase", "kernel32", "ucrtbase", "msvcrt", "combase", "ole32",
    "oleaut32", "advapi32", "sechost", "user32", "gdi32", "shcore", "ws2_32",
    "bcrypt", "crypt32", "shell32", "shlwapi", "rpcrt4", "winhttp", "iphlpapi",
    "powrprof", "version", "dbghelp", "mswsock", "cfgmgr32", "setupapi", "xinput1_4",
    "windows.storage", "twinapi.appcore", "wintypes",
]


# Import and export tables are read here rather than through pefile's parsed
# entries: pefile drops names outside letters, digits and ._?@$()<> and
# descriptors it left empty (Xbox2Windows found such names in packages).

def _cstr(pe, rva, limit=4096):
    return pe.get_data(rva, limit).split(b"\0", 1)[0].decode("latin-1")


def _directory(pe, name):
    d = pe.OPTIONAL_HEADER.DATA_DIRECTORY[pefile.DIRECTORY_ENTRY[name]]
    return d.VirtualAddress, d.Size


def pe_exports(path):
    """(names, ordinals, forwards): forwards maps a name or ordinal to its
    forwarder string ("dll.name" or "dll.#ordinal")."""
    try:
        pe = pefile.PE(str(path), fast_load=True)
        names, ordinals, forwards = set(), set(), {}
        rva, size = _directory(pe, "IMAGE_DIRECTORY_ENTRY_EXPORT")
        if not rva:
            return names, ordinals, forwards
        base = pe.get_dword_at_rva(rva + 16)
        nfuncs, nnames = pe.get_dword_at_rva(rva + 20), pe.get_dword_at_rva(rva + 24)
        funcs, name_ptrs, name_ords = (pe.get_dword_at_rva(rva + o) for o in (28, 32, 36))
        by_index = {}
        for i in range(nfuncs):
            target = pe.get_dword_at_rva(funcs + 4 * i)
            if not target:
                continue
            ordinals.add(base + i)
            if rva <= target < rva + size:
                by_index[i] = _cstr(pe, target, 512)
                forwards[base + i] = by_index[i]
        for i in range(nnames):
            name = _cstr(pe, pe.get_dword_at_rva(name_ptrs + 4 * i), 512)
            index = pe.get_word_at_rva(name_ords + 2 * i)
            names.add(name)
            if index in by_index:
                forwards[name] = by_index[index]
        return names, ordinals, forwards
    except (pefile.PEFormatError, TypeError, AttributeError, IndexError):
        return None


def pe_imports(path):
    pe = pefile.PE(str(path), fast_load=True)
    out = collections.defaultdict(set)
    for dirname, desc_size, name_off, thunk_offs in (
            ("IMAGE_DIRECTORY_ENTRY_IMPORT", 20, 12, (0, 16)),        # OriginalFirstThunk, else FirstThunk
            ("IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT", 32, 4, (16,))):    # ImportNameTable
        rva, _ = _directory(pe, dirname)
        while rva:
            raw = pe.get_data(rva, desc_size)
            if len(raw) < desc_size or not any(raw):
                break
            dll = _cstr(pe, pe.get_dword_at_rva(rva + name_off), 256).lower()
            out[dll]  # a descriptor with no thunks still names a DLL the loader must find
            thunk = next((t for t in (pe.get_dword_at_rva(rva + o) for o in thunk_offs) if t), 0)
            while thunk:
                v = pe.get_qword_at_rva(thunk)
                if not v:
                    break
                out[dll].add(v & 0xFFFF if v >> 63 else _cstr(pe, (v & 0x7FFFFFFF) + 2, 512))
                thunk += 8
            rva += desc_size
    return out


def split_forward(fwd):
    """'dll.name' / 'dll.#7' -> ('dll.dll', 'name' / 7)."""
    dll, _, sym = fwd.rpartition(".")
    dll = dll.lower() + ".dll"
    return dll, int(sym[1:]) if sym.startswith("#") and sym[1:].isdigit() else sym


def load_providers(dirs):
    providers = {}
    for d in dirs:
        for p in sorted(pathlib.Path(d).glob("*.dll")):
            key = p.name.lower()
            if key in providers:
                continue  # earlier providers win, like a search path
            exp = pe_exports(p)
            if exp is not None:
                providers[key] = (exp, str(p))
    return providers


def def_names(path):
    """Export names already declared in a hand-written .def file."""
    names = set()
    in_exports = False
    for line in pathlib.Path(path).read_text().splitlines():
        line = line.split(";", 1)[0].strip()
        if not line:
            continue
        if line.upper() == "EXPORTS":
            in_exports = True
            continue
        if in_exports:
            names.add(re.split(r"[\s=@]", line, maxsplit=1)[0].strip('"'))
    return names


# Return classes of functions the Windows headers declare (--prototypes-from CC): a stub for an
# HRESULT function, or one with no known prototype, returns E_NOTIMPL; one returning anything
# else 0 (FALSE, NULL) with ERROR_CALL_NOT_IMPLEMENTED; a void one nothing. A pointer or handle
# caller would take E_NOTIMPL for an address.
_returns = {}
_DECL = re.compile(r"(?:^|[;}])\s*(?:WINBASEAPI|WINUSERAPI|WINADVAPI|DECLSPEC_IMPORT|__declspec\(dllimport\)|"
                   r"WINGDIAPI|STDAPI_?\(?|extern|WINAPI_INLINE|NTSYSAPI)?([\w\s\*]*?)\s*"
                   r"(?:WINAPI|APIENTRY|NTAPI|__stdcall|STDAPICALLTYPE|CALLBACK)\s+(\w+)\s*\(", re.M)


def load_prototypes(cc):
    """Index the headers next to the compiler's windows.h: function name -> return class."""
    try:
        deps = subprocess.run([cc, "-M", "-x", "c", "-"], input="#include <windows.h>\n", capture_output=True,
                              text=True, timeout=60).stdout
    except (OSError, subprocess.SubprocessError):
        return
    header = next((p for p in re.split(r"[\s\\]+", deps) if p.endswith("/windows.h")), None)
    if not header:
        return
    imported = {}
    for path in sorted(pathlib.Path(header).parent.glob("*.h")):
        try:
            text = path.read_text(errors="ignore")
        except OSError:
            continue
        for m in _DECL.finditer(text):
            decl, ret, name = m.group(0), " ".join(m.group(1).split()), m.group(2)
            if "virtual" in decl or "STDMETHOD" in decl:  # a COM method of the same name
                continue
            if "STDAPI" in decl and not ret:
                ret = "HRESULT"
            # An exported function's declaration beats a typedef or inline helper of that name.
            marked = any(k in decl for k in ("WINBASEAPI", "WINUSERAPI", "WINADVAPI", "dllimport", "WINGDIAPI",
                                              "STDAPI", "NTSYSAPI"))
            if not ret or (name in _returns and (imported.get(name) or not marked)):
                continue
            _returns[name] = "hresult" if "HRESULT" in ret else "void" if ret in ("void", "VOID") else "zero"
            imported[name] = marked
    for name, kind in list(_returns.items()):  # A/W variants answer for the plain name
        if name[-1:] in "AW":
            _returns.setdefault(name[:-1], kind)


def stub_macro(name):
    kind = _returns.get(name) if isinstance(name, str) else None
    return {"zero": "REBOX_STUB_ZERO", "void": "REBOX_STUB_VOID"}.get(kind, "REBOX_STUB")


def write_stub_dll(directory, base, syms):
    """A complete DLL whose every export logs once and returns E_NOTIMPL."""
    directory.mkdir(parents=True, exist_ok=True)
    ident_base = re.sub(r"\W", "_", base)
    c = ["/* Generated by tools/scan_imports.py; do not edit. Every export is a stub:",
         f" * nothing provides {base}.dll. */",
         '#include "rebox_log.h"', ""]
    d = [f"LIBRARY {base}.dll", "EXPORTS"]
    for i, s in enumerate(sorted(syms, key=str)):
        ident = f"{ident_base}_{i:04d}"
        label = s if isinstance(s, str) else f"#{s}"
        c.append(f"{stub_macro(s)}({base}, {ident}, \"{label}\")")
        if isinstance(s, str):
            d.append(f'    "{s}" = stub_{ident}')
        else:
            d.append(f"    ord{s} = stub_{ident} @{s} NONAME")
    (directory / "stubs.c").write_text("\n".join(c) + "\n")
    (directory / f"{base}.def").write_text("\n".join(d) + "\n")


def is_apiset(dll):
    return dll.startswith(("api-ms-win-", "ext-ms-"))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("game_dir")
    ap.add_argument("--provider", action="append", default=[])
    ap.add_argument("--shims", help="shim source dir (shims/), enables stub generation targets")
    ap.add_argument("--gen-stubs", metavar="OUT_DIR")
    ap.add_argument("--prototypes-from", metavar="CC",
                    help="mingw compiler whose Windows headers give generated stubs their return class")
    ap.add_argument("--all", action="store_true", help="list every imported DLL, not just problems")
    args = ap.parse_args()

    game = pathlib.Path(args.game_dir)
    targets = sorted(p for p in game.iterdir() if p.suffix.lower() in (".exe", ".dll"))
    if not any(p.suffix.lower() == ".exe" for p in targets):
        print(f"note: no .exe in {game}; scanning the DLLs only", file=sys.stderr)

    # Exes without relocations load only at their preferred base (Leonida found the game exe
    # can't be relocated under Wine). rebox-run starts the exe as Wine's main image, which gets
    # it; anything placed in front of it (a loader, an injector) must keep that range free.
    for t in targets:
        if t.suffix.lower() != ".exe":
            continue
        try:
            pe = pefile.PE(str(t), fast_load=True)
        except pefile.PEFormatError:
            continue
        opt = pe.OPTIONAL_HEADER
        reloc = opt.DATA_DIRECTORY[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_BASERELOC"]]
        dynamic = bool(opt.DllCharacteristics & 0x40)
        fixed = not reloc.Size or (pe.FILE_HEADER.Characteristics & 0x1)
        print(f"{t.name}: image base {opt.ImageBase:#x}, size {opt.SizeOfImage:#x}, "
              f"{'not relocatable' if fixed else 'relocatable'}{', ASLR' if dynamic else ''}", file=sys.stderr)

    providers = load_providers(args.provider + [str(game)])
    apiset_names = set()
    for host in APISET_HOSTS:
        if host + ".dll" in providers:
            apiset_names |= providers[host + ".dll"][0][0]

    shim_names = set()
    if args.shims:
        shim_names = {p.name for p in pathlib.Path(args.shims).iterdir() if (p / f"{p.name}.def").exists()}

    # dll -> symbol -> set of importing modules
    wanted = collections.defaultdict(lambda: collections.defaultdict(set))
    for t in targets:
        for dll, syms in pe_imports(t).items():
            for s in syms:
                wanted[dll][s].add(t.name)

    # Forwarded exports: binding to one makes the loader resolve its target
    # too, which can be missing even when the forwarding DLL is present.
    # Follow each chain and import the final target as well.
    def lookup(dll, sym):
        (names, ordinals, forwards), _ = providers[dll]
        found = sym in names if isinstance(sym, str) else sym in ordinals
        return found, forwards.get(sym) if found else None

    queue = [(dll, s, u) for dll in wanted if dll in providers and not is_apiset(dll)
             for s in wanted[dll] for u in wanted[dll][s]]
    forwarded = set()
    while queue:
        dll, sym, user = queue.pop()
        if (dll, sym) in forwarded:
            continue
        forwarded.add((dll, sym))
        found, fwd = lookup(dll, sym)
        if not fwd:
            continue
        tdll, tsym = split_forward(fwd)
        if tdll == dll and tsym == sym:
            continue
        wanted[tdll][tsym].add(f"{user} via {dll[:-4]}.{sym if isinstance(sym, str) else '#%d' % sym}")
        if tdll in providers and not is_apiset(tdll):
            queue.append((tdll, tsym, user))

    problems = 0
    stubs = collections.defaultdict(set)
    missing_dlls = {}
    for dll in sorted(wanted):
        syms = wanted[dll]
        base = dll[:-4] if dll.endswith(".dll") else dll
        if is_apiset(dll):
            missing = {s for s in syms if not isinstance(s, str) or s not in apiset_names}
            where = "apiset"
        elif dll in providers:
            (names, ordinals, _), where = providers[dll]
            missing = {s for s in syms if (s not in names if isinstance(s, str) else s not in ordinals)}
        else:
            missing, where = set(syms), None

        if base in shim_names:
            stubs[base] |= missing
        elif where is None and not is_apiset(dll):
            missing_dlls[base] = set(syms)
        status = "MISSING DLL" if where is None else ("ok" if not missing else f"{len(missing)} missing")
        if where is None or missing or args.all:
            if where is None or missing:
                problems += 1
            users = sorted({u for s in syms for u in syms[s]})
            print(f"{dll:55} {len(syms):4} imported  {status:12} {where or ''}")
            print(f"    used by: {', '.join(users)}")
            for s in sorted(missing, key=str)[:60]:
                print(f"    - {s if isinstance(s, str) else '#%d' % s}")
            if len(missing) > 60:
                print(f"    ... {len(missing) - 60} more")

    if args.gen_stubs:
        if args.prototypes_from:
            load_prototypes(args.prototypes_from)
            print(f"{len(_returns)} prototypes from {args.prototypes_from}'s headers", file=sys.stderr)
        out = pathlib.Path(args.gen_stubs)
        out.mkdir(parents=True, exist_ok=True)
        for base in sorted(shim_names):
            handwritten = def_names(pathlib.Path(args.shims) / base / f"{base}.def")
            syms = sorted((s for s in stubs.get(base, ()) if s not in handwritten), key=str)
            c = [f"/* Generated by tools/scan_imports.py; do not edit. */",
                 '#include "../shims/common/rebox_log.h"', ""]
            d = []
            for i, s in enumerate(syms):
                ident = f"{base}_{i:04d}"
                label = s if isinstance(s, str) else f"#{s}"
                c.append(f"{stub_macro(s)}({base}, {ident}, \"{label}\")")
                if isinstance(s, str):
                    d.append(f'    "{s}" = stub_{ident}')
                else:
                    d.append(f"    ord{s} = stub_{ident} @{s} NONAME")
            (out / f"{base}_stubs.c").write_text("\n".join(c) + "\n")
            (out / f"{base}_stubs.def").write_text("\n".join(d) + "\n")
            print(f"generated {len(syms)} stubs for {base}.dll", file=sys.stderr)
        for base, syms in sorted(missing_dlls.items()):
            write_stub_dll(out / "dlls" / base, base, syms)
            print(f"generated stub DLL {base}.dll ({len(syms)} exports)", file=sys.stderr)

    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
