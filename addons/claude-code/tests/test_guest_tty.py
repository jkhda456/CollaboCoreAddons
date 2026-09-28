"""claude's interactive UI inside the guest: the engine in a pseudo-terminal (raw mode)."""
import json, os, shutil, sys, tempfile, time, urllib.request
S = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, S)
import ptydrive
from ptydrive import plain
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

work = tempfile.mkdtemp(prefix="claude-gtty-")
open(os.path.join(work, "a.txt"), "w").write("one\n")
t = ptydrive.Session([f"{RT}/bin/collabo-core-engine", "--kernel", f"{RT}/app/images/vmlinux.wasm", "--initramfs", f"{RT}/app/images/initramfs.cpio",
                      "--addon-dir", f"{RT}/app/images/addons", "--allow-loopback", "--mount", f"{work}:/work", "--arg", "collabo.quiet=1",
                      "--addon-config", "claude-code:baseUrl=http://localhost:18091", "--addon-config", "claude-code:model=claude-sonnet-5"],
                     cols=110, rows=40, env={"TERM": "xterm-256color"})
try:
    t.expect(r"# ", 90)
    t.send("cd /work && TERM=xterm-256color claude\r")
    t.expect("Welcome to Claude Code", 60)
    t.expect("for shortcuts", 20)
    check("guest: welcome and input box, marked as the WASM port", "WASM native port for CollaboCore" in plain(t.all.decode(errors="replace")))

    t.send("안녕하세요 wide text"); out = t.expect("wide text", 10)
    t.send("\x15"); t.read(0.5)
    check("guest: typing Hangul in the box", "안녕하세요" in plain(out))

    scenario([{"tools": [{"name": "Bash", "input": {"command": "rm a.txt"}}]}, {"text": "Removed."}])
    t.send("remove a.txt\r")
    out = t.expect("Do you want to proceed", 30)
    check("guest: permission dialog", "rm a.txt" in plain(out), plain(out)[-600:])
    t.send("3")
    t.expect("What should Claude do instead", 10)
    t.send("keep it\r")
    t.expect("Removed", 30)
    msgs = requests()[-1]["body"]["messages"]
    check("guest: 'No' with feedback reaches the model", os.path.exists(os.path.join(work, "a.txt")) and "keep it" in json.dumps(msgs[-1]), msgs[-1])
    t.expect("for shortcuts", 20)

    scenario([{"delay": 1.0, "text": "streaming words " * 80, "slow": 0.3}])
    t.send("talk\r")
    t.expect("esc to interrupt", 30)
    t.expect("streaming", 30)
    t0 = time.time(); t.send("\x1b")
    t.expect("Interrupted by user", 15)
    check("guest: esc interrupts a streaming answer (vsock read)", time.time() - t0 < 5, time.time() - t0)
    t.expect("for shortcuts", 20)

    scenario([{"tools": [{"name": "Bash", "input": {"command": "sleep 30"}}]}])
    t.send("/permissions add allow Bash(sleep:*)\r"); t.expect("Added allow rule", 10)
    t.send("wait\r")
    t.expect("Running", 30)
    t0 = time.time(); t.send("\x03")
    t.expect("Interrupted by user", 15)
    check("guest: ctrl-c stops a running command", time.time() - t0 < 5, time.time() - t0)
    t.expect("for shortcuts", 20)

    # While Claude works: typing shows, Enter queues, Shift+Tab switches the mode for the next tool.
    scenario([{"delay": 3.0, "tools": [{"name": "Write", "input": {"file_path": "/work/live.txt", "content": "ok"}}]}, {"text": "wrote"}, {"text": "queued answered"}])
    t.buf = b""
    t.send("write\r"); t.expect("esc to interrupt", 30)
    t.send("다음 질문"); t.expect("다음 질문", 10)
    check("guest: typing (Hangul) shows while Claude works", True)
    t.send("\r"); t.expect(r"\(queued\)", 10)
    t.send("\x1b[Z"); t.expect("accept edits on", 10)
    t.expect("queued answered", 60)
    check("guest: shift+tab mid-turn applied, queued message sent", open(os.path.join(work, "live.txt")).read() == "ok" and any(b.get("text") == "다음 질문" for b in requests()[-1]["body"]["messages"][-1]["content"]), requests()[-1]["body"]["messages"][-1])
    t.expect("accept edits on", 20)
    check("guest: the mode chosen mid-turn stays", True)
    t.send("\x1b[Z\x1b[Z"); t.read(0.5)

    t.send("/config\r"); out = t.expect("Keep transcripts", 15)
    t.send("\x1b[B" * 6 + " "); t.expect("Streaming saved", 10)
    streaming = json.load(open(os.path.join(work, ".claude/settings.local.json"))).get("stream")
    t.send("\x1b[3~"); t.expect("Streaming reset", 10); t.send("\x1b"); t.expect("shift\\+tab to cycle|for shortcuts", 10)
    check("guest: /config panel changes a setting", streaming is False and "Base URL" in plain(out), plain(out)[-500:])

    t.send("/status\r"); out = t.expect("Tools:", 10)
    check("guest: /status", "wasm32" in plain(out) and "claude-sonnet-5" in plain(out), plain(out)[-500:])

    t.send("\x04"); t.expect(r"# ", 20)
    check("guest: ctrl-d leaves claude", True)
finally:
    t.send(b"\x1dq"); time.sleep(0.5); t.close()
shutil.rmtree(work, ignore_errors=True)
print(f"\n{'ALL PASSED' if not fails else str(len(fails)) + ' FAILED'}")
sys.exit(1 if fails else 0)
