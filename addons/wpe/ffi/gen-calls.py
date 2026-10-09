#!/usr/bin/env python3
"""Writes ffi_calls.c: a call stub per wasm signature for ffi_call (see ffi.h), and their table
sorted by signature. A signature is the result's wasm type, '_', and each argument's:
i (i32), j (i64), f (f32), d (f64), v (no result).

    gen-calls.py OUT.c
"""
import itertools
import sys

CTYPE = {"i": "uint32_t", "j": "uint64_t", "f": "float", "d": "double"}


def signatures():
    sigs = set()
    for r in "vijfd":
        for n in range(0, 7):                       # every mix of i32/i64/f64 up to 6 arguments
            for args in itertools.product("ijd", repeat=n):
                sigs.add(r + "_" + "".join(args))
        for n in range(0, 4):                       # with f32 too, up to 3
            for args in itertools.product("ijfd", repeat=n):
                sigs.add(r + "_" + "".join(args))
        for n in range(7, 13):                      # longer ones: pointers and ints only
            sigs.add(r + "_" + "i" * n)
    return sorted(sigs)


def stub(sig):
    r, args = sig.split("_")
    name = "s_" + sig
    params = ", ".join(CTYPE[a] for a in args) or "void"
    call = "((%s (*)(%s))fn)(%s)" % (
        "void" if r == "v" else CTYPE[r], params,
        ", ".join("*(%s *)av[%d]" % (CTYPE[a], k) for k, a in enumerate(args)))
    body = call + ";" if r == "v" else "*(%s *)rv = %s;" % (CTYPE[r], call)
    unused = "(void)rv; " if r == "v" else ""
    unused += "(void)av; " if not args else ""
    return "static void %s(void (*fn)(void), void *rv, void **av) { %s%s }" % (name, unused, body)


def main():
    sigs = signatures()
    with open(sys.argv[1], "w") as f:
        f.write("/* written by gen-calls.py: a call stub per wasm signature, for ffi_call */\n")
        f.write("#include <stdint.h>\n#include \"ffi.h\"\n\n")
        for s in sigs:
            f.write(stub(s) + "\n")
        f.write("\nstruct ffi_wasm_stub { const char *sig; ffi_wasm_call_t call; };\n")
        f.write("const struct ffi_wasm_stub ffi_wasm_stubs[] = {\n")
        for s in sigs:
            f.write('  {"%s", s_%s},\n' % (s, s))
        f.write("};\nconst unsigned ffi_wasm_stub_count = %d;\n" % len(sigs))


if __name__ == "__main__":
    main()
