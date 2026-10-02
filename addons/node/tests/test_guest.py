"""node in the guest: Node's APIs (node_cases.js), the add-on's settings, npm and npx with a local
package, bash and rg, and the official Claude Code (installed with npm from its npm tarball) against
the mock Anthropic API: -p answers and its Bash, Write, Read, Edit, Grep and Glob tools."""
import json
import os
import re
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from guest import CLAUDE_CODE_TGZ, MOCK_URL, T, check, fails, guest, requests  # noqa: E402

work = tempfile.mkdtemp(prefix="node-guest-")
shutil.copy(os.path.join(T, "node_cases.js"), work)
KEY = "sk-ant-node-test-0123"
ENV = {"DISABLE_TELEMETRY": "1", "DISABLE_AUTOUPDATER": "1", "DISABLE_ERROR_REPORTING": "1",
       "CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC": "1", "NODE_TEST_SETTING": "from-the-app"}
SETTINGS = {"baseUrl": MOCK_URL, "apiKey": KEY, "env": json.dumps(ENV)}


def sections(out):
    """The output of a guest script that prints '=== NAME' before each part: NAME -> text."""
    parts, name = {}, None
    for line in out.splitlines():
        m = re.match(r"^=== (\S+)$", line)
        if m:
            name = m.group(1)
            parts[name] = ""
        elif name:
            parts[name] += line + "\n"
    return parts


# ---- node itself, its settings, bash, rg ----
status, out, err = guest(work, """
echo "=== version"; node --version
echo "=== arch"; node -p process.arch
echo "=== settings"; node -p 'JSON.stringify([process.env.ANTHROPIC_BASE_URL, process.env.ANTHROPIC_API_KEY, process.env.NODE_TEST_SETTING, process.env.USE_BUILTIN_RIPGREP])'
echo "=== bash"; bash -c 'echo "bash $BASH_VERSION"'
echo "=== rg"; printf 'alpha\\nbeta\\n' > /tmp/x.txt; rg -n beta /tmp/x.txt
echo "=== cases"; node node_cases.js /tmp; echo "status $?"
""", settings=SETTINGS)
s = sections(out)
check("node --version", s.get("version", "").strip() == "v24.21.0", out + err)
check("process.arch is x64 (what programs check for)", s.get("arch", "").strip() == "x64", out + err)
check("the add-on's settings: baseUrl, apiKey (plain http: in the guest), env; USE_BUILTIN_RIPGREP=0",
      json.loads(s.get("settings", "null") or "null") == [MOCK_URL, KEY, "from-the-app", "0"], out + err)
check("bash", s.get("bash", "").startswith("bash 5.3"), out + err)
check("rg", s.get("rg", "").strip() == "2:beta", out + err)
check("Node's APIs in the guest (node_cases.js)", "status 0" in s.get("cases", ""), s.get("cases", "") + err)
for line in s.get("cases", "").splitlines():
    if line.startswith("FAIL"):
        print("     " + line)

# ---- npm and npx, offline, with a local package ----
pkg = os.path.join(work, "pkg")
os.makedirs(pkg)
with open(os.path.join(pkg, "package.json"), "w") as f:
    json.dump({"name": "hello-pkg", "version": "1.0.0", "bin": {"hello-pkg": "cli.js"}}, f)
with open(os.path.join(pkg, "cli.js"), "w") as f:
    f.write("#!/usr/bin/env node\nconsole.log('hello from hello-pkg', require('./package.json').version);\n")
status, out, err = guest(work, """
export npm_config_offline=true npm_config_audit=false npm_config_fund=false npm_config_update_notifier=false
echo "=== npm"; npm --version
echo "=== pack"; cd /work/pkg && npm pack 2>&1 | tail -1
echo "=== install"; mkdir -p /work/app && cd /work/app && npm init -y >/dev/null && npm install ../pkg/hello-pkg-1.0.0.tgz 2>&1 | tail -2; echo "status $?"
echo "=== npx"; npx hello-pkg
echo "=== global"; npm install -g /work/pkg/hello-pkg-1.0.0.tgz >/dev/null 2>&1; hello-pkg
""", timeout=1200)
s = sections(out)
check("npm --version", re.match(r"^\d+\.\d+\.\d+$", s.get("npm", "").strip()) is not None, out + err)
check("npm pack", "hello-pkg-1.0.0.tgz" in s.get("pack", ""), out + err)
check("npm install (a local tarball)", os.path.exists(os.path.join(work, "app/node_modules/hello-pkg/cli.js")), out + err)
check("npx runs the package's bin", s.get("npx", "").strip() == "hello from hello-pkg 1.0.0", out + err)
check("npm install -g puts the bin on PATH", s.get("global", "").strip() == "hello from hello-pkg 1.0.0", out + err)

# ---- the official Claude Code ----
if not os.path.exists(CLAUDE_CODE_TGZ):
    check("Claude Code tarball (tests/run.sh downloads it)", False, CLAUDE_CODE_TGZ)
else:
    shutil.copy(CLAUDE_CODE_TGZ, os.path.join(work, "claude-code.tgz"))
    proj = os.path.join(work, "proj")
    os.makedirs(os.path.join(proj, "sub"))
    with open(os.path.join(proj, "sub/notes.md"), "w") as f:
        f.write("# notes\nthe word gamma is here\n")
    script = """
export npm_config_offline=true npm_config_audit=false npm_config_fund=false npm_config_update_notifier=false
export HOME=/work/home; mkdir -p $HOME
echo "=== install"; npm install -g --omit=optional /work/claude-code.tgz 2>&1 | tail -2; echo "status $?"
echo "=== version"; claude --version
cd /work/proj
p() { name=$1; shift; echo "=== $name"; claude -p "$@" < /dev/null 2>&1; }
p hello 'say hi'
p bash 'run echo $((6*7)) && uname -s' --allowedTools Bash
p write 'tool Write {"file_path":"/work/proj/note.txt","content":"written by claude\\n"}' --allowedTools Write
p read 'tool Read {"file_path":"/work/proj/note.txt"}' --allowedTools Read
p edit 'tools [["Read", {"file_path":"/work/proj/note.txt"}], ["Edit", {"file_path":"/work/proj/note.txt","old_string":"written","new_string":"edited"}]]' --allowedTools Read,Edit
p grep 'tool Grep {"pattern":"gamma","output_mode":"content","-n":true}' --allowedTools Grep
p glob 'tool Glob {"pattern":"**/*.md"}' --allowedTools Glob
"""
    status, out, err = guest(work, script, settings=SETTINGS, timeout=1800)
    s = sections(out)
    check("npm install -g of Claude Code's tarball", "status 0" in s.get("install", ""), out + err)
    check("claude --version", "2.1.112 (Claude Code)" in s.get("version", ""), out + err)
    check("claude -p: an answer from the API", "Hello from the mock model! You said: say hi" in s.get("hello", ""),
          s.get("hello", "") + err)
    sent = requests()
    keys = {r.get("x-api-key") for r in sent if r["path"].startswith("/v1/messages")}
    check("the API key from the add-on's settings went with the requests", keys == {KEY}, keys)
    check("Bash tool (GNU bash in the guest)", '42\\nLinux' in s.get("bash", ""), s.get("bash", "") + err)
    note = open(os.path.join(proj, "note.txt")).read() if os.path.exists(os.path.join(proj, "note.txt")) else ""
    check("Write tool (on the host's folder)", "written by claude" in s.get("read", "") and note == "edited by claude\n",
          s.get("write", "") + s.get("read", "") + repr(note))
    check("Edit tool", note == "edited by claude\n", s.get("edit", "") + repr(note))
    check("Grep tool (rg)", "notes.md" in s.get("grep", "") and "gamma" in s.get("grep", ""), s.get("grep", "") + err)
    check("Glob tool (rg --files)", "notes.md" in s.get("glob", ""), s.get("glob", "") + err)

shutil.rmtree(work, ignore_errors=True)
print(f"{len(fails)} failed" if fails else "all passed")
sys.exit(1 if fails else 0)
