#!/usr/bin/env python3
"""Finds function pointer casts that WebAssembly cannot call: clang's -Wcast-function-type-strict
names every cast between function types; this keeps those whose *wasm* signatures differ (a
different number of parameters, an i64 or a double where an i32 was, a result where none was).
Casts to GCallback / void (*)(void) alone (G_CALLBACK) are left out: what they are called as is
decided elsewhere (a signal's signature).

    fpcast-scan.py BUILD_DIR [FILTER]   runs each compile command of BUILD_DIR's compile_commands.json
                                        (or `ninja -t compdb`) whose file matches FILTER, syntax only
"""
import json
import os
import re
import shlex
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

I64 = {"long long", "unsigned long long", "gint64", "guint64", "goffset", "int64_t", "uint64_t", "time_t",
       "off_t", "off64_t", "gtime_t", "GTimeSpan", "ssize64", "quint64", "__int64", "TimeSpan"}
F64 = {"double", "gdouble", "long double"}
F32 = {"float", "gfloat"}
WARN = re.compile(r"^(.*?):(\d+):\d+: warning: cast from '([^']*)'(?: \(aka '([^']*)'\))? to '([^']*)'(?: \(aka '([^']*)'\))? "
                  r"converts to incompatible function type")


def split_top(s):
    out, depth, cur = [], 0, ""
    for c in s:
        if c in "(<[":
            depth += 1
        elif c in ")>]":
            depth -= 1
        if c == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += c
    if cur.strip():
        out.append(cur.strip())
    return out


def wasm_type(t):
    t = t.strip()
    t = re.sub(r"\b(const|volatile|struct|enum|union|restrict|__restrict)\b", "", t).strip()
    t = re.sub(r"\s+", " ", t)
    if "*" in t or "&" in t or "[" in t or "(" in t:
        return "i"
    if t in I64:
        return "j"
    if t in F64:
        return "d"
    if t in F32:
        return "f"
    if t in ("void",):
        return "v"
    return "i"


def signature(fptr):
    """'int (*)(void *, long long)' -> 'i_ij' ; None when not a plain function pointer type"""
    m = re.match(r"^(.*?)\s*\(\*\)\s*\((.*)\)\s*$", fptr.strip())
    if not m:
        m = re.match(r"^(.*?)\s*\((.*)\)\s*$", fptr.strip())  # a function type
        if not m:
            return None
    ret, params = m.group(1), m.group(2)
    if params.strip().endswith("..."):
        return None
    ps = [] if params.strip() in ("", "void") else [wasm_type(p) for p in split_top(params)]
    r = wasm_type(ret)
    return r + "_" + "".join(ps)


def compdb(build):
    path = os.path.join(build, "compile_commands.json")
    if os.path.exists(path):
        return json.load(open(path))
    out = subprocess.run(["ninja", "-C", build, "-t", "compdb"], capture_output=True, text=True, check=True).stdout
    return json.loads(out)


def scan(entry):
    args = entry.get("arguments") or shlex.split(entry["command"])
    # syntax only, every other warning off
    keep, skip = [], False
    for a in args:
        if skip:
            skip = False
            continue
        if a in ("-o", "-MF", "-MQ", "-MT"):
            skip = True
            continue
        if a.startswith("-W") or a in ("-c", "-MD", "-MMD"):
            continue
        keep.append(a)
    if not keep or keep[0].startswith("-") or "&&" in keep:
        return ""  # not a plain compile command (a generator step ninja lists too)
    keep += ["-fsyntax-only", "-Wno-everything", "-Wcast-function-type-strict", "-fno-color-diagnostics"]
    r = subprocess.run(keep, cwd=entry["directory"], capture_output=True, text=True)
    return r.stderr


def main():
    build = sys.argv[1]
    flt = re.compile(sys.argv[2]) if len(sys.argv) > 2 else None
    entries = [e for e in compdb(build) if (not flt or flt.search(e["file"])) and e["file"].endswith((".c", ".cpp", ".cc"))]
    found = set()
    with ThreadPoolExecutor(max_workers=int(os.environ.get("JOBS", "6"))) as pool:
        for text in pool.map(scan, entries):
            pending = None
            for line in text.splitlines():
                m = WARN.match(line)
                if not m:
                    continue
                path, lineno, src, src_aka, dst, dst_aka = m.groups()
                src_t, dst_t = src_aka or src, dst_aka or dst
                if dst_t.replace(" ", "") == "void(*)(void)":
                    pending = (path, lineno, src_t)   # the first half of (T)(void (*)(void)) f
                    continue
                if src_t.replace(" ", "") == "void(*)(void)" and pending and pending[:2] == (path, lineno):
                    src_t = pending[2]
                pending = None
                a, b = signature(src_t), signature(dst_t)
                if a is None or b is None or a == b:
                    continue
                key = (os.path.normpath(os.path.join(build, path)) if not os.path.isabs(path) else path, int(lineno), a, b)
                found.add(key)
    for path, lineno, a, b in sorted(found):
        print(f"{path}:{lineno}: a {a} function called as {b}")
    print(f"{len(found)} casts whose wasm signatures differ", file=sys.stderr)


if __name__ == "__main__":
    main()
