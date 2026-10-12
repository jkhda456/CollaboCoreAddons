#!/usr/bin/env bash
# The nmap add-on: Nmap (the nmap.org development tree, 7.99x), with Nping and Ncat, for the guest —
# nmap, its scripting engine (NSE: Lua 5.4 and the scripts), its data files, libpcap 1.10.7, OpenSSL
# and libssh2 — packed as out/nmap.cpio (an overlay: /usr/bin/nmap, nping, ncat; /usr/share/nmap).
# README.md has the details.
#
# Needs the userspace step (musl, compiler-rt: userspace/sysroot), the python step (OpenSSL and
# zlib for the guest: python/deps), cmake and ninja (./build.sh tools), and, for the scans that
# send and receive raw packets, a kernel with packet sockets (CONFIG_PACKET: addons/mod/0013).
# The source is the clone ../org/nmap at a pinned commit, exported to build/src/nmap with
# patches/nmap/*.patch applied, and the tcpdump add-on's patches/libpcap/*.patch on its libpcap/;
# a git repository whose first commit is the pristine export, so `git diff` there shows (and
# regenerates) the add-on's patch.
#
#   addons/nmap/build.sh
#   FORCE=stage[,stage]    redo those stages: src libcxx configure nmap cpio
#   JOBS=N
set -euo pipefail
A="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$A/../.." && pwd)"
U="$ROOT/userspace"
ORG="$(cd "$ROOT/.." && pwd)/org"
DEPS="$ROOT/python/deps"
B="$A/build"
S="$B/src/nmap"
JOBS="${JOBS:-$(nproc)}"
FORCE=",${FORCE:-},"

NMAP_SRC="$ORG/nmap"
NMAP_REV=5cbb4288c95a8d004060b7003e90173c90876a80   # 7.99x (SVN) after 7.991: "Re-assign credit to a prior PR"
PCAP_PATCHES="$A/../tcpdump/patches/libpcap"         # libpcap without mmap(), shared with the tcpdump add-on
RT_TAG=llvmorg-19.1.7       # libc++ for the guest: the release of the host's clang-19
RT_TARBALL=https://github.com/llvm/llvm-project/releases/download/llvmorg-19.1.7/llvm-project-19.1.7.src.tar.xz
RT_TARBALL_SHA256=82401fea7b79d0078043f7598b835284d6650a75b93e64b6f761ea7b63097501

CC_W="$A/tools/wasm-nmap-cc"
CXX_W="$A/tools/wasm-nmap-c++"
CMAKE="$ROOT/.tools/cmake/bin/cmake"
NINJA="$ROOT/.tools/cmake/bin/ninja"

stage() { # name output...: true when the stage must run
  local name="$1"; shift
  if [[ "$FORCE" != *",$name,"* ]]; then
    local f ok=1
    for f in "$@"; do [[ -e "$f" ]] || ok=0; done
    if [[ $ok == 1 ]]; then echo "== $name: up to date"; return 1; fi
  fi
  echo "== $name"; return 0
}
redo() { FORCE="$FORCE$1,"; } # name: the stage must run (its inputs changed)
log_run() { # log command...
  local log="$1"; shift
  "$@" >> "$log" 2>&1 || { tail -40 "$log"; echo "failed: $* (log: $log)" >&2; exit 1; }
}
fetch() { # url sha256 file: download into build/dl (once) and check it
  local url="$1" sum="$2" f="$B/dl/$3"
  mkdir -p "$B/dl"
  if [[ ! -f "$f" ]]; then
    curl -fL --retry 3 -o "$f.part" "$url"
    mv "$f.part" "$f"
  fi
  echo "$sum  $f" | sha256sum -c --quiet || { echo "checksum mismatch: $f" >&2; exit 1; }
}

[[ -f "$U/sysroot/lib/crt1.o" ]] || { echo "missing userspace/sysroot; run: ./build.sh userspace" >&2; exit 1; }
[[ -f "$DEPS/lib/libssl.a" && -f "$DEPS/lib/libz.a" ]] \
  || { echo "missing OpenSSL/zlib for the guest; build the python step first (./build.sh python)" >&2; exit 1; }
[[ -x "$CMAKE" && -x "$NINJA" ]] || { echo "missing .tools/cmake; run: ./build.sh tools" >&2; exit 1; }
[[ -d "$NMAP_SRC/.git" ]] || { echo "missing the clone $NMAP_SRC" >&2; exit 1; }
chmod +x "$A"/tools/wasm-*
mkdir -p "$B" "$A/out"

# ── src: nmap at $NMAP_REV with patches/nmap, and the libpcap patches on its libpcap/ ────────────
# A patch newer than the export redoes it. Not exported: Zenmap (a desktop program) and the
# Windows and macOS packaging.
[[ -f "$S/.stamp" && -n "$(find "$A/patches" "$PCAP_PATCHES" -name '*.patch' -newer "$S/.stamp" -print -quit)" ]] && redo src
if stage src "$S/.stamp"; then
  rm -rf "$S"; mkdir -p "$S"
  git -C "$NMAP_SRC" archive "$NMAP_REV" -- . ':!zenmap' ':!mswin32' ':!macosx' | tar -xm -C "$S"
  git -C "$S" init -q && git -C "$S" add -A -f   # -f: nmap ignores files it carries (bpf_filter.c)
  git -C "$S" -c user.name=build -c user.email=build@localhost commit -q -m pristine
  for p in "$A"/patches/nmap/*.patch; do
    [[ -f "$p" ]] || continue
    echo "  patch $(basename "$p")"
    git -C "$S" apply --whitespace=nowarn "$p"
  done
  for p in "$PCAP_PATCHES"/*.patch; do
    echo "  patch libpcap/$(basename "$p")"
    git -C "$S" apply --whitespace=nowarn --directory=libpcap "$p"
  done
  # One time for every file: nmap's makefiles remake what is older than what it is made from
  # (ncat's configure from configure.ac with autoconf; ncat.h's and nping.h's version from nmap.h
  # and NMAP_VERSION, which nmap's release scripts set), and the export's order is no order.
  find "$S" -path "$S/.git" -prune -o -exec touch -h -d "@$(date +%s)" {} +
  touch "$S/.stamp"
  redo configure
fi

# ── libcxx: libc++ / libc++abi for wasm32 (nmap is C++) ─────────────────────────────────────────
# As addons/jdk and addons/node build it: LLVM 19.1.7's runtimes, no exceptions, no unwinder.
LIBCXX="$B/libcxx-install"
if stage libcxx "$LIBCXX/lib/libc++.a" "$B/rt-src/libcxx/LICENSE.TXT"; then
  RT_SRC="$B/rt-src"
  RT_DIRS=(runtimes cmake llvm/cmake llvm/utils/llvm-lit libcxx libcxxabi)
  rm -rf "$RT_SRC" "$B/libcxx" "$LIBCXX"; mkdir -p "$RT_SRC" "$B/dl"
  # the node and jdk add-ons download the same archive: take their copy when there is one
  TB="$B/dl/$(basename "$RT_TARBALL")"
  for other in node jdk llvm wpe; do
    [[ -f "$TB" ]] && break
    [[ -f "$A/../$other/build/dl/$(basename "$RT_TARBALL")" ]] \
      && { ln "$A/../$other/build/dl/$(basename "$RT_TARBALL")" "$TB" 2>/dev/null \
           || cp "$A/../$other/build/dl/$(basename "$RT_TARBALL")" "$TB"; }
  done
  fetch "$RT_TARBALL" "$RT_TARBALL_SHA256" "$(basename "$RT_TARBALL")"
  tar -xJmf "$TB" -C "$RT_SRC" --strip-components=1 "${RT_DIRS[@]/#/llvm-project-19.1.7.src/}"
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
  rm -rf "$B/libcxx"   # the objects; the install is what the build uses
fi

# ── configure: nmap's configure, which configures its libraries too ─────────────────────────────
# Static only (libtool would make shared libraries, which wasm-ld cannot); the libraries nmap
# carries, but OpenSSL and zlib from python/deps; no Zenmap, no Ndiff.
if stage configure "$S/Makefile" "$S/libpcap/Makefile"; then
  LOG="$B/configure.log"; : > "$LOG"
  (cd "$S" && log_run "$LOG" env CONFIG_SITE="$ROOT/python/config.site" ./configure \
     --host=wasm32-unknown-linux-musl --build="$(uname -m)-pc-linux-gnu" \
     CC="$CC_W" CXX="$CXX_W" AR=llvm-ar-19 RANLIB=llvm-ranlib-19 CFLAGS="-O2" CXXFLAGS="-O2" \
     --prefix=/usr --disable-shared --enable-static \
     --with-openssl="$DEPS" --with-libz="$DEPS" --with-libpcap=included --with-libpcre=included \
     --with-libssh2=included --with-liblua=included --with-liblinear=included --with-libdnet=included \
     --without-zenmap --without-ndiff)
  redo nmap
fi

# ── nmap: nmap, nping and ncat, installed into build/stage ──────────────────────────────────────
# make knows what changed, so it runs every time; the stage is redone when a program came out
# different (nmap's make links it again every time, the same bytes when nothing changed).
STAGE="$B/stage"
LOG="$B/make.log"; : > "$LOG"
echo "== make"
log_run "$LOG" make -C "$S" -j"$JOBS"
for prog in nmap nping/nping ncat/ncat; do
  cmp -s "$S/$prog" "$STAGE/usr/bin/$(basename "$prog")" || redo nmap
done
if stage nmap "$STAGE/usr/bin/nmap"; then
  rm -rf "$STAGE"
  log_run "$LOG" make -C "$S" install DESTDIR="$STAGE" STRIP=true
  redo cpio
fi

# ── cpio: the overlay ───────────────────────────────────────────────────────────────────────────
# nmap, nping and ncat in /usr/bin; nmap's data, scripts and NSE libraries in /usr/share/nmap,
# Ncat's CA bundle in /usr/share/ncat. Not the manual pages (the guest has no man).
CPIO="$A/out/nmap.cpio"
if stage cpio "$CPIO"; then
  L="$A/out/licenses"
  rm -rf "$L"; mkdir -p "$L"
  cp "$S/LICENSE" "$L/addon-nmap-nmap.txt"
  cp "$S/libpcap/LICENSE" "$L/addon-nmap-libpcap.txt"
  cp "$S/libdnet-stripped/LICENSE" "$L/addon-nmap-libdnet.txt"
  cp "$S/libpcre/LICENCE.md" "$L/addon-nmap-pcre2.txt"
  cp "$S/libssh2/COPYING" "$L/addon-nmap-libssh2.txt"
  cp "$S/liblinear/COPYRIGHT" "$L/addon-nmap-liblinear.txt"
  # Lua's license is the comment at the end of lua.h
  sed -n '/^\* Copyright (C) 1994-/,/^\*\*\*\*/{s/^\* \{0,1\}//;/^\*\*\*\*/d;p}' "$S/liblua/lua.h" > "$L/addon-nmap-lua.txt"
  cp "$B/rt-src/libcxx/LICENSE.TXT" "$L/addon-nmap-libcxx.txt"
  OPENSSL_LICENSE="$(ls "$ROOT"/python/build-deps/openssl-*/LICENSE.txt 2>/dev/null | head -1)"
  [[ -z "$OPENSSL_LICENSE" ]] || cp "$OPENSSL_LICENSE" "$L/addon-nmap-openssl.txt"
  python3 "$U/mkinitramfs.py" --overlay -o "$CPIO" --tree "usr/bin=$STAGE/usr/bin" \
    --tree "usr/share/nmap=$STAGE/usr/share/nmap" --tree "usr/share/ncat=$STAGE/usr/share/ncat"
fi
ls -l "$CPIO"
