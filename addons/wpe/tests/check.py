#!/usr/bin/env python3
"""Checks what tests/guest.sh left in OUT (NAME.out / .err / .code per step)."""
import os
import sys
import zlib

OUT = sys.argv[1]
failed = 0


def get(name, ext):
    try:
        return open(os.path.join(OUT, f"{name}.{ext}"), encoding="utf-8", errors="replace").read()
    except FileNotFoundError:
        return None


def check(name, ok_code=0, *, contains=(), absent=()):
    global failed
    code, out, err = get(name, "code"), get(name, "out") or "", get(name, "err") or ""
    problems = []
    if code is None:
        problems.append("did not run")
    elif int(code) != ok_code:
        problems.append(f"exit {code.strip()} (want {ok_code})")
    for text in contains:
        if text not in out + err:
            problems.append(f"no {text!r}")
    for text in absent:
        if text in out + err:
            problems.append(f"has {text!r}")
    if problems:
        failed += 1
        print(f"FAIL {name}: {'; '.join(problems)}\n     out: {out.strip()[:400]!r}\n     err: {err.strip()[:400]!r}")
    else:
        print(f"ok   {name}")


def png_ok(path):
    try:
        data = open(path, "rb").read()
    except FileNotFoundError:
        return "no file"
    if not data.startswith(b"\x89PNG"):
        return "not a PNG"
    # the page is not one color: look at the decompressed rows
    chunks, pos, idat = {}, 8, b""
    while pos < len(data):
        length = int.from_bytes(data[pos:pos + 4], "big")
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IDAT":
            idat += body
        elif kind == b"IHDR":
            chunks["w"] = int.from_bytes(body[0:4], "big")
        pos += 12 + length
    raw = zlib.decompress(idat)
    # sampled over the whole image (the top rows of a page are often all background)
    return None if len(set(raw[::97])) > 8 else "one color"


check("open", contains=("127.0.0.1:8000/index.html", "WPE 시험 페이지"))
check("info", contains=("url: http://127.0.0.1:8000/index.html", "loading: no"))
check("read", contains=("# 안녕하세요, WPE", "[1]", "Greet", "Second page", "input text name"))
check("eval", contains=("WPE 시험 페이지 (200000) 3",))
check("korean", contains=("안녕하세요, WPE",))
check("type", contains=("typed 9 characters",))
check("select", contains=("selected green",))
check("submit", contains=("clicked",))
check("result", contains=("hello Claude 한글 / g",))
check("counted", contains=("2",))
check("keyseen", contains=("Tab", "Shift", "Control", "a"))
check("links", contains=("Second page  http://127.0.0.1:8000/second.html",))
check("shot", contains=("page.png",))
problem = png_ok(os.path.join(OUT, "page.png"))
print(f"{'ok  ' if not problem else 'FAIL'} screenshot png{': ' + problem if problem else ''}")
failed += bool(problem)
check("guilist", contains=("web",))
check("follow", contains=("clicked", "second.html"))
check("second", contains=("/second.html Second",))
check("back", contains=("index.html",))
check("backed", contains=("/index.html",))
check("scroll", contains=("scrolled down 2",))
check("badhost", 1, contains=("failed to load",))
check("after", contains=("url:",))
check("close", contains=("closing",))
check("gone", contains=("no browser is running",))
print("all passed" if not failed else f"{failed} failed")
sys.exit(1 if failed else 0)
