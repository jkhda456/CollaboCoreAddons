#!/usr/bin/env bash
# Tools the WebKit build runs on the build machine, which it may not have (and we cannot install
# system packages): meson, ruby (JavaScriptCore's offlineasm and generators), gperf (WebCore's
# hash tables). Built into build/host (bin/ on PATH for the rest of build.sh). glib's own tools
# (glib-mkenums, glib-compile-resources) come with the host glib that tools/deps.sh builds.
#
#   tools/host-tools.sh            (called by build.sh; each tool skipped when it is there)
set -euo pipefail
A="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
B="$A/build"
H="$B/host"
JOBS="${JOBS:-$(nproc)}"
. "$A/tools/lib.sh"

mkdir -p "$H/bin" "$H/src"

# ── meson: pure Python, run from its source ─────────────────────────────────────────────────────
if [[ ! -x "$H/bin/meson" ]]; then
  echo "== host: meson"
  f="$(fetch meson)"
  rm -rf "$H/src/meson-"*; tar -xzf "$f" -C "$H/src"
  d="$(ls -d "$H/src"/meson-*/ | head -1)"
  printf '#!/bin/sh\nexec python3 %s/meson.py "$@"\n' "${d%/}" > "$H/bin/meson"
  chmod +x "$H/bin/meson"
fi

# ── ninja: the one bootstrap-tools.sh put in .tools ─────────────────────────────────────────────
if [[ ! -x "$H/bin/ninja" ]]; then
  n="$(find "$A/../../.tools" -maxdepth 5 -name ninja -type f -perm /111 2>/dev/null | head -1)"
  [[ -n "$n" ]] || { echo "no ninja in .tools; run ./build.sh tools" >&2; exit 1; }
  ln -sf "$n" "$H/bin/ninja"
  c="$(find "$A/../../.tools" -maxdepth 4 -name cmake -type f -perm /111 2>/dev/null | head -1)"
  ln -sf "$c" "$H/bin/cmake"
fi

# ── gperf ──────────────────────────────────────────────────────────────────────────────────────
if [[ ! -x "$H/bin/gperf" ]]; then
  echo "== host: gperf"
  f="$(fetch gperf)"
  rm -rf "$H/src/gperf-"*; tar -xzf "$f" -C "$H/src"
  d="$(ls -d "$H/src"/gperf-*/ | head -1)"
  LOG="$B/host-gperf.log"; : > "$LOG"
  (cd "$d" && log_run "$LOG" env CC=clang-19 CXX=clang++-19 ./configure --prefix="$H" \
     && log_run "$LOG" make -j"$JOBS" && log_run "$LOG" make install)
fi

# ── unifdef: WebKit bundles it but would build it for the guest; the build runs it here ─────────
WK_SRC="${WK_SRC:-$(cd "$A/../../.." && pwd)/org/wpewebkit-2.54.1}"
if [[ ! -x "$H/bin/unifdef" ]]; then
  echo "== host: unifdef"
  clang-19 -O2 -w -I"$WK_SRC/Source/ThirdParty/unifdef" -o "$H/bin/unifdef" "$WK_SRC/Source/ThirdParty/unifdef/unifdef.c"
fi

# ── libyaml: for ruby's psych (WebKit's generators read YAML) ──────────────────────────────────
if [[ ! -f "$H/lib/libyaml.a" ]]; then
  echo "== host: libyaml"
  f="$(fetch libyaml)"
  rm -rf "$H/src/yaml-"*; tar -xzf "$f" -C "$H/src"
  d="$(ls -d "$H/src"/yaml-*/ | head -1)"
  LOG="$B/host-libyaml.log"; : > "$LOG"
  (cd "$d" && log_run "$LOG" env CC=clang-19 CFLAGS="-O2 -fPIC" ./configure --prefix="$H" --disable-shared \
     && log_run "$LOG" make -j"$JOBS" && log_run "$LOG" make install)
  rm -f "$H/bin/ruby"   # rebuilt with psych
fi

# ── ruby ───────────────────────────────────────────────────────────────────────────────────────
if [[ ! -x "$H/bin/ruby" ]]; then
  echo "== host: ruby"
  f="$(fetch ruby)"
  rm -rf "$H/src/ruby-"*; tar -xzf "$f" -C "$H/src"
  d="$(ls -d "$H/src"/ruby-*/ | head -1)"
  LOG="$B/host-ruby.log"; : > "$LOG"
  (cd "$d" && log_run "$LOG" env CC=clang-19 CXX=clang++-19 ./configure --prefix="$H" \
       --disable-install-doc --disable-install-rdoc --disable-shared --without-gmp \
       --with-out-ext=openssl,readline,fiddle,gdbm,dbm,win32,win32ole --with-libyaml-dir="$H" \
     && log_run "$LOG" make -j"$JOBS" && log_run "$LOG" make install)
fi
"$H/bin/ruby" -e 'require "digest/sha1"; require "pathname"; require "optparse"; require "yaml"' \
  || { echo "host ruby lacks what offlineasm needs" >&2; exit 1; }
echo "host tools: $(ls "$H/bin" | tr '\n' ' ')"
