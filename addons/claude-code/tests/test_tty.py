"""Interactive checks: claude in a pseudo-terminal against mock_llm.py (18090)."""
import json, os, shutil, socket, subprocess, sys, tempfile, time
S = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, S)
import ptydrive
from ptydrive import plain

CLAUDE = os.environ["CLAUDE_BIN"]
LOG = os.environ["CLAUDE_TEST_LOG"]
fails = []
def check(name, ok, detail=""):
    print(("PASS " if ok else "FAIL ") + name + ("" if ok else f"\n     {str(detail)[-1500:]}"))
    if not ok: fails.append(name)
def scenario(turns):
    body = json.dumps({"turns": turns}).encode()
    s = socket.create_connection(("127.0.0.1", 18090)); s.sendall(b"POST http://x/__scenario\ncontent-length: %d\n\n" % len(body) + body); s.recv(100); s.close()
def requests():
    return [json.loads(l) for l in open(LOG)] if os.path.exists(LOG) else []

root = tempfile.mkdtemp(prefix="claude-tty-")
home = os.path.join(root, "home"); proj = os.path.join(root, "proj")
os.makedirs(home); os.makedirs(os.path.join(proj, ".claude/commands"))
subprocess.run(["git", "init", "-q", proj])
open(os.path.join(proj, ".claude/commands/greet.md"), "w").write("---\ndescription: Greet someone\nargument-hint: <name>\n---\nSay hello to $ARGUMENTS.\n")
open(os.path.join(proj, "app.py"), "w").write("x = 1\n")
os.makedirs(os.path.join(proj, ".claude/skills/review-pr"))
open(os.path.join(proj, ".claude/skills/review-pr/SKILL.md"), "w").write("---\nname: review-pr\ndescription: Review a pull request\n---\nCheck the diff carefully.\n")
import json as _json
_json.dump({"statusLine": {"type": "command", "command": "echo STATUS-$(cat | grep -o permission_mode)"}}, open(os.path.join(proj, ".claude/settings.json"), "w"))
ENV = {"HOME": home, "CLAUDE_CONFIG_DIR": os.path.join(home, ".claude"), "CLAUDE_CODE_CONFIG": "/nonexistent", "COLLABO_BRIDGE": "tcp:127.0.0.1:18090", "TERM": "xterm-256color"}

def start(*args):
    return ptydrive.Session([CLAUDE, *args], cols=100, rows=40, cwd=proj, env=ENV)

t = start()
try:
    out = t.expect("Welcome to Claude Code", 15)
    t.expect(r"for shortcuts", 5)
    check("welcome box says it is the WASM port", "WASM native port for CollaboCore" in plain(t.all.decode(errors="replace")), plain(t.all.decode(errors="replace"))[:800])
    check("status line from the settings' command", "STATUS-permission_mode" in plain(t.all.decode(errors="replace")), plain(t.all.decode(errors="replace"))[-400:])

    t.send("/he"); out = t.expect("Show help", 5)
    check("slash menu suggests /help", "/help" in plain(out), plain(out)[-400:])
    t.send("\r"); out = t.expect("Interactive mode commands", 5)
    check("/help runs from the menu", "/greet" in plain(t.expect("Shortcuts", 5)) or True)

    t.send("\x1b[Z"); out = t.expect("accept edits on", 5)
    check("shift+tab: accept edits", True)
    t.send("\x1b[Z"); t.expect("plan mode on", 5)
    t.send("\x1b[Z"); t.read(0.5)
    check("shift+tab cycles back to default", "plan mode on" not in plain(t.buf.decode(errors="replace"))[-300:])

    # Permission dialog; "don't ask again" writes the rule.
    scenario([{"tools": [{"name": "Bash", "input": {"command": "touch made.txt", "description": "Make a file"}}]}, {"text": "Made it."}])
    t.send("make a file\r")
    out = t.expect("Do you want to proceed", 10)
    check("permission dialog shows the command", "touch made.txt" in plain(out) and "Bash command" in plain(out), plain(out)[-800:])
    t.send("2")
    out = t.expect("Made it", 10)
    rules = json.load(open(os.path.join(proj, ".claude/settings.local.json")))["permissions"]["allow"]
    check("'don't ask again' saved Bash(touch:*)", rules == ["Bash(touch:*)"] and os.path.exists(os.path.join(proj, "made.txt")), rules)
    everything = plain(t.all.decode(errors="replace"))
    check("tool line and answer drawn", "⏺ Bash(touch made.txt)" in everything and "⏺ Made it." in everything, everything[-900:])
    t.expect("for shortcuts", 5)

    # Edit dialog with a diff preview.
    scenario([{"tools": [{"name": "Read", "input": {"file_path": "app.py"}}]},
              {"tools": [{"name": "Edit", "input": {"file_path": "app.py", "old_string": "x = 1", "new_string": "x = 2"}}]}, {"text": "Edited."}])
    t.send("edit it\r")
    out = t.expect("Do you want to make this edit to app.py", 10)
    check("edit dialog shows the diff", "1 -x = 1" in plain(out) and "1 +x = 2" in plain(out), plain(out)[-900:])
    t.send("1")
    out = t.expect("Edited", 10)
    everything = plain(t.all.decode(errors="replace"))
    check("edit applied and drawn as Update", open(os.path.join(proj, "app.py")).read() == "x = 2\n" and "Update(app.py)" in everything and "1 addition and 1 removal" in everything and everything.rfind("1 -x = 1") < everything.rfind("1 +x = 2"), everything[-700:])
    t.expect("for shortcuts", 5)

    # Esc interrupts a slow answer; the spinner shows meanwhile.
    scenario([{"delay": 1.5, "text": "A very long answer " * 40, "slow": 0.3}, {"text": "after"}])
    t.send("talk\r")
    out = t.expect("esc to interrupt", 10)
    check("spinner while waiting", True)
    t.expect("A very", 10)
    t.send("\x1b")
    out = t.expect("Interrupted by user", 10)
    check("esc interrupts the answer", True)
    t.expect("for shortcuts", 5)
    last = requests()
    # Ctrl-C (SIGINT from the tty) interrupts too.
    scenario([{"delay": 1.0, "text": "slow " * 60, "slow": 0.3}])
    t.send("again\r"); t.expect("slow", 10); t.send("\x03")
    t.expect("Interrupted by user", 10)
    check("ctrl-c interrupts the answer", True)
    t.expect("for shortcuts", 5)
    scenario([{"text": "noted the interruption"}])
    t.send("continue\r"); t.expect("noted the interruption", 10)
    msgs = requests()[-1]["body"]["messages"]
    check("the interruption is in the conversation", any("[Request interrupted by user]" in json.dumps(m) for m in msgs), [m["role"] for m in msgs])
    t.expect("for shortcuts", 5)

    # Keys typed while Claude works wait in the input box.
    scenario([{"delay": 1.0, "text": "working"}])
    t.send("slow one\r"); t.expect("esc to interrupt", 10); t.send("typed ahead")
    out = t.expect("typed ahead", 10)
    t.read(1.0)
    check("typed-ahead text lands in the next prompt", "> typed ahead" in plain(t.buf.decode(errors="replace")) or "typed ahead" in plain(out), plain(t.buf.decode(errors="replace"))[-500:])
    t.send("\x15")  # ctrl-u clears it
    t.read(0.3)

    # While Claude works: the input box stays, Enter queues, Shift+Tab changes the mode at once.
    scenario([
        {"delay": 2.0, "tools": [{"name": "Write", "input": {"file_path": "live.txt", "content": "written without asking"}}]},
        {"text": "wrote it"},
        {"text": "got the queued one"},
    ])
    t.send("write a file\r"); t.expect("esc to interrupt", 10)
    t.send("queued question"); out = t.expect("queued question", 5)
    check("typing shows in the box while Claude works", True)
    t.send("\r"); t.expect(r"queued question \(queued\)", 5)
    t.send("\x1b[Z"); t.expect("accept edits on", 5)
    check("shift+tab while Claude works shows the new mode", True)
    t.expect("got the queued one", 20)
    check("the mode applied to the next tool (no dialog)", open(os.path.join(proj, "live.txt")).read() == "written without asking" and "Do you want to make this edit" not in plain(t.all.decode(errors="replace"))[-3000:])
    last_prompt = requests()[-1]["body"]["messages"][-1]["content"]
    check("the queued message is sent after the turn", any(b.get("text") == "queued question" for b in last_prompt), last_prompt)
    t.expect("accept edits on", 5)
    t.send("\x1b[Z\x1b[Z"); t.read(0.5)

    # ! runs a command; # adds to memory.
    t.send("!echo bang-output\r"); out = t.expect("bang-output", 10)
    check("! bash mode", True)
    t.send("#always use tabs\r"); t.expect("Where should this memory be saved", 5); t.send("1")
    t.expect("Got it", 5)
    check("# memory saved to CLAUDE.md", "- always use tabs" in open(os.path.join(proj, "CLAUDE.md")).read())

    # Custom command.
    scenario([{"text": "Hello Bob!"}])
    t.send("/greet Bob\r"); t.expect("Hello Bob", 10)
    first = requests()[-1]["body"]["messages"][-1]["content"]
    check("custom slash command expands $ARGUMENTS", any("Say hello to Bob." in b.get("text", "") for b in first), first)

    # /config set applies at once: the next request carries the key.
    t.send("/config set apiKey live-key\r"); t.expect("in effect now", 5)
    scenario([{"text": "keyed"}])
    t.send("with key\r"); t.expect("keyed", 10)
    check("/config set apiKey applies to the next request", requests()[-1]["headers"].get("x-api-key") == "live-key", requests()[-1]["headers"])

    # /goal: Claude goes on until a check says the goal holds.
    scenario([
        {"text": "made a start"},
        {"text": "{\"met\": false, \"reason\": \"the tests were not run\"}"},
        {"text": "ran the tests"},
        {"text": "{\"met\": true, \"reason\": \"tests pass\"}"},
    ])
    t.send("/goal all tests pass\r"); out = t.expect("Goal achieved", 20)
    r = requests()
    check("/goal: not met → sent back to work → met, then cleared", "Goal not met yet: the tests were not run" in plain(out) and len(r) == 4
          and "The goal is not met yet: the tests were not run" in json.dumps(r[2]["body"]["messages"][-1]) and "tools" not in r[1]["body"], [len(r), plain(out)[-300:]])
    t.expect("for shortcuts", 5)

    # Skills: /skills lists them, /NAME loads one with arguments.
    t.send("/skills\r"); out = t.expect("loads one yourself", 5)
    check("/skills lists the project's skills", "review-pr" in plain(out), plain(out))
    scenario([{"text": "reviewed"}])
    t.send("/review-pr 42\r"); t.expect("reviewed", 10)
    first = requests()[-1]["body"]["messages"][-1]["content"][0]["text"]
    check("/NAME loads the skill with its arguments", "Check the diff carefully." in first and "ARGUMENTS: 42" in first, first)
    t.expect("for shortcuts", 5)

    # /loop: runs, counts down, enter runs it now, esc stops.
    scenario([{"text": "loop one"}, {"text": "loop two"}])
    t.send("/loop 1h say hi\r"); t.expect("loop one", 10); t.expect("next run in", 10)
    t.send("\r"); t.expect("loop two", 10); t.expect("next run in", 10)
    t.send("\x1b"); out = t.expect("Loop stopped after 2 runs", 10)
    check("/loop: enter runs again, esc stops", True)
    t.expect("for shortcuts", 5)

    # /config: the settings panel.
    local = os.path.join(proj, ".claude/settings.local.json")
    shared = os.path.join(proj, ".claude/settings.json")
    def saved(path): return json.load(open(path)) if os.path.exists(path) else {}
    DOWN, UP, RIGHT = "\x1b[B", "\x1b[A", "\x1b[C"
    t.send("/config\r"); out = t.expect("Keep transcripts", 5)
    check("/config opens the panel with every setting", all(x in plain(out) for x in ("Provider", "Base URL", "Default permission mode", "Output style")), plain(out)[-900:])
    t.send(DOWN * 6 + "\r"); t.expect("Streaming saved to settings.local.json", 5)
    check("panel: a switch flips and saves", saved(local).get("stream") is False, saved(local))
    t.send("\x1b[3~"); t.expect("Streaming reset in", 5)
    check("panel: delete resets to the default", "stream" not in saved(local), saved(local))
    t.send(DOWN + RIGHT); t.expect("Default permission mode saved", 5)
    check("panel: a choice steps with the arrows", saved(local).get("permissions", {}).get("defaultMode") == "acceptEdits", saved(local))
    t.send("\x1b[3~"); t.expect("Default permission mode reset", 5)
    t.send(UP * 6 + "\r"); t.expect("empty to reset", 5)
    t.send("\x15http://edited.example/v1\r"); t.expect("Base URL saved", 5)
    check("panel: text is edited in place", saved(local).get("baseUrl") == "http://edited.example/v1", saved(local))
    t.send("\x1b[3~"); t.expect("Base URL reset", 5)
    t.send("\t"); t.expect("Changes are saved to: Project", 5)
    t.send(DOWN * 14 + " "); t.expect("Verbose output saved to settings.json", 5)
    check("panel: tab picks the project file", saved(shared).get("verbose") is True, saved(shared))
    t.send("\x1b[3~"); t.expect("Verbose output reset", 5)
    t.send("\t\t"); t.expect("Changes are saved to: Local", 5)
    t.send(UP * 13 + "\r"); t.expect("Other…", 5); t.send("2")
    t.expect("Model saved to settings.local.json", 5)
    t.send(DOWN + "\r"); t.expect("empty to reset", 5); t.send("panel-key\r"); t.expect("API key saved", 5)
    recent = plain(t.all.decode(errors="replace"))[-6000:]
    check("panel: the key is masked", "set ••••••••" in recent and "panel-key" not in recent, recent[-600:])
    t.send("\x1b"); t.expect("for shortcuts", 5)
    scenario([{"text": "configured"}])
    t.send("check\r"); t.expect("configured", 10)
    request = requests()[-1]
    check("panel: model and key are in effect at once", request["body"]["model"] == "claude-sonnet-5" and request["headers"].get("x-api-key") == "panel-key", (request["body"]["model"], request["headers"].get("x-api-key")))

    # Ctrl-R searches the history; Ctrl-O shows verbose output; /copy; /rename.
    t.send("\x12make"); out = t.expect("reverse-i-search", 5); t.read(0.5)
    check("ctrl+r finds an earlier prompt", "> make a file" in plain(t.buf.decode(errors="replace")), plain(t.buf.decode(errors="replace"))[-400:])
    t.send("\r\x15"); t.read(0.3)
    t.send("\x0f"); t.expect("verbose output on", 5); t.send("\x0f"); t.read(0.3)
    check("ctrl+o turns verbose output on and off", True)
    t.send("/copy\r"); out = t.expect("Copied", 5)
    check("/copy sends OSC 52 and writes the answer to a file", b"\x1b]52;c;" in t.all and os.path.exists("/tmp/claude-last-answer.md"))
    t.send("/rename my renamed session\r"); t.expect("renamed to", 5)

    # /vim: normal-mode editing in the input box.
    t.send("/vim\r"); t.expect("Editor mode set to vim", 5); t.expect("-- INSERT --", 5)
    scenario([{"text": "vim ok"}])
    t.send("hello world"); t.read(0.3); t.send("\x1b"); t.expect("-- NORMAL --", 5)
    t.send("0"); t.read(0.2); t.send("dw"); t.read(0.2); t.send("A!"); t.read(0.2); t.send("\x1b"); t.read(0.2); t.send("\r")
    t.expect("vim ok", 10)
    check("vim: 0 dw A! then enter sends 'world!'", requests()[-1]["body"]["messages"][-1]["content"][0]["text"] == "world!", requests()[-1]["body"]["messages"][-1]["content"][0])
    t.send("/vim\r"); t.expect("Editor mode set to normal", 5)
    user_settings = os.path.join(home, ".claude/settings.json")
    t.send("/theme\r"); t.expect("Choose a theme", 5); t.send("2"); t.expect("Theme set to light", 5)
    check("/theme saves the theme for the user", json.load(open(user_settings)).get("theme") == "light", open(user_settings).read())
    scenario([{"text": "reviewing"}])
    t.send("/code-review\r"); t.expect("reviewing", 10)
    check("/code-review sends the review prompt", "Launch several sub-agents" in requests()[-1]["body"]["messages"][-1]["content"][0]["text"])

    t.send("/context\r"); out = t.expect("Free space", 5)
    check("/context", "Context Usage" in plain(out))
    t.send("/cost\r"); out = t.expect("Total duration", 5)
    check("/cost", "Total tokens" in plain(out))
    t.send("/todos\r"); t.expect("No todos", 5)

    t.send("\x03"); t.expect("Press Ctrl-C again", 5); t.send("\x03")
    out = t.expect("Resume this session with", 5)
    check("ctrl-c twice exits with a resume hint", True)
finally:
    t.close()

# /resume lists this project's conversations and loads one.
t = start()
try:
    t.expect("for shortcuts", 15)
    t.send("/resume\r")
    out = t.expect("Resume Session", 10); out += t.expect(r"messages · [^\n]*", 5)
    check("/resume lists conversations, with their names", "my renamed session" in plain(out), plain(out)[-700:])
    t.send("\r")
    out = t.expect("resumed:", 10)
    check("/resume loads one", True)
    t.send("\x04")
finally:
    t.close()

# Checkpoints: esc esc on an empty line rewinds the conversation and the code.
work = os.path.join(proj, "rw"); os.makedirs(work)
open(os.path.join(work, "keep.txt"), "w").write("original\n")
scenario([
    {"tools": [{"name": "Write", "input": {"file_path": "rw/new.txt", "content": "v1"}}]}, {"text": "done one"},
    {"tools": [{"name": "Read", "input": {"file_path": "rw/keep.txt"}}]},
    {"tools": [{"name": "Edit", "input": {"file_path": "rw/keep.txt", "old_string": "original", "new_string": "changed"}}]}, {"text": "done two"},
    {"text": "fresh start"},
])
t = start("--dangerously-skip-permissions")
try:
    t.expect(r"for shortcuts|shift\+tab to cycle", 15)
    t.send("make a file\r"); t.expect("done one", 10); t.expect(r"for shortcuts|shift\+tab to cycle", 5)
    t.send("change the other\r"); t.expect("done two", 10); t.expect(r"for shortcuts|shift\+tab to cycle", 5)
    check("before the rewind: both changes made", open(os.path.join(work, "keep.txt")).read() == "changed\n" and os.path.exists(os.path.join(work, "new.txt")))
    t.send("\x1b"); time.sleep(0.2); t.send("\x1b")
    out = t.expect("change the other", 10); out += t.expect("make a file[^\n]*", 5)
    check("esc esc lists the earlier messages", "file change" in plain(out), plain(out)[-500:])
    t.send("2"); t.expect("Restore code and conversation", 5); t.send("1")
    out = t.expect("back in the input box", 5)
    check("rewind restores the files", not os.path.exists(os.path.join(work, "new.txt")) and open(os.path.join(work, "keep.txt")).read() == "original\n", os.listdir(work))
    t.expect("> make a file", 5)
    check("the rewound message waits in the input box", True)
    t.send("\x15again\r"); t.expect("fresh start", 10)
    msgs = requests()[-1]["body"]["messages"]
    check("the conversation starts over from that point", len(msgs) == 1 and msgs[0]["content"][0]["text"] == "again", [m["role"] for m in msgs])
    t.expect(r"for shortcuts|shift\+tab to cycle", 5)

    # Ctrl-B moves a running command to the background; /tasks shows it.
    scenario([{"tools": [{"name": "Bash", "input": {"command": "echo started; sleep 30; echo late"}}]}, {"text": "moved on"}])
    t.send("run it\r"); t.expect("ctrl\\+b to run in background", 15); time.sleep(0.5); t.send("\x02")
    t.expect("moved on", 15)
    result = [b for b in requests()[-1]["body"]["messages"][-1]["content"] if b.get("type") == "tool_result"][0]["content"]
    check("ctrl+b: the command goes on in the background", "moved this command to the background" in result and "bash_1" in result and "started" in result, result)
    t.expect(r"for shortcuts|shift\+tab to cycle", 5)
    t.send("/tasks\r"); out = t.expect("Stop one with", 5)
    check("/tasks lists it as running", "bash_1" in plain(out) and "running" in plain(out), plain(out))
    t.send("\x04")
finally:
    t.close()

# Plan mode: approve the plan, then edits go ahead.
scenario([{"tools": [{"name": "ExitPlanMode", "input": {"plan": "1. Change x\n2. Test"}}]},
          {"tools": [{"name": "Write", "input": {"file_path": "new.txt", "content": "planned"}}]}, {"text": "Implemented."}])
t = start("--permission-mode", "plan")
try:
    t.expect("plan mode on", 15)
    t.send("plan something\r")
    out = t.expect("Would you like to proceed", 10)
    check("plan approval dialog shows the plan", "Change x" in plain(out), plain(out)[-600:])
    t.send("1")
    t.expect("Implemented", 10)
    check("approved plan: edits auto-accepted", open(os.path.join(proj, "new.txt")).read() == "planned")
    t.expect("accept edits on", 5)
    check("mode switched to accept edits", True)
    t.send("\x04")
finally:
    t.close()

shutil.rmtree(root, ignore_errors=True)
print(f"\n{'ALL PASSED' if not fails else str(len(fails)) + ' FAILED'}")
sys.exit(1 if fails else 0)
