#!/usr/bin/env python3
"""Writes browser-js.h: the browser's scripts (snapshot.js, target.js) as C string constants.

    gen-js.py OUT.h SCRIPT.js...
"""
import json
import os
import sys

with open(sys.argv[1], "w") as out:
    out.write("/* written by gen-js.py from the browser's .js files */\n")
    for path in sys.argv[2:]:
        name = os.path.splitext(os.path.basename(path))[0] + "_js"
        source = open(path, encoding="utf-8").read()
        # the function expression, without the leading comment lines
        lines = [l for l in source.splitlines() if not l.startswith("//")]
        text = "\n".join(lines).strip()
        out.write("static const char %s[] =\n" % name)
        for line in text.splitlines():
            out.write("    %s\n" % json.dumps(line + "\n", ensure_ascii=True))
        out.write(";\n")
