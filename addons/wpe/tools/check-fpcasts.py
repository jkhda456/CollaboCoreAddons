#!/usr/bin/env python3
"""Finds GObject type functions registered with fewer parameters than GLib calls them with:
interface init functions (G_IMPLEMENT_INTERFACE, GInterfaceInfo), class and instance init and
finalize functions (GTypeInfo, g_type_register_static_simple) — on WebAssembly such a call traps
("indirect call type mismatch"), where native code gets away with it. GLib's own type macros are
fixed by patches/glib/0002; this finds what code does by hand.

    check-fpcasts.py [--fix] DIR...    prints FILE:LINE: NAME has N parameter(s), called with M
                                       --fix: adds the missing (unused) parameters to the
                                       declarations and definitions of what it finds
"""
import os
import re
import sys

# what GLib calls each kind of function with (gtype.h)
ARITY = {"GInterfaceInitFunc": 2, "GInterfaceFinalizeFunc": 2, "GClassInitFunc": 2, "GClassFinalizeFunc": 2,
         "GInstanceInitFunc": 2, "GBaseInitFunc": 1, "GBaseFinalizeFunc": 1}
IMPLEMENT = re.compile(r"G_IMPLEMENT_INTERFACE(?:_DYNAMIC)?\s*\(\s*[\w:]+(?:\s*\(\s*\))?\s*,\s*"
                       r"(?:\(\s*GInterfaceInitFunc\s*\)\s*)?([A-Za-z_]\w*)\s*\)")
CAST = re.compile(r"\(\s*(" + "|".join(ARITY) + r")\s*\)\s*(?:\(\s*void\s*\(\s*\*\s*\)\s*\(\s*void\s*\)\s*\)\s*)?([A-Za-z_]\w*)")
DECL = r"(?:^|[\s*])(?:void|gboolean)\s*\**\s*\n?\s*{name}\s*\("


def declarations(text, name):
    """(start of parameter list, end) of each declaration or definition of name"""
    spans = []
    for m in re.finditer(r"\b" + re.escape(name) + r"\s*\(", text):
        line_start = text.rfind("\n", 0, m.start())
        prev_start = text.rfind("\n", 0, max(line_start, 0))
        before = text[prev_start + 1:m.start()]
        if not re.search(r"\b(void|gboolean)\b\s*\**\s*$", before.replace("\n", " ")) and not re.search(r"^\s*static\b", before):
            continue
        start = m.end()
        depth, i = 1, start
        while i < len(text) and depth:
            depth += {"(": 1, ")": -1}.get(text[i], 0)
            i += 1
        spans.append((start, i - 1))
    return spans


def count_params(inner):
    inner = inner.strip()
    if inner in ("", "void"):
        return 0
    depth, count = 0, 1
    for c in inner:
        if c in "(<":
            depth += 1
        elif c in ")>":
            depth -= 1
        elif c == "," and depth == 0:
            count += 1
    return count


def main():
    fix = "--fix" in sys.argv
    roots = [a for a in sys.argv[1:] if a != "--fix"]
    problems = 0
    for root in roots:
        for dirpath, _, files in os.walk(root):
            for f in sorted(files):
                if not f.endswith((".c", ".cpp")):
                    continue
                path = os.path.join(dirpath, f)
                text = open(path, encoding="utf-8", errors="replace").read()
                found = {}
                for m in IMPLEMENT.finditer(text):
                    found.setdefault(m.group(1), (2, m.start()))
                for m in CAST.finditer(text):
                    found.setdefault(m.group(2), (ARITY[m.group(1)], m.start()))
                edits = []
                for name, (want, at) in found.items():
                    if name in ("NULL", "nullptr") or name.startswith("G_"):
                        continue
                    spans = declarations(text, name)
                    if not spans:
                        continue
                    have = count_params(text[spans[0][0]:spans[0][1]])
                    if have >= want:
                        continue
                    line = text.count("\n", 0, at) + 1
                    print(f"{path}:{line}: {name} has {have} parameter(s), called with {want}")
                    problems += 1
                    for start, end in spans:
                        if count_params(text[start:end]) == have:
                            # C++: unnamed (no unused-parameter warning); C needs names
                            extra = ", ".join("gpointer" if path.endswith((".cpp", ".cc")) else f"gpointer unused_{k}"
                                              for k in range(have, want))
                            edits.append((end, (", " if have else "") + extra if have else extra))
                if fix and edits:
                    for end, insert in sorted(edits, reverse=True):
                        if text[:end].rstrip().endswith("(") or text[:end].strip().endswith("(void"):
                            # no parameters yet: replace "void" if it is there
                            head = text[:end].rstrip()
                            if head.endswith("void"):
                                text = head[:-4] + insert.lstrip(", ") + text[end:]
                                continue
                        text = text[:end] + insert + text[end:]
                    open(path, "w", encoding="utf-8").write(text)
    print(f"{problems} found{' and fixed' if fix else ''}", file=sys.stderr)


if __name__ == "__main__":
    main()
