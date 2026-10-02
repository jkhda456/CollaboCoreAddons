#!/usr/bin/env bash
# Fetches GNU bash 5.3 and its official patch series into build/src/dl, checked against the
# sha256s below, and unpacks a patched tree at build/src/bash (official patches, then the
# wasm Linux port from third_party/distro/distro/bash, then patches/bash/*.patch).
#
#   tools/fetch-bash.sh            (re)creates build/src/bash
#
# The port (tombl's distro) runs bash's child continuations through the kernel's callback
# clone() instead of fork(), which the guest does not have; see patches/bash/README.md.
set -euo pipefail
A="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOT="$(cd "$A/../.." && pwd)"
DL="$A/build/src/dl"
SRC="$A/build/src/bash"
DISTRO="$ROOT/third_party/distro/distro/bash"

BASH_VERSION_PIN=5.3
BASH_TARBALL_SHA256=0d5cd86965f869a26cf64f4b71be7b96f90a3ba8b3d74e27e8e9d9d5550f31ba
# bash53-NNN: the official patches the distro port was made against (001-009).
PATCH_SHA256=(
  1f608434364af86b9b45c8b0ea3fb3b165fb830d27697e6cdfc7ac17dee3287f
  e385548a00130765ec7938a56fbdca52447ab41fabc95a25f19ade527e282001
  f245d9c7dc3f5a20d84b53d249334747940936f09dc97e1dcb89fc3ab37d60ed
  9591d245045529f32f0812f94180b9d9ce9023f5a765c039b852e5dfc99747d0
  cca1ef52dbbf433bc98e33269b64b2c814028efe2538be1e2c9a377da90bc99d
  29119addefed8eff91ae37fd51822c31780ee30d4a28376e96002706c995ff10
  c0976bbfffa1453c7cfdd62058f206a318568ff2d690f5d4fa048793fa3eb299
  097cd723cbfb8907674ac32214063a3fd85282657ec5b4e544d2c0f719653fb4
  eee30fe78a4b0cb2fe20e010e00308899cfc613e0774ebb3c8557a1552f24f8c
)
# The distro's wasm patches, in its package.nix order.
DISTRO_PATCHES=(
  wasm-anonfile.patch
  wasm-callback-clone.patch
  wasm-execute-callbacks.patch
  wasm-fork-shim.patch
  wasm-main.patch
  wasm-simple-command-clone.patch
  wasm-disk-command-clone.patch
  wasm-process-substitution-clone.patch
  wasm-coproc-clone.patch
  wasm-null-command-clone.patch
)
GNU=https://ftp.gnu.org/gnu/bash

fetch() { # URL SHA256 FILE
  local url="$1" sum="$2" out="$DL/$3"
  if [[ ! -f "$out" ]] || ! echo "$sum  $out" | sha256sum -c --quiet - 2>/dev/null; then
    curl -fsSL -o "$out.part" "$url"
    mv "$out.part" "$out"
  fi
  echo "$sum  $out" | sha256sum -c --quiet - || { echo "checksum mismatch: $out" >&2; exit 1; }
}

mkdir -p "$DL"
fetch "$GNU/bash-$BASH_VERSION_PIN.tar.gz" "$BASH_TARBALL_SHA256" "bash-$BASH_VERSION_PIN.tar.gz"
for i in "${!PATCH_SHA256[@]}"; do
  n=$(printf '%03d' $((i + 1)))
  fetch "$GNU/bash-$BASH_VERSION_PIN-patches/bash53-$n" "${PATCH_SHA256[$i]}" "bash53-$n"
done

rm -rf "$SRC" "$SRC.tmp"
mkdir -p "$SRC.tmp"
tar -xzf "$DL/bash-$BASH_VERSION_PIN.tar.gz" -C "$SRC.tmp" --strip-components=1
cd "$SRC.tmp"
for i in "${!PATCH_SHA256[@]}"; do
  patch -s -p0 < "$DL/bash53-$(printf '%03d' $((i + 1)))"
done
for p in "${DISTRO_PATCHES[@]}"; do
  patch -s -p1 < "$DISTRO/$p"
done
cp "$DISTRO/bash-clone.c" "$DISTRO/bash-clone.h" "$DISTRO/fork-shim.c" .
for p in "$A"/patches/bash/*.patch; do
  [[ -e "$p" ]] || continue
  patch -s -p1 < "$p"
done
cd "$A"
mv "$SRC.tmp" "$SRC"
echo "bash $BASH_VERSION_PIN (patchlevel ${#PATCH_SHA256[@]}) at $SRC"
