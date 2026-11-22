"""Turn the interface definitions of an xgameruntime reimplementation (IDL files: the Xodus project, LGPL-2.1) into a table of interfaces.

usage: python -I idl_table.py <dir with *.idl> <out.json>

The IDL files and the generated table are not part of this repository (the IDLs are LGPL and come from another project);
this script only reads them. Output, per interface: name, uuid, parent, own methods (return type, name, parameter text), and the
vtable slot of each method (slots 0-2 are IUnknown; a derived interface continues after its parent's methods). A slot the IDL declares
without a public name (`HRESULT __PADDING__();`) is kept as an entry marked "unnamed", so the methods after it keep their real slot.
"""
import glob, json, os, re, sys


def strip_comments(t):
    t = re.sub(r"/\*.*?\*/", "", t, flags=re.S)
    return re.sub(r"//[^\n]*", "", t)


def parse(path):
    t = strip_comments(open(path, encoding="utf-8", errors="ignore").read())
    out = []
    # [ attrs ] interface Name : Parent { body }
    for m in re.finditer(r"\[([^\]]*?uuid\(([0-9a-fA-F-]{36})\)[^\]]*?)\]\s*interface\s+(\w+)\s*:\s*(\w+)\s*\{(.*?)\n\}", t, re.S):
        attrs, uuid, name, parent, body = m.groups()
        methods = []
        for mm in re.finditer(r"([\w\s\*]+?)\s+(\bX\w+|\b__PADDING\w*__)\s*\(([^;]*?)\)\s*;", body, re.S):
            ret, mname, params = mm.groups()
            ret = " ".join(ret.split())
            params = " ".join(params.split())
            methods.append({"ret": ret, "name": mname, "params": params, "unnamed": mname.startswith("__")})
        out.append({"name": name, "uuid": uuid.lower(), "parent": parent, "methods": methods, "file": os.path.basename(path)})
    return out


def main():
    src, dst = sys.argv[1], sys.argv[2]
    ifaces = []
    for p in sorted(glob.glob(os.path.join(src, "*.idl"))):
        ifaces += parse(p)
    by = {i["name"]: i for i in ifaces}
    by["IUnknown"] = {"name": "IUnknown", "uuid": "00000000-0000-0000-c000-000000000046", "parent": None, "methods": [], "nslots": 3}

    def nslots(i):
        if "nslots" in i:
            return i["nslots"]
        i["nslots"] = nslots(by[i["parent"]]) + len(i["methods"]) if i["parent"] in by else 3 + len(i["methods"])
        return i["nslots"]

    for i in ifaces:
        n = nslots(i)
        base = n - len(i["methods"])
        for k, m in enumerate(i["methods"]):
            m["slot"] = base + k
    json.dump({"interfaces": ifaces}, open(dst, "w"), indent=1)
    nm = sum(not m["unnamed"] for i in ifaces for m in i["methods"])
    print("%d interfaces, %d methods -> %s" % (len(ifaces), nm, dst))


main()
