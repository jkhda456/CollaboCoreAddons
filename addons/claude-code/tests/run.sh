#!/usr/bin/env bash
# Checks for the claude-code add-on, against a scripted model (mock_llm.py):
#
#   tests/run.sh          unit tests; -p scenarios and the interactive UI (a pseudo-terminal)
#                         with a native build; then the same in the guest (dist/runtime)
#   tests/run.sh native   the native half only (no runtime needed)
#
# The model answers from a script each test loads; it listens on 127.0.0.1:18090 (the host
# request API's protocol, for a native claude: COLLABO_BRIDGE) and 18091 (HTTP, for the guest).
set -euo pipefail
T="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
A="$(dirname "$T")"
ROOT="$(cd "$A/../.." && pwd)"
export RUSTUP_HOME="$ROOT/.tools/rustup" CARGO_HOME="$ROOT/.tools/cargo"
export PATH="$CARGO_HOME/bin:$PATH"
HOST_TRIPLE="$(rustc -vV | sed -n 's/^host: //p')"
export "CARGO_TARGET_$(echo "$HOST_TRIPLE" | tr a-z- A-Z_)_LINKER=clang-19"

work="$(mktemp -d)"
export CLAUDE_TEST_LOG="$work/llm-requests.jsonl"
python3 "$T/mock_llm.py" 18090 >"$work/bridge.log" 2>&1 &
bridge=$!
python3 "$T/mock_llm.py" 18091 http >"$work/http.log" 2>&1 &
http=$!
trap 'kill $bridge $http 2>/dev/null; rm -rf "$work"' EXIT
sleep 0.5

echo "== unit tests"
cargo test --quiet --manifest-path "$A/agent/Cargo.toml" --target-dir "$A/build/host" 2>&1 | grep -E "^test result|FAILED|panicked"
echo "== native build"
cargo build --quiet --manifest-path "$A/agent/Cargo.toml" --target-dir "$A/build/host"
export CLAUDE_BIN="$A/build/host/debug/claude"
echo "== -p"
python3 "$T/test_print.py"
echo "== interactive (pty)"
python3 "$T/test_tty.py"

[[ "${1:-}" == native ]] && exit 0
case "$(uname -s)-$(uname -m)" in
  Linux-x86_64) platform=linux-x64 ;; Linux-aarch64) platform=linux-arm64 ;;
  Darwin-arm64) platform=darwin-arm64 ;; Darwin-x86_64) platform=darwin-x64 ;;
esac
export COLLABO_RUNTIME="$ROOT/dist/runtime/collabo-core-$platform"
[[ -f "$COLLABO_RUNTIME/app/images/addons/claude-code.cpio" ]] || { echo "no runtime with the add-on; run: ./build.sh addons engine runtime" >&2; exit 1; }
echo "== in the guest: -p"
python3 "$T/test_guest.py"
echo "== in the guest: interactive"
python3 "$T/test_guest_tty.py"
