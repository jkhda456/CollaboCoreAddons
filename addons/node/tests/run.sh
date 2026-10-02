#!/usr/bin/env bash
# Checks for the node add-on:
#
#   tests/run.sh          node_cases.js with the native build (build/host/bin/node, TEST=1 build.sh),
#                         then in the guest: test_guest.py (Node's APIs, settings, npm/npx, bash, rg,
#                         Claude Code -p and its tools) and test_guest_tty.py (Claude Code's terminal UI)
#   tests/run.sh native   the native checks only
#
# Claude Code is the official npm package @anthropic-ai/claude-code 2.1.112 (its last pure-JS
# release), downloaded from the npm registry into build/test (checked against the registry's
# sha512) and installed in the guest with npm; it talks to a scripted Anthropic API
# (mock_anthropic.py on 127.0.0.1:18094, the guest's 192.0.2.1:18094).
#
# The guest is the packaged runtime (dist/runtime/collabo-core-<platform>, COLLABO_RUNTIME) with
# out/node.cpio (NODE_CPIO); NODE_ENGINE picks another engine binary. npm needs an engine with
# addons/mod/0006 (deep guest stacks): until the runtime has it, give the one built with it, e.g.
#   NODE_ENGINE=engine/target/release/collabo-core-engine addons/node/tests/run.sh
set -euo pipefail
T="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
A="$(dirname "$T")"
ROOT="$(cd "$A/../.." && pwd)"

status=0
if [[ -x "$A/build/host/bin/node" ]]; then
  echo "== native: node_cases.js"
  tmp="$(mktemp -d)"
  "$A/build/host/bin/node" "$T/node_cases.js" "$tmp" || status=1
  rm -rf "$tmp"
else
  echo "== native: skipped (no build/host/bin/node; build with TEST=1 addons/node/build.sh)"
fi
[[ "${1:-}" == native ]] && exit $status

case "$(uname -s)-$(uname -m)" in
  Linux-x86_64) platform=linux-x64 ;; Linux-aarch64) platform=linux-arm64 ;;
  Darwin-arm64) platform=darwin-arm64 ;; Darwin-x86_64) platform=darwin-x64 ;;
esac
export COLLABO_RUNTIME="${COLLABO_RUNTIME:-$ROOT/dist/runtime/collabo-core-$platform}"
[[ -f "$COLLABO_RUNTIME/app/images/vmlinux.wasm" ]] || { echo "no runtime at $COLLABO_RUNTIME; run: ./build.sh engine runtime" >&2; exit 1; }
export NODE_CPIO="${NODE_CPIO:-$A/out/node.cpio}"
[[ -f "$NODE_CPIO" ]] || { echo "no $NODE_CPIO; run: addons/node/build.sh" >&2; exit 1; }

CC_VERSION=2.1.112
CC_SHA512=9FUgJ0EOvILyhIqxFKNVliebiUjL68dwpEW3eGSSe0vkVDJ1c5qMDNWc22gW3zkD7zRAqtfQPSGv0t4vMM2DPA==
export CLAUDE_CODE_TGZ="${CLAUDE_CODE_TGZ:-$A/build/test/claude-code-$CC_VERSION.tgz}"
if [[ ! -f "$CLAUDE_CODE_TGZ" ]]; then
  mkdir -p "$(dirname "$CLAUDE_CODE_TGZ")"
  curl -fsSL --retry 3 -o "$CLAUDE_CODE_TGZ.part" \
    "https://registry.npmjs.org/@anthropic-ai/claude-code/-/claude-code-$CC_VERSION.tgz"
  got="$(openssl dgst -sha512 -binary "$CLAUDE_CODE_TGZ.part" | base64 -w0)"
  [[ "$got" == "$CC_SHA512" ]] || { echo "claude-code-$CC_VERSION.tgz: sha512 mismatch" >&2; rm -f "$CLAUDE_CODE_TGZ.part"; exit 1; }
  mv "$CLAUDE_CODE_TGZ.part" "$CLAUDE_CODE_TGZ"
fi

export NODE_MOCK_PORT="${NODE_MOCK_PORT:-18094}"
log="$(mktemp)"
python3 "$T/mock_anthropic.py" "$NODE_MOCK_PORT" >"$log" 2>&1 &
mock=$!
trap 'kill $mock 2>/dev/null; rm -f "$log"' EXIT
sleep 0.5

echo "== in the guest: node, npm, Claude Code -p"
python3 "$T/test_guest.py" || status=1
echo "== in the guest: Claude Code's terminal UI"
python3 "$T/test_guest_tty.py" || status=1
exit $status
