#!/usr/bin/env bash
# Runs tests/bash-cases.sh with the host's bash and with build/bash/bash.wasm in the guest
# (dist/runtime's engine) and diffs the two outputs. Needs tools/build-bash.sh first.
set -euo pipefail
T="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
A="$(dirname "$T")"
ROOT="$(cd "$A/../.." && pwd)"
case "$(uname -s)-$(uname -m)" in
  Linux-x86_64) platform=linux-x64 ;; Linux-aarch64) platform=linux-arm64 ;;
  Darwin-arm64) platform=darwin-arm64 ;; Darwin-x86_64) platform=darwin-x64 ;;
esac
RT="${COLLABO_RUNTIME:-$ROOT/dist/runtime/collabo-core-$platform}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cp "$T/bash-cases.sh" "$work/"
python3 "$ROOT/userspace/mkinitramfs.py" --overlay -o "$work/bash.cpio" \
  --file usr/bin/bash="$A/build/bash/bash.wasm" --link bin/bash=../usr/bin/bash >/dev/null
(cd /tmp && bash "$work/bash-cases.sh" > "$work/host.out" 2>&1)
(cd "$RT" && bin/collabo-core-engine exec --kernel app/images/vmlinux.wasm \
   --initramfs app/images/initramfs.cpio --initramfs "$work/bash.cpio" \
   --mount "$work:/work" --cwd /work -- /bin/sh -c '/bin/bash /work/bash-cases.sh > /work/guest.out 2>&1' \
   > "$work/engine.log" 2>&1) || { tail -20 "$work/engine.log"; exit 1; }
if diff "$work/host.out" "$work/guest.out"; then
  echo "PASS: $(tail -1 "$work/guest.out" | sed 's/done //') cases, the guest's bash matches $(bash -c 'echo $BASH_VERSION') on the host"
else
  echo "FAIL: outputs differ"; exit 1
fi
