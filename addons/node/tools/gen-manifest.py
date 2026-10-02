#!/usr/bin/env python3
"""Lists the builtin modules for js2bc: "id<TAB>path" lines.

    tools/gen-manifest.py NODE_SRC EXTRA_SRC ADDON_LIB > manifest.txt

Node's lib/ (as builtin ids), the deps Node bundles as internal/deps/*, and
the add-on's own lib/, whose files replace or add to Node's of the same id.
"""
import os
import sys

node, extra, addon_lib = sys.argv[1:4]
mods = {}
for root, _, files in os.walk(os.path.join(node, 'lib')):
    for f in files:
        if f.endswith('.js'):
            p = os.path.join(root, f)
            mods[os.path.relpath(p, os.path.join(node, 'lib'))[:-3]] = p
deps = {
    'internal/deps/acorn/acorn/dist/acorn': 'deps/acorn/acorn/dist/acorn.js',
    'internal/deps/acorn/acorn-walk/dist/walk': 'deps/acorn/acorn-walk/dist/walk.js',
    'internal/deps/minimatch/index': 'deps/minimatch/index.js',
    'internal/deps/undici/undici': 'deps/undici/undici.js',
}
for k, v in deps.items():
    mods[k] = os.path.join(node, v)
mods['internal/deps/cjs-module-lexer/lexer'] = os.path.join(extra, 'cjs-module-lexer/lexer.js')
for root, _, files in os.walk(addon_lib):
    for f in files:
        if f.endswith('.js'):
            p = os.path.join(root, f)
            mods[os.path.relpath(p, addon_lib)[:-3]] = p
for k in sorted(mods):
    print('%s\t%s' % (k, os.path.abspath(mods[k])))
