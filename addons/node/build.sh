#!/usr/bin/env bash
# The node add-on: Node.js v24.21.0 for the guest — Node's own JavaScript library on the QuickJS-ng
# engine with libuv and C implementations of Node's internal bindings (src/) — plus npm/npx and
# GNU bash 5.3, packed as out/node.cpio (an overlay: /usr/bin/node, npm, npx, bash).
#
# Needs the userspace step (musl, compiler-rt: userspace/sysroot) and the python step (OpenSSL,
# zlib, ncurses for the guest: python/deps). Sources come from the clones beside the tree
# (../org/node, ../org/quickjs-ng, ../org/llvm-project) at pinned revisions; wasm3, bash and
# ripgrep are downloaded (tools/fetch-wasm3.sh, tools/fetch-bash.sh, tools/build-ripgrep.sh), and
# libc++ too when there is no llvm-project clone (the LLVM release's source archive).
#
#   addons/node/build.sh
#   FORCE=stage[,stage]    redo those stages: src wasm3 libcxx node bash ripgrep cpio host
#   TEST=1                 also build node natively (build/host/bin/node; needs a host OpenSSL,
#                          the `host` stage builds one) for tests/run.sh
#   JOBS=N
set -euo pipefail
A="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$A/../.." && pwd)"
U="$ROOT/userspace"
DEPS="$ROOT/python/deps"
ORG="$(cd "$ROOT/.." && pwd)/org"
B="$A/build"
S="$B/src"
JOBS="${JOBS:-$(nproc)}"
FORCE=",${FORCE:-},"

NODE_SRC="$ORG/node"
NODE_TAG=v24.21.0
LEXER_TAG=v24.13.1          # the last tag with deps/cjs-module-lexer (Node 24.21 has merve, C++)
QJS_SRC="$ORG/quickjs-ng"
QJS_REV=2f0aa72             # quickjs-ng 0.17.0 + "Port bellard/quickjs@10c81b1"
LLVM_SRC="$ORG/llvm-project"
RT_TAG=llvmorg-19.1.7       # libc++ for the guest: the release of the host's clang-19
# without the clone: the release's source archive
RT_TARBALL=https://github.com/llvm/llvm-project/releases/download/llvmorg-19.1.7/llvm-project-19.1.7.src.tar.xz
RT_TARBALL_SHA256=82401fea7b79d0078043f7598b835284d6650a75b93e64b6f761ea7b63097501

stage() { # name output...: true when the stage must run
  local name="$1"; shift
  if [[ "$FORCE" != *",$name,"* ]]; then
    local f ok=1
    for f in "$@"; do [[ -e "$f" ]] || ok=0; done
    if [[ $ok == 1 ]]; then echo "== $name: up to date"; return 1; fi
  fi
  echo "== $name"; return 0
}
log_run() { # log command...
  local log="$1"; shift
  "$@" >> "$log" 2>&1 || { tail -40 "$log"; echo "failed: $* (log: $log)" >&2; exit 1; }
}

[[ -f "$U/sysroot/lib/crt1.o" ]] || { echo "missing userspace/sysroot; run: ./build.sh userspace" >&2; exit 1; }
[[ -f "$DEPS/lib/libssl.a" && -f "$DEPS/lib/libz.a" ]] \
  || { echo "missing OpenSSL/zlib for the guest; build the python step first" >&2; exit 1; }
for d in "$NODE_SRC" "$QJS_SRC"; do
  [[ -d "$d/.git" ]] || { echo "missing the clone $d" >&2; exit 1; }
done
mkdir -p "$B" "$A/out"

# ── src: Node's library and native deps, QuickJS-ng, with patches/ applied ─────────────────────
# Each tree is a git repository whose first commit is the pristine export, so `git diff` there
# shows (and regenerates) the add-on's patches.
pristine() { # dir: commit what is there as "pristine"
  git -C "$1" init -q && git -C "$1" add -A && \
    git -C "$1" -c user.name=build -c user.email=build@localhost commit -q -m pristine
}
apply_patches() { # dir patch...
  local d="$1" p; shift
  for p in "$@"; do
    [[ -f "$p" ]] || continue
    echo "  patch $(basename "$p")"
    git -C "$d" apply --whitespace=nowarn "$p"
  done
}
if stage src "$S/node/.stamp" "$S/quickjs/.stamp" "$S/extra/cjs-module-lexer/lexer.js"; then
  rm -rf "$S/node" "$S/quickjs" "$S/extra"
  mkdir -p "$S/node" "$S/quickjs" "$S/extra/cjs-module-lexer"
  git -C "$NODE_SRC" archive "$NODE_TAG" LICENSE lib src typings \
      deps/acorn deps/ada deps/brotli deps/cares deps/histogram deps/llhttp deps/merve \
      deps/minimatch deps/nbytes deps/nghttp2 deps/npm deps/sqlite deps/undici deps/uv deps/zstd \
    | tar -xm -C "$S/node"
  pristine "$S/node"
  apply_patches "$S/node" "$A"/patches/node/*.patch
  touch "$S/node/.stamp"
  git -C "$QJS_SRC" archive "$QJS_REV" LICENSE cutils.h dtoa.c dtoa.h list.h \
      libregexp.c libregexp.h libregexp-opcode.h libunicode.c libunicode.h libunicode-table.h \
      quickjs.c quickjs.h quickjs-atom.h quickjs-opcode.h quickjs-c-atomics.h \
      builtin-array-fromasync.h builtin-iterator-zip.h builtin-iterator-zip-keyed.h \
    | tar -xm -C "$S/quickjs"
  pristine "$S/quickjs"
  apply_patches "$S/quickjs" "$A"/patches/quickjs/*.patch
  touch "$S/quickjs/.stamp"
  git -C "$NODE_SRC" show "$LEXER_TAG:deps/cjs-module-lexer/lexer.js" > "$S/extra/cjs-module-lexer/lexer.js"
  git -C "$NODE_SRC" show "$LEXER_TAG:deps/cjs-module-lexer/LICENSE" > "$S/extra/cjs-module-lexer/LICENSE"
fi

# ── wasm3: the WebAssembly interpreter behind the WebAssembly API ───────────────────────────────
if stage wasm3 "$S/wasm3/source/wasm3.h"; then
  sh "$A/tools/fetch-wasm3.sh" "$A"
  rm -rf "$B/wasm/wasm3"   # its objects have no header dependencies
fi

# ── libcxx: libc++ / libc++abi for wasm32 (ada, the WHATWG URL parser, is C++) ──────────────────
# As addons/llvm builds it: LLVM 19.1.7's runtimes, no exceptions, no unwinder.
LIBCXX="$B/libcxx-install"
if stage libcxx "$LIBCXX/lib/libc++.a" "$B/rt-src/libcxx/LICENSE.TXT"; then
  CMAKE="$ROOT/.tools/cmake/bin/cmake"
  NINJA="$(dirname "$CMAKE")/ninja"
  [[ -x "$NINJA" ]] || NINJA="$(find "$ROOT/.tools" -maxdepth 5 -name ninja -perm /111 2>/dev/null | head -1)"
  [[ -x "$CMAKE" && -n "$NINJA" ]] || { echo ".tools/cmake missing; run: ./build.sh tools" >&2; exit 1; }
  RT_SRC="$B/rt-src"
  RT_DIRS=(runtimes cmake llvm/cmake llvm/utils/llvm-lit libcxx libcxxabi)
  rm -rf "$RT_SRC" "$B/libcxx" "$LIBCXX"; mkdir -p "$RT_SRC"
  if [[ -d "$LLVM_SRC/.git" ]]; then
    git -C "$LLVM_SRC" archive "$RT_TAG" "${RT_DIRS[@]}" | tar -xm -C "$RT_SRC"
  else
    mkdir -p "$B/dl"
    TB="$B/dl/$(basename "$RT_TARBALL")"
    if [[ ! -f "$TB" ]]; then
      curl -fL --retry 3 -o "$TB.part" "$RT_TARBALL"
      mv "$TB.part" "$TB"
    fi
    echo "$RT_TARBALL_SHA256  $TB" | sha256sum -c --quiet || { echo "checksum mismatch: $TB" >&2; exit 1; }
    tar -xJmf "$TB" -C "$RT_SRC" --strip-components=1 \
      "${RT_DIRS[@]/#/llvm-project-19.1.7.src/}"
  fi
  WASM_FLAGS="--target=wasm32-unknown-unknown -matomics -mbulk-memory -mllvm -wasm-enable-sjlj -D__linux__=1 -D__linux=1 -D__unix__=1 -D__unix=1 -D__gnu_linux__=1 -Wno-unused-command-line-argument -Wno-override-module --sysroot=$U/sysroot"
  LOG="$B/libcxx.log"; : > "$LOG"
  log_run "$LOG" "$CMAKE" -DCMAKE_MAKE_PROGRAM="$NINJA" -S "$RT_SRC/runtimes" -B "$B/libcxx" -G Ninja \
    --log-level=WARNING -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$LIBCXX" \
    -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_C_COMPILER=/usr/bin/clang-19 -DCMAKE_CXX_COMPILER=/usr/bin/clang++-19 \
    -DCMAKE_AR=/usr/bin/llvm-ar-19 -DCMAKE_RANLIB=/usr/bin/llvm-ranlib-19 \
    -DCMAKE_C_COMPILER_TARGET=wasm32-unknown-unknown -DCMAKE_CXX_COMPILER_TARGET=wasm32-unknown-unknown \
    "-DCMAKE_C_FLAGS=$WASM_FLAGS" "-DCMAKE_CXX_FLAGS=$WASM_FLAGS" -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
    -DLLVM_ENABLE_RUNTIMES="libcxxabi;libcxx" -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_DOCS=OFF \
    -DLIBCXX_INCLUDE_BENCHMARKS=OFF -DLIBCXX_INCLUDE_TESTS=OFF -DLIBCXXABI_INCLUDE_TESTS=OFF \
    -DLIBCXXABI_ENABLE_SHARED=OFF -DLIBCXXABI_ENABLE_STATIC=ON -DLIBCXXABI_ENABLE_EXCEPTIONS=OFF \
    -DLIBCXXABI_USE_LLVM_UNWINDER=OFF -DLIBCXXABI_HAS_PTHREAD_API=ON -DLIBCXXABI_HAS_CXA_THREAD_ATEXIT_IMPL=OFF \
    -DLIBCXX_ENABLE_SHARED=OFF -DLIBCXX_ENABLE_STATIC=ON -DLIBCXX_ENABLE_EXCEPTIONS=OFF \
    -DLIBCXX_ENABLE_FILESYSTEM=ON -DLIBCXX_HAS_MUSL_LIBC=ON -DLIBCXX_CXX_ABI=libcxxabi \
    -DLIBCXX_ENABLE_STATIC_ABI_LIBRARY=ON
  log_run "$LOG" "$NINJA" -C "$B/libcxx" -j"$JOBS" install
fi

# ── node: node.wasm ──────────────────────────────────────────────────────────────────────────────
# make itself knows what changed; FORCE=node rebuilds everything.
NODE_WASM="$B/wasm/bin/node.wasm"
echo "== node"
[[ "$FORCE" == *",node,"* ]] && rm -rf "$B/wasm"
make -C "$A" TARGET=wasm LIBCXX="$LIBCXX" -j"$JOBS"

# ── bash: GNU bash 5.3 for the guest (Claude Code's Bash tool needs a bash) ─────────────────────
if stage bash "$B/bash/bash.wasm"; then
  FORCE="$([[ "$FORCE" == *",bash,"* ]] && echo 1 || true)" bash "$A/tools/build-bash.sh"
fi

# ── ripgrep: `rg` (Claude Code's Grep and Glob run it; node sets USE_BUILTIN_RIPGREP=0) ──────────
if stage ripgrep "$B/ripgrep/rg.wasm"; then
  FORCE="$([[ "$FORCE" == *",ripgrep,"* ]] && echo 1 || true)" bash "$A/tools/build-ripgrep.sh"
fi

# ── host: node built natively, for the tests (TEST=1) ───────────────────────────────────────────
if [[ -n "${TEST:-}" ]]; then
  HOSTSSL="$B/host-deps/openssl"
  if stage host-openssl "$HOSTSSL/lib/libcrypto.a"; then
    mkdir -p "$B/host-deps"; rm -rf "$B/host-deps/openssl-3.5.7"
    tar -xzf "$ROOT/python/src/openssl-3.5.7.tar.gz" -C "$B/host-deps"
    case "$(uname -m)" in aarch64) ostarget=linux-aarch64;; x86_64) ostarget=linux-x86_64;; *) ostarget=linux-generic64;; esac
    LOG="$B/host-deps/openssl.log"; : > "$LOG"
    (cd "$B/host-deps/openssl-3.5.7" && log_run "$LOG" env CC=clang-19 AR=llvm-ar-19 RANLIB=llvm-ranlib-19 \
       ./Configure "$ostarget" no-shared no-tests no-docs no-apps --prefix="$HOSTSSL" --libdir=lib -O2 \
     && log_run "$LOG" make -j"$JOBS" && log_run "$LOG" make install_sw)
  fi
  make -C "$A" TARGET=host -j"$JOBS"
fi

# ── cpio: the overlay ────────────────────────────────────────────────────────────────────────────
# /usr/bin/node, npm and npx (Node's bundled npm in /usr/lib/node_modules/npm, as Node installs
# it), bash (also as /bin/bash: Claude Code looks there), rg, and the licenses.
CPIO="$A/out/node.cpio"
if stage cpio "$CPIO" || [[ "$NODE_WASM" -nt "$CPIO" || "$B/bash/bash.wasm" -nt "$CPIO" \
                          || "$B/ripgrep/rg.wasm" -nt "$CPIO" ]]; then
  rm -rf "$A/out/licenses"; mkdir -p "$A/out/licenses"
  cp "$S/node/LICENSE" "$A/out/licenses/addon-node-node.txt"
  cp "$S/quickjs/LICENSE" "$A/out/licenses/addon-node-quickjs-ng.txt"
  cp "$S/wasm3/LICENSE" "$A/out/licenses/addon-node-wasm3.txt"
  cp "$S/bash/COPYING" "$A/out/licenses/addon-node-bash.txt"
  cp "$S/node/deps/npm/LICENSE" "$A/out/licenses/addon-node-npm.txt"
  cp "$S/node/deps/nghttp2/COPYING" "$A/out/licenses/addon-node-nghttp2.txt"
  cp "$S/ripgrep/LICENSE-MIT" "$A/out/licenses/addon-node-ripgrep.txt"
  cp "$B/ripgrep/crates.txt" "$A/out/licenses/addon-node-ripgrep-crates.txt"
  cp "$B/rt-src/libcxx/LICENSE.TXT" "$A/out/licenses/addon-node-libcxx.txt"
  python3 "$U/mkinitramfs.py" --overlay -o "$CPIO" \
    --file usr/bin/node="$NODE_WASM" \
    --tree usr/lib/node_modules/npm="$S/node/deps/npm" \
    --link usr/bin/npm=../lib/node_modules/npm/bin/npm-cli.js \
    --link usr/bin/npx=../lib/node_modules/npm/bin/npx-cli.js \
    --file usr/bin/bash="$B/bash/bash.wasm" \
    --link bin/bash=../usr/bin/bash \
    --file usr/bin/rg="$B/ripgrep/rg.wasm"
fi
ls -l "$CPIO" "$NODE_WASM"
