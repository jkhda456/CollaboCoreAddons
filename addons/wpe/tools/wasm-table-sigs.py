#!/usr/bin/env python3
"""Lists what a wasm program's function table holds, by signature: how many functions of each
parameter count are callable through pointers, and the ones with the most parameters (by name when
the program keeps its names). For deciding on function pointer cast emulation.

    wasm-table-sigs.py PROGRAM.wasm [MIN_PARAMS]
"""
import sys
from collections import Counter


def leb(data, pos):
    result = shift = 0
    while True:
        b = data[pos]
        pos += 1
        result |= (b & 0x7F) << shift
        shift += 7
        if not b & 0x80:
            return result, pos


def main():
    data = open(sys.argv[1], "rb").read()
    minimum = int(sys.argv[2]) if len(sys.argv) > 2 else 12
    pos = 8
    types, imported_funcs, func_types, table_funcs, names = [], 0, [], [], {}
    while pos < len(data):
        sid = data[pos]
        size, pos = leb(data, pos + 1)
        end = pos + size
        p = pos
        if sid == 1:  # types
            count, p = leb(data, p)
            for _ in range(count):
                form = data[p]; p += 1
                assert form == 0x60, hex(form)
                n, p = leb(data, p)
                params = data[p:p + n]; p += n
                m, p = leb(data, p)
                results = data[p:p + m]; p += m
                types.append((bytes(params), bytes(results)))
        elif sid == 2:  # imports
            count, p = leb(data, p)
            for _ in range(count):
                for _ in range(2):
                    n, p = leb(data, p); p += n
                kind = data[p]; p += 1
                if kind == 0:
                    t, p = leb(data, p); func_types.append(t); imported_funcs += 1
                elif kind == 1:
                    p += 1; flags, p = leb(data, p); _, p = leb(data, p)
                    if flags & 1: _, p = leb(data, p)
                elif kind == 2:
                    flags, p = leb(data, p); _, p = leb(data, p)
                    if flags & 1: _, p = leb(data, p)
                elif kind == 3:
                    p += 2
        elif sid == 3:  # functions
            count, p = leb(data, p)
            for _ in range(count):
                t, p = leb(data, p); func_types.append(t)
        elif sid == 9:  # elements (the table's function indices)
            count, p = leb(data, p)
            for _ in range(count):
                flags, p = leb(data, p)
                if flags in (0, 4):
                    while data[p] != 0x0B: p += 1
                    p += 1
                elif flags in (2, 6):
                    _, p = leb(data, p)
                    while data[p] != 0x0B: p += 1
                    p += 1
                if flags in (1, 2, 3):
                    p += 1  # elemkind
                n, p = leb(data, p)
                for _ in range(n):
                    if flags & 4:
                        while data[p] != 0x0B: p += 1
                        p += 1
                    else:
                        f, p = leb(data, p); table_funcs.append(f)
        elif sid == 0:  # custom: "name"
            n, q = leb(data, p)
            if data[q:q + n] == b"name":
                q += n
                while q < end:
                    sub = data[q]; subsize, q = leb(data, q + 1)
                    if sub == 1:
                        cnt, r = leb(data, q)
                        for _ in range(cnt):
                            idx, r = leb(data, r); ln, r = leb(data, r)
                            names[idx] = data[r:r + ln].decode("utf-8", "replace"); r += ln
                    q += subsize
        pos = end

    counts = Counter(len(types[func_types[f]][0]) for f in table_funcs)
    print(f"{len(table_funcs)} functions in the table; by parameter count:")
    for k in sorted(counts):
        print(f"  {k:3d} params: {counts[k]}")
    many = sorted({f for f in table_funcs if len(types[func_types[f]][0]) >= minimum},
                  key=lambda f: -len(types[func_types[f]][0]))
    for f in many[:40]:
        print(f"  {len(types[func_types[f]][0]):3d}  {names.get(f, f'#{f}')}")


if __name__ == "__main__":
    main()
