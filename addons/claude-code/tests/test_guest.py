"""claude inside the guest (engine exec), against mock_llm.py in HTTP mode on 18091."""
import json, os, shutil, subprocess, sys, tempfile, urllib.request
S = os.path.dirname(os.path.abspath(__file__))
RT = os.environ["COLLABO_RUNTIME"]
LOG = os.environ["CLAUDE_TEST_LOG"]
fails = []
def check(name, ok, detail=""):
    print(("PASS " if ok else "FAIL ") + name + ("" if ok else f"\n     {str(detail)[-1500:]}"))
    if not ok: fails.append(name)
def scenario(turns):
    urllib.request.urlopen(urllib.request.Request("http://127.0.0.1:18091/__scenario", data=json.dumps({"turns": turns}).encode(), method="POST")).read()
def requests():
    return [json.loads(l) for l in open(LOG)] if os.path.exists(LOG) else []
def results(req):
    return [b for b in req["body"]["messages"][-1]["content"] if b.get("type") == "tool_result"]

work = tempfile.mkdtemp(prefix="claude-guest-")
def guest(command, extra=()):
    argv = [f"{RT}/bin/collabo-core-engine", "exec", "--kernel", "app/images/vmlinux.wasm", "--initramfs", "app/images/initramfs.cpio",
            "--addon-dir", "app/images/addons", "--allow-loopback", "--mount", f"{work}:/work",
            "--addon-config", "claude-code:baseUrl=http://localhost:18091", "--addon-config", "claude-code:model=claude-sonnet-5",
            *extra, "--cwd", "/work", "--", "/bin/sh", "-c", command]
    p = subprocess.run(argv, cwd=RT, capture_output=True, text=True, timeout=240)
    return p.returncode, p.stdout, p.stderr

open(os.path.join(work, "main.py"), "w").write("print('v1')\n")

# 1. Read, Edit (acceptEdits), Bash in the guest; the files are the host's.
scenario([
    {"tools": [{"name": "Read", "input": {"file_path": "/work/main.py"}}]},
    {"tools": [{"name": "Edit", "input": {"file_path": "/work/main.py", "old_string": "v1", "new_string": "v2"}}]},
    {"tools": [{"name": "Bash", "input": {"command": "cat main.py && uname -m"}}]},
    {"text": "Updated and checked."},
])
code, out, err = guest("claude -p --permission-mode acceptEdits 'update it'")
r = requests()
check("guest: Read → Edit → Bash, answer", out.strip() == "Updated and checked." and open(os.path.join(work, "main.py")).read() == "print('v2')\n", (code, out, err[-800:]))
check("guest: Bash ran in the guest (wasm)", "print('v2')" in results(r[3])[0]["content"] and "\nwasm" in results(r[3])[0]["content"], results(r[3]))

# 2. Background shell, TodoWrite, sub-agent, all in the guest.
scenario([
    {"tools": [{"name": "Bash", "input": {"command": "for i in 1 2 3; do echo tick$i; sleep 0.2; done", "run_in_background": True}}]},
    {"tools": [{"name": "TodoWrite", "input": {"todos": [{"content": "Wait", "status": "in_progress", "activeForm": "Waiting"}]}}], "delay": 1.5},
    {"tools": [{"name": "BashOutput", "input": {"bash_id": "bash_1"}}]},
    {"tools": [{"name": "Task", "input": {"description": "count files", "prompt": "count", "subagent_type": "general-purpose"}}]},
    {"tools": [{"name": "Glob", "input": {"pattern": "*.py"}}]},
    {"text": "one python file"},
    {"text": "finished"},
])
code, out, err = guest("claude -p --dangerously-skip-permissions 'go'")
r = requests()
check("guest: background shell output", "tick3" in results(r[3])[0]["content"] and "exited with code 0" in results(r[3])[0]["content"], results(r[3]))
check("guest: sub-agent report", results(r[6])[0]["content"] == "one python file" and out.strip() == "finished", (out, err[-500:]))

# 3. Settings, hooks and a custom command in the project, read in the guest.
os.makedirs(os.path.join(work, ".claude/commands"), exist_ok=True)
json.dump({"permissions": {"allow": ["Bash(echo:*)"], "deny": ["Read(./secret.txt)"]},
           "hooks": {"PostToolUse": [{"matcher": "Bash", "hooks": [{"type": "command", "command": "echo \"hooked $(cat | grep -o '\"tool_name\":\"[A-Za-z]*\"')\" > /work/hook.log"}]}]}},
          open(os.path.join(work, ".claude/settings.json"), "w"))
open(os.path.join(work, "secret.txt"), "w").write("k")
scenario([{"tools": [{"name": "Bash", "input": {"command": "echo allowed-by-rule > out.txt"}}, {"name": "Read", "input": {"file_path": "secret.txt"}}]}, {"text": "ok"}])
code, out, err = guest("claude -p 'go'")
r = requests()
check("guest: allow rule from project settings", open(os.path.join(work, "out.txt")).read().strip() == "allowed-by-rule", results(r[1]))
check("guest: deny rule for Read", "denied by the rule Read(./secret.txt)" in results(r[1])[1]["content"], results(r[1]))
check("guest: PostToolUse hook ran in the guest", open(os.path.join(work, "hook.log")).read().strip() == 'hooked "tool_name":"Bash"', open(os.path.join(work, "hook.log")).read() if os.path.exists(os.path.join(work, "hook.log")) else "no hook.log")

# 4. An MCP server running in the guest (sh), from .mcp.json.
open(os.path.join(work, "srv.sh"), "w").write(r'''while IFS= read -r line; do
  id=$(echo "$line" | sed -n 's/.*"id":\([0-9]*\).*/\1/p')
  case "$line" in
    *'"initialize"'*) echo "{\"jsonrpc\":\"2.0\",\"id\":$id,\"result\":{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},\"serverInfo\":{\"name\":\"t\"}}}" ;;
    *'"tools/list"'*) echo "{\"jsonrpc\":\"2.0\",\"id\":$id,\"result\":{\"tools\":[{\"name\":\"uname\",\"description\":\"Machine\",\"inputSchema\":{\"type\":\"object\"}}]}}" ;;
    *'"tools/call"'*) echo "{\"jsonrpc\":\"2.0\",\"id\":$id,\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"$(uname -m)\"}]}}" ;;
  esac
done''')
json.dump({"mcpServers": {"sys": {"command": "/bin/sh", "args": ["/work/srv.sh"]}}}, open(os.path.join(work, ".mcp.json"), "w"))
scenario([{"tools": [{"name": "mcp__sys__uname", "input": {}}]}, {"text": "done"}])
code, out, err = guest("claude -p --allowedTools mcp__sys 'which machine?'")
r = requests()
check("guest: MCP stdio server in the guest", results(r[1])[0]["content"] == "wasm", results(r[1]) if len(r) > 1 else (out, err[-800:]))

# 4b. An MCP server over https whose certificate is of a private CA (tests/fixtures), as a local
# server's self-signed one: the engine refuses it, unless claude asks it not to check.
json.dump({"mcpServers": {"tls": {"type": "http", "url": "https://localhost:18092/mcp"}}}, open(os.path.join(work, ".mcp.json"), "w"))
code, out, err = guest("claude mcp list")
check("guest: https MCP, private CA: the engine refuses it", "tls: project - ✗ failed: network:" in out and "certificate" in out and "mcpSkipTlsVerify" in out, (out, err[-800:]))
code, out, err = guest("claude mcp list --mcp-skip-tls-verify")
check("guest: claude mcp list --mcp-skip-tls-verify", "tls: project - ✓ Connected (1 tools)" in out, (out, err[-800:]))
scenario([{"tools": [{"name": "mcp__tls__lookup", "input": {"q": "guest"}}]}, {"text": "done"}])
code, out, err = guest("claude -p --allowedTools mcp__tls 'look it up'", ["--addon-config", "claude-code:mcpSkipTlsVerify=true"])
r = requests()
check("guest: mcpSkipTlsVerify from the app: the tool works", len(r) > 1 and results(r[1])[0]["content"] == "looked up guest", results(r[1]) if len(r) > 1 else (out, err[-800:]))
os.remove(os.path.join(work, ".mcp.json"))

# 4b'. The engine's --skip-tls-verify: no certificate is checked for anything the host fetches,
# without claude's option; the app's secrets still go (the app chose it).
json.dump({"mcpServers": {"tls": {"type": "http", "url": "https://localhost:18092/mcp"}, "keyed": {"type": "http", "url": "https://localhost:18092/auth/mcp"}}}, open(os.path.join(work, ".mcp.json"), "w"))
code, out, err = guest("claude mcp list; hfetch https://localhost:18092/page", ["--skip-tls-verify", "--secret", "localhost:x-auth-token=t0ken"])
check("guest: engine --skip-tls-verify: https MCP connects without claude's option", "tls: project - ✓ Connected (1 tools)" in out, (out, err[-800:]))
check("guest: engine --skip-tls-verify: the app's secret still goes", "keyed: project - ✓ Connected (1 tools)" in out, (out, err[-800:]))
check("guest: engine --skip-tls-verify: hfetch too, and a warning at start", "fetched text" in out and "--skip-tls-verify: https certificates are not checked" in err, (out, err[-800:]))
os.remove(os.path.join(work, ".mcp.json"))

# 4b''. The model's API over https with a certificate of a private CA (the mock on 18092, which
# has no script: it answers "(no more scripted turns)"): apiSkipTlsVerify, apart from MCP's.
API = ["--addon-config", "claude-code:baseUrl=https://localhost:18092"]
code, out, err = guest("claude -p hi", API)
check("guest: model API, private CA: refused at once, with a hint", code != 0 and "certificate" in err and "apiSkipTlsVerify" in err and "retrying" not in err, (code, out, err[-800:]))
code, out, err = guest("claude -p hi", API + ["--addon-config", "claude-code:apiSkipTlsVerify=true"])
check("guest: apiSkipTlsVerify from the app: the model answers", out.strip() == "(no more scripted turns)", (code, out, err[-800:]))
code, out, err = guest("claude -p --api-skip-tls-verify hi", API + ["--addon-config", "claude-code:apiKey=k-123"])
check("guest: the app's key is not sent unchecked: refused, with a hint", code != 0 and "only sent to a server whose certificate is checked" in err and "--skip-tls-verify" in err, (code, out, err[-800:]))
scenario([])
code, out, err = guest("claude -p hi", API + ["--addon-config", "claude-code:apiKey=k-123", "--skip-tls-verify"])
r = requests()
check("guest: the engine's --skip-tls-verify: the model answers, with the app's key", out.strip() == "(no more scripted turns)" and r and r[-1]["headers"].get("x-api-key") == "k-123", (out, err[-800:], r[-1]["headers"] if r else None))

# 4c. An MCP server that takes its key in a header of its own: the engine refuses the header
# unless its policy allows it (--allow-header).
json.dump({"mcpServers": {"auth": {"type": "http", "url": "http://localhost:18091/auth/mcp", "headers": {"X-Auth-Token": "t0ken"}}}}, open(os.path.join(work, ".mcp.json"), "w"))
code, out, err = guest("claude mcp list")
check("guest: an MCP header the policy does not allow: refused, with a hint", 'auth: project - ✗ failed: header-not-allowed: request header "x-auth-token" is not allowed' in out and "--allow-header" in out, (out, err[-800:]))
scenario([{"tools": [{"name": "mcp__auth__lookup", "input": {"q": "key"}}]}, {"text": "done"}])
code, out, err = guest("claude -p --allowedTools mcp__auth 'look it up'", ["--allow-header", "x-auth-token"])
r = requests()
check("guest: --allow-header x-auth-token: the server gets its key", len(r) > 1 and results(r[1])[0]["content"] == "looked up key", results(r[1]) if len(r) > 1 else (out, err[-800:]))
os.remove(os.path.join(work, ".mcp.json"))

# 5. Sessions persist in the guest's ~/.claude: --continue within one boot.
scenario([{"text": "first"}, {"text": "second"}])
code, out, err = guest("claude -p 'remember 7' && claude -p -c 'what?'")
r = requests()
check("guest: --continue", out.split() == ["first", "second"] and any("remember 7" in json.dumps(m) for m in r[1]["body"]["messages"]), (out, err[-600:]))

shutil.rmtree(work, ignore_errors=True)
print(f"\n{'ALL PASSED' if not fails else str(len(fails)) + ' FAILED'}")
sys.exit(1 if fails else 0)
