#!/bin/sh
# Fetches wasm3 (MIT), the interpreter behind the WebAssembly JS API
# (src/b_wasm.c), into build/src/wasm3 at a pinned commit and applies the
# add-on's patches to it.
#
#   tools/fetch-wasm3.sh [ADDON_DIR]
set -eu

WASM3_URL=https://github.com/wasm3/wasm3
WASM3_COMMIT=28ecb9af6d2040e474a70f7cb7f43666740141fb   # 2026-09-29 "Implement Atomics"

A=$(cd "${1:-$(dirname "$0")/..}" && pwd)
D="$A/build/src/wasm3"

if [ ! -d "$D/.git" ]; then
  rm -rf "$D"
  mkdir -p "$D"
  git -C "$D" init -q
  git -C "$D" remote add origin "$WASM3_URL"
fi
if [ "$(git -C "$D" rev-parse HEAD 2>/dev/null)" != "$WASM3_COMMIT" ]; then
  git -C "$D" fetch -q --depth 1 origin "$WASM3_COMMIT"
  git -C "$D" checkout -q --force "$WASM3_COMMIT"
fi
git -C "$D" checkout -q -- source     # back to pristine before patching

for p in "$A"/build/patchwork/wasm3_*.py; do
  python3 "$p" "$D/source"
done
