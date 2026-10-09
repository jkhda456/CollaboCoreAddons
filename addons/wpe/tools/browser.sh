#!/usr/bin/env bash
# The add-on's programs, linked against the WebKit build (tools/webkit.sh):
#
#   build/browser/wpe-browser.wasm   every process of the browser in one static program: the UI
#                                    process (browser/browser.c), WPEWebProcess, WPENetworkProcess
#                                    (by the name it is run as, browser/main.cpp)
#   build/browser/web.wasm           the `web` command (browser/web.c), no WebKit in it
#
#   The function names stay in wpe-browser (45 MB of 170: the engine's report of a crash names the
#   frames, which is what makes one fixable); WPE_WASM_KEEP_NAMES=0 leaves them out.
set -euo pipefail
A="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOT="$(cd "$A/../.." && pwd)"
B="$A/build"
P="$B/sysroot"
O="$B/webkit"
OUT="$B/browser"
U="$ROOT/userspace"
. "$A/tools/lib.sh"
export PATH="$B/host/bin:$PATH"
export WPE_WASM_KEEP_NAMES="${WPE_WASM_KEEP_NAMES-1}"
CC="$A/tools/wasm-wpe-cc"
CXX="$A/tools/wasm-wpe-c++"
mkdir -p "$OUT"

[[ -f "$O/lib/libWPEWebKit-2.0.a" ]] || { echo "no WebKit build in $O; run tools/webkit.sh" >&2; exit 1; }

# ── web: the command line, small and quick to start ─────────────────────────────────────────────
if [[ ! -f "$OUT/web.wasm" || "$A/browser/web.c" -nt "$OUT/web.wasm" ]]; then
  echo "== web"
  "$U/bin/wasm-cc" -O2 -Wall -Wextra -Wno-unused-parameter "$A/browser/web.c" -o "$OUT/web.wasm" \
    -Wl,--import-memory -Wl,--max-memory=4294967296 -Wl,--shared-memory -Wl,--export-table -Wl,-z,stack-size=1048576
fi

# ── wpe-browser ────────────────────────────────────────────────────────────────────────────────
H="$B/stage/usr/include/wpe-webkit-2.0"   # WebKit's API headers, installed there by tools/webkit.sh
INCLUDES=(
  -I"$OUT" -I"$H" -I"$H/wpe-platform"
  -I"$P/include" -I"$P/include/glib-2.0" -I"$P/lib/glib-2.0/include" -I"$P/include/libsoup-3.0"
)
python3 "$A/browser/gen-js.py" "$OUT/browser-js.h.new" "$A/browser/snapshot.js" "$A/browser/target.js"
cmp -s "$OUT/browser-js.h.new" "$OUT/browser-js.h" 2>/dev/null && rm "$OUT/browser-js.h.new" || mv "$OUT/browser-js.h.new" "$OUT/browser-js.h"

objects=()
compile() { # source object compiler flags...
  local src="$1" obj="$2" cc="$3"; shift 3
  if [[ ! -f "$obj" || "$src" -nt "$obj" || "$OUT/browser-js.h" -nt "$obj" ]]; then
    echo "  cc $(basename "$src")"
    "$cc" -O2 -g0 -msimd128 -Wall -Wno-unused-parameter "${INCLUDES[@]}" "$@" -c "$src" -o "$obj"
  fi
  objects+=("$obj")
}
echo "== wpe-browser"
compile "$A/browser/browser.c" "$OUT/browser.o" "$CC" -std=gnu11
compile "$A/browser/tls.c" "$OUT/tls.o" "$CC" -std=gnu11
compile "$A/browser/main.cpp" "$OUT/main.o" "$CXX" -std=c++20 -fno-exceptions -fno-rtti

# What CMake links WPEWebProcess with: the library and its dependencies, plus the WebKit objects
# outside it (lib.sh webkit_objects), bmalloc as an archive (pulled in only if something needs
# it), what the libraries depend on statically (pkg-config gave CMake shared-library lists) and
# glib-networking's OpenSSL backend.
mapfile -t webkit_objs < <(webkit_objects "$O")
[[ ${#webkit_objs[@]} -gt 1000 ]] || { echo "could not read WebKit's objects from $O/build.ninja" >&2; exit 1; }
mapfile -t cmake_libs < <(awk '/^build bin\/WPEWebProcess:/,/^$/' "$O/build.ninja" | grep -E "^ *LINK_LIBRARIES" \
  | sed 's/^ *LINK_LIBRARIES = //' | tr ' ' '\n' | grep -v '^$' | grep -v '^-l\(dl\|m\|pthread\|rt\)$' \
  | grep -v '\.o$')
mapfile -t bmalloc_objs < <(webkit_bmalloc_objects "$O")
rm -f "$OUT/libbmalloc.a.new"
(cd "$O" && llvm-ar-19 rcsT "$OUT/libbmalloc.a.new" "${bmalloc_objs[@]}")
cmp -s "$OUT/libbmalloc.a.new" "$OUT/libbmalloc.a" 2>/dev/null && rm "$OUT/libbmalloc.a.new" || mv "$OUT/libbmalloc.a.new" "$OUT/libbmalloc.a"
extra=(
  "$P/lib/gio/modules/libgioopenssl.a" "$P/lib/libssl.a" "$P/lib/libcrypto.a"
  "$P/lib/libsoup-3.0.a" "$P/lib/libnghttp2.a" "$P/lib/libpsl.a" "$P/lib/libbrotlidec.a" "$P/lib/libbrotlienc.a"
  "$P/lib/libbrotlicommon.a" "$P/lib/libsharpyuv.a" "$P/lib/libpcre2-8.a" "$P/lib/libffi.a" "$P/lib/libz.a"
)
link_rsp="$OUT/link.rsp"
{ printf '%s\n' "${objects[@]}"; printf "$O/%s\n" "${webkit_objs[@]}"
  printf "$O/%s\n" "${cmake_libs[@]}" | sed "s|^$O/\(/\)|\1|"; echo "$OUT/libbmalloc.a"; printf '%s\n' "${extra[@]}"; } > "$link_rsp.new"
cmp -s "$link_rsp.new" "$link_rsp" 2>/dev/null && rm "$link_rsp.new" || mv "$link_rsp.new" "$link_rsp"

mapfile -t inputs < "$link_rsp"
stale=0; [[ -f "$OUT/wpe-browser.wasm" ]] || stale=1
for f in "${inputs[@]}" "$link_rsp"; do [[ "$f" -nt "$OUT/wpe-browser.wasm" ]] && { stale=1; break; }; done
if [[ $stale == 1 ]]; then
  echo "  link (a few minutes: one program with all of WebKit)"
  LOG="$B/browser-link.log"; : > "$LOG"
  # 8 MiB of linear-memory stack for each process's main thread (JavaScript's own stack is apart)
  log_run "$LOG" env WASM_STACK_SIZE=8388608 "$CXX" -O2 -Wl,--gc-sections @"$link_rsp" -o "$OUT/wpe-browser.wasm.new"
  mv "$OUT/wpe-browser.wasm.new" "$OUT/wpe-browser.wasm"
fi
ls -l "$OUT/wpe-browser.wasm" "$OUT/web.wasm"
