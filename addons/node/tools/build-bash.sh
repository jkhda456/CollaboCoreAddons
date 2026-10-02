#!/usr/bin/env bash
# Builds GNU bash for the guest (wasm32 Linux, musl) as build/bash/bash.wasm, from the tree
# tools/fetch-bash.sh prepares (run first, or let this run it). Mirrors
# third_party/distro/distro/bash/package.nix without Nix: bash's own readline, termcap from
# python/deps (ncurses' libtinfo), job control on, children through callback clone().
# One difference from the distro: the base image has no /dev/fd link (devtmpfs is mounted
# over /dev), so process substitution names /proc/self/fd/N (bash_cv_dev_fd=whacky).
#
#   tools/build-bash.sh            FORCE=1 to reconfigure from scratch
set -euo pipefail
A="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOT="$(cd "$A/../.." && pwd)"
U="$ROOT/userspace"
DEPS="$ROOT/python/deps"
SRC="$A/build/src/bash"
B="$A/build/bash"
CC="$U/bin/wasm-cc"
JOBS="${JOBS:-$(nproc)}"
LINK_FLAGS="-Wl,--import-memory -Wl,--max-memory=4294967296 -Wl,--shared-memory -Wl,--export-table -Wl,-z,stack-size=8388608"

[[ -f "$U/sysroot/lib/crt1.o" ]] || { echo "missing userspace/sysroot; run: ./build.sh userspace" >&2; exit 1; }
[[ -f "$DEPS/lib/libtinfo.a" ]] || { echo "missing python/deps (ncurses); build the python step first" >&2; exit 1; }
[[ -f "$SRC/configure" ]] || bash "$A/tools/fetch-bash.sh"

mkdir -p "$B"
LOG="$B/build.log"
: > "$LOG"
run() { "$@" >> "$LOG" 2>&1 || { tail -40 "$LOG"; echo "failed: $*  (log: $LOG)" >&2; exit 1; }; }

# out-of-tree build: the source tree stays as fetch-bash.sh left it
W="$B/obj"
if [[ -n "${FORCE:-}" || ! -f "$W/Makefile" ]]; then
  rm -rf "$W"; mkdir -p "$W"
  # the callback-clone helper and the loud fork() guard, linked through LOCAL_LIBS
  (cd "$W" && run env CONFIG_SITE="$ROOT/python/config.site" "$SRC/configure" \
     --host=wasm32-unknown-linux-musl --build="$(uname -m)-pc-linux-gnu" \
     CC="$CC" AR=llvm-ar-19 RANLIB=llvm-ranlib-19 \
     CC_FOR_BUILD=clang-19 CFLAGS_FOR_BUILD="-std=gnu17 -g" \
     CFLAGS="-g -O2 -mllvm -wasm-enable-sjlj" \
     CPPFLAGS="-I$DEPS/include" LDFLAGS="-L$DEPS/lib $LINK_FLAGS" \
     LOCAL_LIBS="./bash-clone.o ./fork-shim.o" \
     --prefix=/usr --enable-static-link --without-bash-malloc --disable-nls \
     --with-curses \
     bash_cv_termcap_lib=libtinfo \
     bash_cv_job_control_missing=present \
     bash_cv_sys_named_pipes=present \
     bash_cv_getcwd_malloc=yes \
     bash_cv_getenv_redef=yes \
     bash_cv_func_sigsetjmp=present \
     bash_cv_dev_fd=whacky \
     bash_cv_pgrp_pipe=no \
     bash_cv_wexitstatus_offset=8 \
     bash_cv_unusable_rtsigs=no \
     bash_cv_printf_a_format=yes \
     bash_cv_wcontinued_broken=no)
fi
run "$CC" -I"$W" -I"$SRC" -I"$SRC/include" -I"$SRC/lib" -g -O2 -mllvm -wasm-enable-sjlj \
  -c "$SRC/bash-clone.c" -o "$W/bash-clone.o"
run "$CC" -I"$W" -I"$SRC" -g -O2 -c "$SRC/fork-shim.c" -o "$W/fork-shim.o"
run make -C "$W" -j"$JOBS" bash
cp "$W/bash" "$B/bash.wasm"
ls -l "$B/bash.wasm"
