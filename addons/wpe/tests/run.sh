#!/usr/bin/env bash
# The wpe add-on's checks: boots the runtime with the add-on (out/) and the tools image (the
# display), runs tests/guest.sh against pages served in the guest (tests/site), checks the results.
#
#   tests/run.sh [RUNTIME_DIR]     (default: dist/runtime/collabo-core-<platform>)
set -euo pipefail
T="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
A="$(dirname "$T")"
ROOT="$(cd "$A/../.." && pwd)"
case "$(uname -s)-$(uname -m)" in
  Linux-x86_64) platform=linux-x64 ;; Linux-aarch64) platform=linux-arm64 ;;
  Darwin-arm64) platform=darwin-arm64 ;; Darwin-x86_64) platform=darwin-x64 ;;
esac
RT="${1:-$ROOT/dist/runtime/collabo-core-$platform}"
[[ -f "$A/out/wpe.cpio" ]] || { echo "build the add-on first: addons/wpe/build.sh" >&2; exit 1; }
cp "$A/addon.json" "$A/out/wpe.json"
chmod +x "$T/guest.sh"
cd "$RT"
time timeout 1500 bin/collabo-core-engine exec --kernel app/images/vmlinux.wasm \
  --initramfs app/images/initramfs.cpio --initramfs app/images/python.cpio --tools-image app/images/tools.cpio \
  --addon-dir "$A/out" --addon wpe --mount "$T:/t" -- /bin/sh /t/guest.sh > "$T/engine.log" 2>&1 || true
python3 "$T/check.py" "$T/out"
