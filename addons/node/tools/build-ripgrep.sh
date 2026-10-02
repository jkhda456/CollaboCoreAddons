#!/usr/bin/env bash
# Builds ripgrep (`rg`) for the guest (wasm32 Linux, musl) as build/ripgrep/rg.wasm. Claude Code's
# Grep and Glob tools run ripgrep, and the binaries it vendors are native (x64/arm64 ELF); with
# USE_BUILTIN_RIPGREP=0 it runs the `rg` on PATH, this one.
#
# Built the way addons/claude-code/build.sh builds Rust for the guest: the guest's Rust toolchain
# (python/build-rust-toolchain.sh), tombl's libc crate fork, linked through wasm-cc.
# patches/ripgrep/*.patch apply to the source (git apply).
#
#   tools/build-ripgrep.sh        FORCE=1 refetches and rebuilds from scratch
set -euo pipefail
A="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOT="$(cd "$A/../.." && pwd)"
U="$ROOT/userspace"
R="$ROOT/.tools/rust-wasm"
TARGET=wasm32-unknown-linux-musl
NIGHTLY=nightly-2026-07-22   # as python/build-rust-toolchain.sh
SRC="$A/build/src/ripgrep"
B="$A/build/ripgrep"

RG_URL=https://github.com/BurntSushi/ripgrep
RG_VERSION=15.2.0
RG_COMMIT=e89fff89ac9af12e8d4ce9d5fd07beb408ca730f   # tag 15.2.0

[[ -f "$U/sysroot/lib/crt1.o" ]] || { echo "missing userspace/sysroot; run: ./build.sh userspace" >&2; exit 1; }
bash "$ROOT/python/build-rust-toolchain.sh"

export RUSTUP_HOME="$ROOT/.tools/rustup" CARGO_HOME="$ROOT/.tools/cargo"
export PATH="$CARGO_HOME/bin:$PATH"
HOST_TRIPLE="$(rustc +"$NIGHTLY" -vV | sed -n 's/^host: //p')"
HOST_ENV="$(echo "$HOST_TRIPLE" | tr a-z- A-Z_)"
export "CARGO_TARGET_${HOST_ENV}_LINKER=clang-19" "CC_${HOST_TRIPLE//-/_}=clang-19"

# ── source: the release tag, patched ─────────────────────────────────────────────────────────────
if [[ -n "${FORCE:-}" || "$(git -C "$SRC" rev-parse HEAD 2>/dev/null)" != "$RG_COMMIT" ]]; then
  rm -rf "$SRC" "$B"
  mkdir -p "$SRC"
  git -C "$SRC" init -q
  git -C "$SRC" remote add origin "$RG_URL"
  git -C "$SRC" fetch -q --depth 1 origin "$RG_COMMIT"
  git -C "$SRC" checkout -q FETCH_HEAD
fi
git -C "$SRC" checkout -q -- .     # back to the release before patching
for p in "$A"/patches/ripgrep/*.patch; do
  [[ -f "$p" ]] || continue
  echo "  patch $(basename "$p")"
  git -C "$SRC" apply "$p"
done

mkdir -p "$B"
LOG="$B/build.log"
# rustc hands the linker --target=wasm32-unknown-linux-musl (the spec's name), which clang-19
# behind wasm-cc does not know; wasm-cc sets its own --target=wasm32.
cat > "$B/wasm-cc-rust" <<SH
#!/bin/sh
for a; do shift; case "\$a" in --target=*) ;; *) set -- "\$@" "\$a" ;; esac; done
exec "$U/bin/wasm-cc" "\$@"
SH
chmod +x "$B/wasm-cc-rust"
# The lock file may name a newer libc than the fork; resolve libc to the fork so the patch takes.
LIBC_VERSION="$(sed -n 's/^version = "\(.*\)"/\1/p' "$R/libc/Cargo.toml" | head -1)"
cargo +"$NIGHTLY" update --manifest-path "$SRC/Cargo.toml" --config "patch.crates-io.libc.path=\"$R/libc\"" \
  -p libc --precise "$LIBC_VERSION" > "$LOG" 2>&1 || true
# The link goes through wasm-cc with the toolchain's own wasm-ld (the objects are newer LLVM than
# clang-19's); std is the one build-rust-toolchain.sh built; libc is tombl's fork (ILP32 musl).
# The stack: 8 MiB, as the guest's other programs (regex compilation recurses).
echo "== rg (Rust -> $TARGET)"
WASM_LD="$R/bin/wasm-ld" RUSTC="$R/bin/rustc" \
RUSTFLAGS="-Clinker=$B/wasm-cc-rust -Clink-arg=-Wl,-z,stack-size=8388608 --remap-path-prefix=$SRC=/usr/src/ripgrep --remap-path-prefix=$CARGO_HOME=/usr/src/cargo" \
  cargo +"$NIGHTLY" build --release --target "$TARGET" \
    --manifest-path "$SRC/Cargo.toml" --target-dir "$B/target" \
    --config "patch.crates-io.libc.path=\"$R/libc\"" \
    --config profile.release.debug=0 --config profile.release.strip=true \
    >> "$LOG" 2>&1 || { tail -40 "$LOG"; echo "FAILED: rg (log: $LOG)" >&2; exit 1; }
OUT="$B/target/$TARGET/release/rg.wasm"
[[ -f "$OUT" ]] || OUT="$B/target/$TARGET/release/rg"
[[ -f "$OUT" ]] || { echo "cargo wrote no rg binary" >&2; exit 1; }
cp "$OUT" "$B/rg.wasm"

# The crates linked in, with their licenses (MIT / Unlicense / Apache-2.0).
cargo +"$NIGHTLY" metadata --format-version 1 --manifest-path "$SRC/Cargo.toml" \
    --config "patch.crates-io.libc.path=\"$R/libc\"" --filter-platform "$HOST_TRIPLE" 2>/dev/null \
  | python3 -c '
import json, sys
meta = json.load(sys.stdin)
used = {n["id"] for n in meta["resolve"]["nodes"]}
for p in sorted(meta["packages"], key=lambda p: p["name"]):
    if p["id"] in used:
        print(p["name"], p["version"] + ":", p.get("license") or "see crate", p.get("repository") or "")
' > "$B/crates.txt" || true
ls -l "$B/rg.wasm" | awk '{print "  " $NF, $5}'
