"""Functional checks of claude -p against mock_llm.py (bridge protocol on 18090)."""
import json, os, shutil, subprocess, sys, tempfile, urllib.request

S = os.path.dirname(os.path.abspath(__file__))
CLAUDE = os.environ["CLAUDE_BIN"]
LOG = os.environ["CLAUDE_TEST_LOG"]
fails = []

def check(name, ok, detail=""):
    print(("PASS " if ok else "FAIL ") + name + ("" if ok else f"\n     {str(detail)[:1500]}"))
    if not ok: fails.append(name)

import socket
def scenario(turns):
    body = json.dumps({"turns": turns}).encode()
    s = socket.create_connection(("127.0.0.1", 18090))
    s.sendall(b"POST http://x/__scenario\ncontent-length: %d\n\n" % len(body) + body)
    s.recv(100); s.close()

def requests():
    return [json.loads(l) for l in open(LOG)] if os.path.exists(LOG) else []

root = tempfile.mkdtemp(prefix="claude-native-")
home = os.path.join(root, "home"); proj = os.path.join(root, "proj")
os.makedirs(home); os.makedirs(proj)
subprocess.run(["git", "init", "-q", proj])
ENV = {**os.environ, "HOME": home, "CLAUDE_CONFIG_DIR": os.path.join(home, ".claude"), "CLAUDE_CODE_CONFIG": "/nonexistent",
       "COLLABO_BRIDGE": "tcp:127.0.0.1:18090", "NO_COLOR": "1"}

def run(*args, stdin="", cwd=proj, env=None):
    p = subprocess.run([CLAUDE, *args], cwd=cwd, env={**ENV, **(env or {})}, input=stdin, capture_output=True, text=True, timeout=60)
    return p.returncode, p.stdout, p.stderr

def last_tool_results():
    r = requests()
    out = []
    for block in r[-1]["body"]["messages"][-1]["content"]:
        if block.get("type") == "tool_result": out.append(block)
    return out

# 1. A safe command runs without asking; the answer comes back.
scenario([{"tools": [{"name": "Bash", "input": {"command": "echo hi && pwd"}}]}, {"text": "Done: saw hi."}])
code, out, err = run("-p", "go")
check("safe Bash runs without asking", code == 0 and out.strip() == "Done: saw hi." and "hi\n" + os.path.realpath(proj) in last_tool_results()[0]["content"], (code, out, err, last_tool_results()))

# 2. A command that changes things is refused in -p (nobody to ask), and reported.
scenario([{"tools": [{"name": "Bash", "input": {"command": "touch made.txt"}}]}, {"text": "ok"}])
code, out, err = run("-p", "--output-format", "json", "go")
j = json.loads(out)
check("-p refuses what needs a yes", not os.path.exists(os.path.join(proj, "made.txt")) and j["permission_denials"][0]["tool_name"] == "Bash" and last_tool_results()[0]["is_error"], j)

# 3. --allowedTools lets it through.
scenario([{"tools": [{"name": "Bash", "input": {"command": "touch made.txt"}}]}, {"text": "ok"}])
code, out, err = run("-p", "--allowedTools", "Bash(touch:*)", "go")
check("--allowedTools Bash(touch:*)", os.path.exists(os.path.join(proj, "made.txt")), (out, err))

# 4. Edit before Read is refused; Read then Edit works (acceptEdits).
open(os.path.join(proj, "app.py"), "w").write("def f():\n    return 1\n")
scenario([
    {"tools": [{"name": "Edit", "input": {"file_path": "app.py", "old_string": "return 1", "new_string": "return 2"}}]},
    {"tools": [{"name": "Read", "input": {"file_path": "app.py"}}]},
    {"tools": [{"name": "Edit", "input": {"file_path": "app.py", "old_string": "return 1", "new_string": "return 2"}}]},
    {"text": "edited"},
])
code, out, err = run("-p", "--permission-mode", "acceptEdits", "change it")
r = requests()
first_result = [b for b in r[1]["body"]["messages"][-1]["content"] if b.get("type") == "tool_result"][0]
check("Edit needs a Read first", "has not been read yet" in first_result["content"], first_result)
check("Read then Edit (acceptEdits)", open(os.path.join(proj, "app.py")).read() == "def f():\n    return 2\n" and out.strip() == "edited", (out, err))

# 5. Settings: deny rule, and a PreToolUse hook that blocks.
os.makedirs(os.path.join(proj, ".claude"), exist_ok=True)
json.dump({"permissions": {"deny": ["Bash(rm:*)"]},
           "hooks": {"PreToolUse": [{"matcher": "Write", "hooks": [{"type": "command", "command": "echo 'no writing today' >&2; exit 2"}]}],
                     "UserPromptSubmit": [{"hooks": [{"type": "command", "command": "echo 'context from hook'"}]}]}},
          open(os.path.join(proj, ".claude/settings.json"), "w"))
scenario([{"tools": [{"name": "Bash", "input": {"command": "rm -rf x"}}, {"name": "Write", "input": {"file_path": "w.txt", "content": "x"}}]}, {"text": "ok"}])
code, out, err = run("-p", "--dangerously-skip-permissions", "go")
res = last_tool_results()
check("deny rule wins even in bypass mode", "denied by the rule Bash(rm:*)" in res[0]["content"], res)
check("PreToolUse hook blocks with its reason", "no writing today" in res[1]["content"] and not os.path.exists(os.path.join(proj, "w.txt")), res)
first_user = requests()[0]["body"]["messages"][0]["content"]
check("UserPromptSubmit hook adds context", any("context from hook" in b.get("text", "") for b in first_user), first_user)
os.remove(os.path.join(proj, ".claude/settings.json"))

# 6. TodoWrite, a sub-agent (Task), background shell.
scenario([
    {"tools": [{"name": "TodoWrite", "input": {"todos": [{"content": "Look", "status": "in_progress", "activeForm": "Looking"}]}}]},
    {"tools": [{"name": "Task", "input": {"description": "find files", "prompt": "list the files", "subagent_type": "Explore"}}]},
    {"tools": [{"name": "Glob", "input": {"pattern": "*.py"}}]},   # the sub-agent's turn
    {"text": "sub-agent report: app.py"},                           # the sub-agent's answer
    {"tools": [{"name": "Bash", "input": {"command": "sleep 0.2; echo bg-done", "run_in_background": True}}]},
    {"tools": [{"name": "BashOutput", "input": {"bash_id": "bash_1"}}], "delay": 0.6},
    {"text": "all done"},
])
code, out, err = run("-p", "--dangerously-skip-permissions", "work")
r = requests()
sub = r[2]["body"]
check("Task runs a sub-agent with its own prompt and tools", "file search specialist" in sub["system"][0]["text"] and all(t["name"] != "Task" for t in sub["tools"]) and sub["messages"][0]["content"][0]["text"] == "list the files", sub["system"][0]["text"][:200])
task_result = [b for b in r[4]["body"]["messages"][-1]["content"] if b.get("type") == "tool_result"][0]
check("the sub-agent's report is the Task result", task_result["content"] == "sub-agent report: app.py", task_result)
bg = [b for b in r[6]["body"]["messages"][-1]["content"] if b.get("type") == "tool_result"][0]
check("background shell + BashOutput", "bg-done" in bg["content"] and "exited with code 0" in bg["content"], bg)
check("the whole run answered", out.strip() == "all done", (out, err))

# 7. stream-json, then --continue sees the conversation.
scenario([{"text": "first answer"}])
code, out, err = run("-p", "--output-format", "stream-json", "remember 42")
lines = [json.loads(l) for l in out.splitlines()]
check("stream-json: init, messages, result", lines[0]["subtype"] == "init" and lines[-1]["type"] == "result" and any(l["type"] == "assistant" for l in lines), [l["type"] for l in lines])
scenario([{"text": "second answer"}])
code, out, err = run("-p", "-c", "what was the number?")
msgs = requests()[0]["body"]["messages"]
check("--continue resumes the last conversation", any("remember 42" in json.dumps(m) for m in msgs) and out.strip() == "second answer", [m["role"] for m in msgs])

# 8. MCP server from .mcp.json; the tool is offered and called.
server = os.path.join(root, "srv.sh")
open(server, "w").write(r'''while IFS= read -r line; do
  id=$(echo "$line" | sed -n 's/.*"id":\([0-9]*\).*/\1/p')
  case "$line" in
    *'"initialize"'*) echo "{\"jsonrpc\":\"2.0\",\"id\":$id,\"result\":{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},\"serverInfo\":{\"name\":\"t\"}}}" ;;
    *'"tools/list"'*) echo "{\"jsonrpc\":\"2.0\",\"id\":$id,\"result\":{\"tools\":[{\"name\":\"weather\",\"description\":\"Weather\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"city\":{\"type\":\"string\"}}}}]}}" ;;
    *'"tools/call"'*) echo "{\"jsonrpc\":\"2.0\",\"id\":$id,\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"sunny in Seoul\"}]}}" ;;
  esac
done''')
json.dump({"mcpServers": {"wx": {"command": "/bin/sh", "args": [server]}}}, open(os.path.join(proj, ".mcp.json"), "w"))
scenario([{"tools": [{"name": "mcp__wx__weather", "input": {"city": "Seoul"}}]}, {"text": "It is sunny."}])
code, out, err = run("-p", "--allowedTools", "mcp__wx", "weather?")
r = requests()
check("MCP tool offered", any(t["name"] == "mcp__wx__weather" for t in r[0]["body"]["tools"]), [t["name"] for t in r[0]["body"]["tools"]])
check("MCP tool called", last_tool_results()[0]["content"] == "sunny in Seoul", last_tool_results())
code, out, err = run("mcp", "list")
check("claude mcp list", "wx: project - ✓ Connected (1 tools)" in out, out + err)
os.remove(os.path.join(proj, ".mcp.json"))

# 8b. A streamable-HTTP MCP server, reached through the host's request API.
json.dump({"mcpServers": {"web": {"type": "http", "url": "https://mcp.example.com/mcp"}}}, open(os.path.join(proj, ".mcp.json"), "w"))
scenario([{"tools": [{"name": "mcp__web__lookup", "input": {"q": "rust"}}]}, {"text": "done"}])
code, out, err = run("-p", "--allowedTools", "mcp__web", "look it up")
check("MCP over HTTP: session id kept, SSE answer read", last_tool_results()[0]["content"] == "looked up rust", (last_tool_results(), err[-400:]))
os.remove(os.path.join(proj, ".mcp.json"))

# 9. OpenAI-compatible provider: tools as functions, tool results as role tool.
scenario([{"tools": [{"name": "Grep", "input": {"pattern": "return"}}]}, {"text": "found it"}])
code, out, err = run("-p", "--provider", "openai", "--base-url", "http://localhost:11434/v1", "find")
r = requests()
check("openai: model auto-picked, tools as functions", r[0]["body"]["model"] == "mock-model" and r[0]["body"]["tools"][0]["type"] == "function", r[0]["body"].get("model"))
check("openai: tool result goes back as role tool", r[1]["body"]["messages"][-1]["role"] == "tool" and "app.py" in r[1]["body"]["messages"][-1]["content"], r[1]["body"]["messages"][-1])

# 10. Thinking words turn on extended thinking; plan mode offers ExitPlanMode.
scenario([{"thinking": "hmm", "text": "thought"}])
run("-p", "ultrathink about it")
b = requests()[0]["body"]
check("ultrathink sets a thinking budget", b.get("thinking") == {"type": "enabled", "budget_tokens": 31999} and b["max_tokens"] > 31999, b.get("thinking"))
scenario([{"tools": [{"name": "Write", "input": {"file_path": "p.txt", "content": "x"}}]}, {"text": "planned"}])
run("-p", "--permission-mode", "plan", "plan it")
r = requests()
check("plan mode: ExitPlanMode offered, writes refused", any(t["name"] == "ExitPlanMode" for t in r[0]["body"]["tools"]) and "Plan mode" in last_tool_results()[0]["content"] and not os.path.exists(os.path.join(proj, "p.txt")), last_tool_results())

# 11. Custom agents and commands are loaded; config subcommand writes settings.
code, out, err = run("config", "set", "model", "sonnet")
check("claude config set", code == 0 and json.load(open(os.path.join(proj, ".claude/settings.local.json")))["model"] == "sonnet", out + err)
scenario([{"text": "x"}])
run("-p", "hi")
check("settings model alias resolves", requests()[0]["body"]["model"] == "claude-sonnet-5", requests()[0]["body"]["model"])

# 11b. Skills: listed in the Skill tool, loaded with their folder.
os.makedirs(os.path.join(proj, ".claude/skills/deploy"), exist_ok=True)
open(os.path.join(proj, ".claude/skills/deploy/SKILL.md"), "w").write("---\nname: deploy\ndescription: Deploy the app to staging\nallowed-tools: Bash(./deploy.sh:*)\n---\nRun ./deploy.sh with the target.\n")
scenario([{"tools": [{"name": "Skill", "input": {"skill": "deploy"}}]}, {"tools": [{"name": "Bash", "input": {"command": "./deploy.sh staging"}}]}, {"text": "deployed"}])
open(os.path.join(proj, "deploy.sh"), "w").write("#!/bin/sh\necho deploying $1\n"); os.chmod(os.path.join(proj, "deploy.sh"), 0o755)
code, out, err = run("-p", "ship it")
r = requests()
skill_tool = [t for t in r[0]["body"]["tools"] if t["name"] == "Skill"]
check("Skill tool lists the skills", skill_tool and "deploy: Deploy the app to staging" in skill_tool[0]["description"], [t["name"] for t in r[0]["body"]["tools"]])
loaded = [b for b in r[1]["body"]["messages"][-1]["content"] if b.get("type") == "tool_result"][0]["content"]
check("Skill loads the body and its folder", "Run ./deploy.sh with the target." in loaded and ".claude/skills/deploy" in loaded, loaded)
ran = [b for b in r[2]["body"]["messages"][-1]["content"] if b.get("type") == "tool_result"][0]
check("the skill's allowed-tools let its command run in -p", ran["content"] == "deploying staging" and not ran["is_error"], ran)

# 11c. Plugins: a marketplace, install, commands/agents/skills/hooks, "if" hooks, disable.
mkt = os.path.join(root, "mkt"); plug = os.path.join(mkt, "plugins/hello")
for sub in (".claude-plugin", "commands", "agents", "skills/pskill", "hooks"):
    os.makedirs(os.path.join(plug, sub), exist_ok=True)
os.makedirs(os.path.join(mkt, ".claude-plugin"), exist_ok=True)
json.dump({"name": "testmkt", "plugins": [{"name": "hello", "description": "Hello plugin", "source": "./plugins/hello"}]}, open(os.path.join(mkt, ".claude-plugin/marketplace.json"), "w"))
json.dump({"name": "hello", "description": "Says hello", "version": "0.1.0"}, open(os.path.join(plug, ".claude-plugin/plugin.json"), "w"))
open(os.path.join(plug, "commands/greet.md"), "w").write("---\ndescription: Greet\n---\nGreet $ARGUMENTS warmly.\n")
open(os.path.join(plug, "agents/helper.md"), "w").write("---\nname: plugin-helper\ndescription: Helps from a plugin\n---\nYou help.\n")
open(os.path.join(plug, "skills/pskill/SKILL.md"), "w").write("---\nname: pskill\ndescription: A plugin skill\n---\nDo the plugin thing.\n")
json.dump({"hooks": {
    "UserPromptSubmit": [{"hooks": [{"type": "command", "command": "echo \"plugin hook at ${CLAUDE_PLUGIN_ROOT}\""}]}],
    "PreToolUse": [{"matcher": "Bash", "hooks": [{"type": "command", "command": "echo 'no commits from the plugin' >&2; exit 2", "if": "Bash(git commit:*)"}]}]}},
    open(os.path.join(plug, "hooks/hooks.json"), "w"))
code, out, err = run("plugin", "marketplace", "add", mkt)
code2, out2, err2 = run("plugin", "install", "hello@testmkt")
cache = os.path.join(home, ".claude/plugins/cache/testmkt/hello")
check("plugin: marketplace add + install copies and enables it", code == 0 and code2 == 0 and os.path.isfile(os.path.join(cache, "commands/greet.md")), out + err + out2 + err2)
scenario([{"tools": [{"name": "Bash", "input": {"command": "git commit -m x"}}, {"name": "Bash", "input": {"command": "echo fine"}}]}, {"text": "greeted"}])
code, out, err = run("-p", "--dangerously-skip-permissions", "/hello:greet Bob")
r = requests()
first = r[0]["body"]
texts = [b.get("text", "") for b in first["messages"][0]["content"]]
check("plugin command runs from -p as /plugin:command", texts[0] == "Greet Bob warmly.", texts)
check("plugin hook ran with CLAUDE_PLUGIN_ROOT", any(f"plugin hook at {cache}" in t for t in texts), texts)
check("plugin agent and skill are offered", "plugin-helper" in first["system"][0]["text"] and any(t["name"] == "Skill" and "pskill" in t["description"] for t in first["tools"]))
res = [b for b in r[1]["body"]["messages"][-1]["content"] if b.get("type") == "tool_result"]
check("hook 'if': only git commit is blocked", "no commits from the plugin" in res[0]["content"] and res[1]["content"] == "fine", res)
run("plugin", "disable", "hello@testmkt")
scenario([{"text": "plain"}])
run("-p", "hi")
texts = [b.get("text", "") for b in requests()[0]["body"]["messages"][0]["content"]]
check("a disabled plugin's hooks are gone", not any("plugin hook" in t for t in texts), texts)
run("plugin", "uninstall", "hello@testmkt")
check("plugin uninstall removes the copy", not os.path.exists(cache))

# 12. Auto-compaction when the context is nearly full, then the work goes on.
scenario([
    {"tools": [{"name": "Bash", "input": {"command": "echo step1"}}], "usage": {"input_tokens": 190000}},
    {"text": "SUMMARY: the user asked for steps; step1 ran."},
    {"text": "continued after compaction"},
])
code, out, err = run("-p", "do steps")
r = requests()
check("auto-compaction: a summary request without tools", len(r) == 3 and "tools" not in r[1]["body"] and "summariz" in r[1]["body"]["system"][0]["text"].lower(), [list(x["body"].keys()) for x in r])
check("auto-compaction: the conversation restarts from the summary", "This session is being continued" in json.dumps(r[2]["body"]["messages"][0]) and "SUMMARY:" in json.dumps(r[2]["body"]["messages"][0]) and out.strip() == "continued after compaction", (out, err))

# 13. Web search (Anthropic's server tool): its blocks stream in and go back as they came.
scenario([{"search": "rust wasm", "text": "Found it."}, {"text": "next"}])
code, out, err = run("-p", "search the web")
check("web search answer", out.strip() == "Found it.", (out, err))
scenario([{"text": "next"}])
code, out, err = run("-p", "-c", "and?")
assistant = [m for m in requests()[0]["body"]["messages"] if m["role"] == "assistant"][-1]["content"]
check("server_tool_use and its result sent back intact", assistant[0] == {"type": "server_tool_use", "id": "srvtoolu_1", "name": "web_search", "input": {"query": "rust wasm"}} and assistant[1]["type"] == "web_search_tool_result", assistant)

shutil.rmtree(root, ignore_errors=True)
print(f"\n{'ALL PASSED' if not fails else str(len(fails)) + ' FAILED'}")
sys.exit(1 if fails else 0)
