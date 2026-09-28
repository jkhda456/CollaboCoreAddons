#!/usr/bin/env bash
# The claude-code add-on: `claude`, a coding agent for the guest, as a wasm32 Linux program
# (Rust, agent/), packed as out/claude-code.cpio (an overlay: /usr/bin/claude).
#
# Needs the userspace step (musl, compiler-rt: userspace/sysroot) and the guest's Rust toolchain
# (python/build-rust-toolchain.sh, which this runs; it skips when it is up to date).
#
#   addons/claude-code/build.sh
#   TEST=1 addons/claude-code/build.sh     also run the agent's unit tests natively first
set -euo pipefail

A="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$A/../.." && pwd)"
U="$ROOT/userspace"
R="$ROOT/.tools/rust-wasm"
TARGET=wasm32-unknown-linux-musl
NIGHTLY=nightly-2026-07-22   # as python/build-rust-toolchain.sh
B="$A/build"

[[ -f "$U/sysroot/lib/crt1.o" ]] || { echo "missing userspace/sysroot; run: ./build.sh userspace" >&2; exit 1; }
bash "$ROOT/python/build-rust-toolchain.sh"

export RUSTUP_HOME="$ROOT/.tools/rustup" CARGO_HOME="$ROOT/.tools/cargo"
export PATH="$CARGO_HOME/bin:$PATH"
# Build scripts are host programs, and this machine's C compiler is clang-19 (no `cc`).
HOST_TRIPLE="$(rustc +"$NIGHTLY" -vV | sed -n 's/^host: //p')"
HOST_ENV="$(echo "$HOST_TRIPLE" | tr a-z- A-Z_)"
export "CARGO_TARGET_${HOST_ENV}_LINKER=clang-19" "CC_${HOST_TRIPLE//-/_}=clang-19"

if [[ -n "${TEST:-}" ]]; then
  echo "== unit tests ($HOST_TRIPLE)"
  cargo +"$NIGHTLY" test --manifest-path "$A/agent/Cargo.toml" --target-dir "$B/host" --locked
fi

echo "== claude (Rust -> $TARGET)"
mkdir -p "$B" "$A/out"
# Built from a copy: the libc patch below replaces a crates.io package in the lock file, which
# must not rewrite the checked-in one. Every other version stays as agent/Cargo.lock has it.
rm -rf "$B/src"; mkdir -p "$B/src"
cp -r "$A/agent/Cargo.toml" "$A/agent/Cargo.lock" "$A/agent/src" "$B/src/"
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
cargo +"$NIGHTLY" update --manifest-path "$B/src/Cargo.toml" --config "patch.crates-io.libc.path=\"$R/libc\"" \
  -p libc --precise "$LIBC_VERSION" >> "$B/claude.log" 2>&1 || true
# The link goes through wasm-cc with the toolchain's own wasm-ld (the objects are newer LLVM than
# clang-19's), and std is the one build-rust-toolchain.sh built. The libc crate is tombl's fork:
# this platform's ILP32 musl ABI.
WASM_LD="$R/bin/wasm-ld" RUSTC="$R/bin/rustc" \
RUSTFLAGS="-Clinker=$B/wasm-cc-rust --remap-path-prefix=$B/src=/usr/src/claude --remap-path-prefix=$CARGO_HOME=/usr/src/cargo" \
  cargo +"$NIGHTLY" build --release --target "$TARGET" \
    --manifest-path "$B/src/Cargo.toml" --target-dir "$B/wasm" \
    --config "patch.crates-io.libc.path=\"$R/libc\"" \
    > "$B/claude.log" 2>&1 || { tail -40 "$B/claude.log"; echo "FAILED: claude (log: $B/claude.log)" >&2; exit 1; }
WASM="$B/wasm/$TARGET/release/claude.wasm"
[[ -f "$WASM" ]] || WASM="$B/wasm/$TARGET/release/claude"
[[ -f "$WASM" ]] || { echo "cargo wrote no claude binary" >&2; exit 1; }

echo "== cpio"
python3 "$U/mkinitramfs.py" --overlay -o "$A/out/claude-code.cpio" --file usr/bin/claude="$WASM"

# The crates linked in, with their licenses (all MIT or Apache-2.0); Rust's std is listed by
# scripts/build-engine.sh with python.cpio's.
mkdir -p "$A/out/licenses"
cargo +"$NIGHTLY" metadata --format-version 1 --offline --manifest-path "$B/src/Cargo.toml" \
    --config "patch.crates-io.libc.path=\"$R/libc\"" \
    --filter-platform "$HOST_TRIPLE" 2>/dev/null \
  | python3 -c '
import json, sys
meta = json.load(sys.stdin)
used = {n["id"] for n in meta["resolve"]["nodes"]}
for p in sorted(meta["packages"], key=lambda p: p["name"]):
    if p["id"] in used and p["name"] != "collabo-claude":
        print(p["name"], p["version"] + ":", p.get("license") or "see crate", p.get("repository") or "")
' > "$A/out/licenses/crates.txt"
ls -l "$A/out/claude-code.cpio" | awk '{print "  " $NF, $5}'
