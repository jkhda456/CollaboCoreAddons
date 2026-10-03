#!/usr/bin/env bash
# Writes patches/jdk/*.patch again from build/src/jdk (a git repository whose first commit is the
# pristine export): each patch is `git diff` of its files. A changed file outside these lists is
# reported; add it to one of them. New files under src and make are told to git first.
set -euo pipefail
A="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
JS="$A/build/src/jdk"
P="$A/patches/jdk"
CRYPTO="src/hotspot/cpu/zero/zeroIntrinsics_zero.cpp src/hotspot/cpu/zero/zeroInterpreter_zero.hpp
  src/hotspot/cpu/zero/globals_zero.hpp src/hotspot/share/interpreter/abstractInterpreter.hpp
  src/hotspot/share/interpreter/abstractInterpreter.cpp src/hotspot/share/interpreter/zero/zeroInterpreterGenerator.hpp
  src/hotspot/share/interpreter/zero/zeroInterpreterGenerator.cpp"
declare -A FILES=(
  [0001-build-wasm32-cpu]="make/autoconf/platform.m4"
  [0002-hotspot-zero-on-the-wasm32-guest]="src/hotspot/os src/hotspot/os_cpu src/hotspot/cpu src/hotspot/share/interpreter
    src/hotspot/share/logging/logOutputList.cpp
    $(for f in $CRYPTO; do echo ":(exclude)$f"; done)"
  [0003-hotspot-zero-frames-without-pc-are-not-empty]="src/hotspot/share/runtime/frame.hpp"
  [0004-launcher-one-static-program-for-java-and-the-tools]="src/java.base/share/native/launcher src/java.base/unix/native/libjli"
  [0005-libraries-ilp32-statx-and-procfs]="src/java.base/unix/native/libnio src/jdk.management"
  [0006-security-ec-no-p256-table-at-first-on-an-interpreter]="src/java.base/share/classes/sun/security/ec"
  [0007-jimage-read-in-bulk]="src/java.base/share/classes/jdk/internal/jimage"
  [0008-hotspot-zero-crypto-in-c]="$CRYPTO"
)
git -C "$JS" ls-files -z --others --exclude-standard -- src make | xargs -0 -r git -C "$JS" add -N --
covered=()
for name in "${!FILES[@]}"; do
  # shellcheck disable=SC2086
  git -C "$JS" -c core.abbrev=12 diff -- ${FILES[$name]} > "$P/$name.patch"
  # shellcheck disable=SC2086
  covered+=($(git -C "$JS" diff --name-only -- ${FILES[$name]}))
done
for f in $(git -C "$JS" diff --name-only); do
  [[ " ${covered[*]} " == *" $f "* ]] || echo "not in any patch: $f" >&2
done
ls -l "$P"
