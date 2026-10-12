#!/usr/bin/env bash
# The tcpdump add-on: tcpdump (the tcpdump.org development tree, 5.0.0-PRE) and libpcap 1.10.7 for
# the guest, packed as out/tcpdump.cpio (an overlay: /usr/bin/tcpdump). README.md has the details.
#
# Needs the userspace step (musl, compiler-rt: userspace/sysroot), cmake and ninja (./build.sh
# tools), and, to capture at all, a kernel with packet sockets (CONFIG_PACKET: addons/mod/0013).
# The sources are the clones ../org/tcpdump and ../org/nmap (whose libpcap/ is libpcap 1.10.7 as
# tcpdump.org released it, with only its build trimmed), at pinned commits, exported to build/src
# with patches/<name>/*.patch applied; each is a git repository whose first commit is the pristine
# export, so `git diff` there shows (and regenerates) the add-on's patch.
#
#   addons/tcpdump/build.sh
#   FORCE=stage[,stage]    redo those stages: src libpcap tcpdump cpio
#   JOBS=N
set -euo pipefail
A="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$A/../.." && pwd)"
U="$ROOT/userspace"
ORG="$(cd "$ROOT/.." && pwd)/org"
B="$A/build"
S="$B/src"
JOBS="${JOBS:-$(nproc)}"
FORCE=",${FORCE:-},"

TCPDUMP_SRC="$ORG/tcpdump"
TCPDUMP_REV=57768afc4cffbaa17faad7808f106bc325ada417   # master after 4.99.6: "no-capture and no-inject device flags"
NMAP_SRC="$ORG/nmap"
NMAP_REV=5cbb4288c95a8d004060b7003e90173c90876a80      # its libpcap/: 1.10.7

CC="$U/bin/wasm-cc"
CMAKE="$ROOT/.tools/cmake/bin/cmake"
NINJA="$ROOT/.tools/cmake/bin/ninja"
LINK_FLAGS="-Wl,--import-memory -Wl,--max-memory=4294967296 -Wl,--shared-memory -Wl,--export-table -Wl,-z,stack-size=8388608"
PCAP="$B/pcap"   # libpcap installed for the builds that link it (headers, libpcap.a, libpcap.pc)

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
export_src() { # clone tree-ish dest patches-dir: a pristine export as a git repository, patched
  local clone="$1" rev="$2" dest="$3" patches="$4"
  rm -rf "$dest"; mkdir -p "$dest"
  git -C "$clone" archive "$rev" | tar -xm -C "$dest"
  git -C "$dest" init -q && git -C "$dest" add -A -f
  git -C "$dest" -c user.name=build -c user.email=build@localhost commit -q -m pristine
  local p
  for p in "$patches"/*.patch; do
    [[ -f "$p" ]] || continue
    echo "  patch $(basename "$p")"
    git -C "$dest" apply --whitespace=nowarn "$p"
  done
  # one time for every file, so that no generated file (configure) looks older than its source
  find "$dest" -path "$dest/.git" -prune -o -exec touch -h -d "@$(date +%s)" {} +
}

[[ -f "$U/sysroot/lib/crt1.o" ]] || { echo "missing userspace/sysroot; run: ./build.sh userspace" >&2; exit 1; }
[[ -x "$CMAKE" && -x "$NINJA" ]] || { echo "missing .tools/cmake; run: ./build.sh tools" >&2; exit 1; }
for clone in "$TCPDUMP_SRC" "$NMAP_SRC"; do
  [[ -d "$clone/.git" ]] || { echo "missing the clone $clone" >&2; exit 1; }
done
mkdir -p "$B" "$A/out"

# ── src: tcpdump, and libpcap from nmap's tree, with patches/ applied ───────────────────────────
# A patch newer than the export redoes it.
[[ -f "$S/.stamp" && -n "$(find "$A/patches" -name '*.patch' -newer "$S/.stamp" -print -quit)" ]] && redo src
if stage src "$S/.stamp"; then
  rm -rf "$S"; mkdir -p "$S"
  export_src "$NMAP_SRC" "$NMAP_REV:libpcap" "$S/libpcap" "$A/patches/libpcap"
  export_src "$TCPDUMP_SRC" "$TCPDUMP_REV" "$S/tcpdump" "$A/patches/tcpdump"
  touch "$S/.stamp"
  redo libpcap
fi

# ── libpcap: libpcap.a, installed into build/pcap ───────────────────────────────────────────────
# Built in its tree: nmap's copy carries the generated grammar and scanner there.
LP="$S/libpcap"
if stage libpcap "$PCAP/lib/libpcap.a" "$PCAP/lib/pkgconfig/libpcap.pc"; then
  LOG="$B/libpcap.log"; : > "$LOG"
  rm -rf "$PCAP"
  # PF_PACKET only: no USB, Bluetooth, D-Bus or RDMA capture, no remote capture (rpcap)
  (cd "$LP" && log_run "$LOG" env CONFIG_SITE="$ROOT/python/config.site" ./configure \
     --host=wasm32-unknown-linux-musl --build="$(uname -m)-pc-linux-gnu" \
     CC="$CC" AR=llvm-ar-19 RANLIB=llvm-ranlib-19 CFLAGS="-O2" --prefix="$PCAP" \
     --disable-shared --with-pcap=linux --without-libnl --disable-usb --disable-bluetooth \
     --disable-dbus --disable-rdma --disable-netmap --disable-remote)
  log_run "$LOG" make -C "$LP" -j"$JOBS" libpcap.a
  log_run "$LOG" make -C "$LP" install
  grep -q -- "-lpcap" "$PCAP/lib/pkgconfig/libpcap.pc" || { echo "libpcap.pc without -lpcap" >&2; exit 1; }
  redo tcpdump
fi

# ── tcpdump: one static program ─────────────────────────────────────────────────────────────────
# Without libcrypto (no ESP decryption, -E) and libsmi; no fork(), so no -z (a command run on each
# finished savefile).
TB="$B/tcpdump-build"
if stage tcpdump "$TB/tcpdump"; then
  LOG="$B/tcpdump.log"; : > "$LOG"
  rm -rf "$TB"
  log_run "$LOG" env PKG_CONFIG_LIBDIR="$PCAP/lib/pkgconfig" "$CMAKE" -S "$S/tcpdump" -B "$TB" -G Ninja -Wno-dev \
    -DCMAKE_MAKE_PROGRAM="$NINJA" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=wasm32 -DCMAKE_C_COMPILER="$CC" \
    -DCMAKE_AR=/usr/bin/llvm-ar-19 -DCMAKE_RANLIB=/usr/bin/llvm-ranlib-19 -DCMAKE_C_FLAGS="-O2" \
    -DCMAKE_EXE_LINKER_FLAGS="$LINK_FLAGS" -DCMAKE_SKIP_RPATH=ON -DCMAKE_INSTALL_PREFIX=/usr \
    -DWITH_CRYPTO=OFF -DWITH_SMI=OFF -DWITH_CAP_NG=OFF
  log_run "$LOG" "$NINJA" -C "$TB" -j"$JOBS" tcpdump
  redo cpio
fi

# ── cpio: the overlay ───────────────────────────────────────────────────────────────────────────
CPIO="$A/out/tcpdump.cpio"
if stage cpio "$CPIO"; then
  rm -rf "$A/out/licenses"; mkdir -p "$A/out/licenses"
  cp "$S/tcpdump/LICENSE" "$A/out/licenses/addon-tcpdump-tcpdump.txt"
  cp "$LP/LICENSE" "$A/out/licenses/addon-tcpdump-libpcap.txt"
  python3 "$U/mkinitramfs.py" --overlay -o "$CPIO" --file "usr/bin/tcpdump=$TB/tcpdump"
fi
ls -l "$CPIO"
