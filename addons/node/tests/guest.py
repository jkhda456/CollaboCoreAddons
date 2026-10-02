"""Shared by the guest tests: the runtime, the add-on, the mock API, running a command in the guest."""
import json
import os
import shutil
import subprocess
import tempfile
import urllib.request

T = os.path.dirname(os.path.abspath(__file__))
A = os.path.dirname(T)
ROOT = os.path.dirname(os.path.dirname(A))
RT = os.environ.get("COLLABO_RUNTIME") or os.path.join(ROOT, "dist/runtime/collabo-core-linux-arm64")
# The engine: the runtime's, or another one (NODE_ENGINE, e.g. engine/target/release's).
ENGINE = os.environ.get("NODE_ENGINE") or os.path.join(RT, "bin/collabo-core-engine")
CPIO = os.environ.get("NODE_CPIO") or os.path.join(A, "out/node.cpio")
MOCK_PORT = int(os.environ.get("NODE_MOCK_PORT", "18094"))
# the host's loopback, seen from the guest (--allow-loopback)
MOCK_URL = f"http://192.0.2.1:{MOCK_PORT}"
CLAUDE_CODE_TGZ = os.environ.get("CLAUDE_CODE_TGZ") or os.path.join(A, "build/test/claude-code-2.1.112.tgz")

fails = []


def check(name, ok, detail=""):
    print(("PASS " if ok else "FAIL ") + name + ("" if ok else f"\n     {str(detail)[-2500:]}"), flush=True)
    if not ok:
        fails.append(name)


def addon_dir():
    """An --addon-dir with the built image and this addon.json (as build.py lays out out/)."""
    d = tempfile.mkdtemp(prefix="node-addon-")
    os.symlink(CPIO, os.path.join(d, "node.cpio"))
    shutil.copyfile(os.path.join(A, "addon.json"), os.path.join(d, "node.json"))
    return d


ADDONS = addon_dir()


def requests():
    return json.loads(urllib.request.urlopen(f"http://127.0.0.1:{MOCK_PORT}/__requests").read())


def engine_options(work, settings=None):
    """The engine's options for a guest with the add-on, `work` mounted at /work."""
    options = ["--kernel", f"{RT}/app/images/vmlinux.wasm", "--initramfs", f"{RT}/app/images/initramfs.cpio",
               "--addon-dir", ADDONS, "--addon", "node", "--allow-loopback", "--mount", f"{work}:/work"]
    # The tests talk to the mock only, never to Anthropic's services.
    for host in ("*.anthropic.com", "anthropic.com", "*.claude.ai", "claude.ai", "*.sentry.io", "*.statsig.com"):
        options += ["--deny", host]
    for key, value in (settings or {}).items():
        options += ["--addon-config", f"node:{key}={value}"]
    return options


def guest(work, command, settings=None, timeout=900):
    """Runs `command` (sh) in a fresh guest, in /work; returns (status, stdout, stderr)."""
    argv = [ENGINE, "exec", *engine_options(work, settings), "--cwd", "/work", "--", "/bin/sh", "-c", command]
    process = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
    # stderr less the kernel's boot messages
    err = process.stderr
    mark = err.find("init: starting /bin/sh")
    if mark >= 0:
        err = err[err.find("\n", mark) + 1:]
    return process.returncode, process.stdout, err
