"""The official Claude Code's terminal UI in the guest: the engine in a pseudo-terminal, Claude Code
installed with npm, a prompt and its streamed answer, a Bash tool call through the permission
dialog, and /exit back to the guest's shell."""
import json
import os
import re
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ptydrive  # noqa: E402
from guest import CLAUDE_CODE_TGZ, ENGINE, MOCK_URL, check, engine_options, fails  # noqa: E402
from ptydrive import squeezed  # noqa: E402

KEY = "sk-ant-node-tty-0456"
ENV = {"DISABLE_TELEMETRY": "1", "DISABLE_AUTOUPDATER": "1", "DISABLE_ERROR_REPORTING": "1",
       "CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC": "1"}

if not os.path.exists(CLAUDE_CODE_TGZ):
    check("Claude Code tarball (tests/run.sh downloads it)", False, CLAUDE_CODE_TGZ)
    sys.exit(1)

work = tempfile.mkdtemp(prefix="node-tty-")
shutil.copy(CLAUDE_CODE_TGZ, os.path.join(work, "claude-code.tgz"))
os.makedirs(os.path.join(work, "proj"))
os.makedirs(os.path.join(work, "home"))
# A user who has been through the first-run screens and trusts the folder; the key is approved.
with open(os.path.join(work, "home/.claude.json"), "w") as f:
    json.dump({"hasCompletedOnboarding": True, "theme": "dark", "numStartups": 1,
               "customApiKeyResponses": {"approved": [KEY[-20:]], "rejected": []},
               "projects": {"/work/proj": {"hasTrustDialogAccepted": True}}}, f)


def wait_for(t, text, timeout):
    """Until the screen output, squeezed (the UI moves the cursor between words), has `text`."""
    target = re.sub(r"\s+", "", text)
    end = time.time() + timeout
    while time.time() < end:
        if target in squeezed(t.all.decode(errors="replace")):
            return True
        t.read(0.3)
    return False


def since(t, mark):
    return squeezed(t.all[mark:].decode(errors="replace"))


t = ptydrive.Session([ENGINE, *engine_options(work, {"baseUrl": MOCK_URL, "apiKey": KEY, "env": json.dumps(ENV)})],
                     cols=120, rows=40, env={"TERM": "xterm-256color"})
try:
    t.expect(r"root@collabo:\S*# ", 180)
    t.send("npm install -g --offline --omit=optional --no-audit --no-fund /work/claude-code.tgz >/dev/null 2>&1;"
           " cd /work/proj && HOME=/work/home claude\r")
    check("tty: Claude Code starts (its input box)", wait_for(t, "for shortcuts", 300), since(t, 0)[-1500:])
    t.read(1.0)

    mark = len(t.all)
    t.send("say hello")
    t.read(0.5)
    t.send("\r")
    check("tty: a prompt and its streamed answer",
          wait_for(t, "Hello from the mock model! You said: say hello", 120), since(t, mark)[-1500:])
    t.read(1.0)

    # a command that writes asks first (read-only ones such as echo run without asking)
    mark = len(t.all)
    t.send("run touch made-in-the-guest.txt && echo tty-$((40+2))")
    t.read(0.5)
    t.send("\r")
    asked = wait_for(t, "Do you want to proceed?", 120)
    check("tty: the permission dialog for a Bash command", asked, since(t, mark)[-1500:])
    t.read(0.5)
    t.send("\r")  # 1. Yes
    check("tty: the command ran in the guest on the host's folder, its output in the answer",
          wait_for(t, "tty-42", 120) and os.path.exists(os.path.join(work, "proj/made-in-the-guest.txt")),
          since(t, mark)[-1500:])
    t.read(1.0)

    mark = len(t.all)
    t.send("/exit")
    t.read(0.5)
    t.send("\r")
    back = False
    end = time.time() + 60
    while time.time() < end and not back:
        t.read(0.3)
        back = re.search(r"root@collabo:/work/proj# ", t.all[mark:].decode(errors="replace")) is not None
    check("tty: /exit returns to the guest's shell", back, since(t, mark)[-800:])
finally:
    t.close()
    shutil.rmtree(work, ignore_errors=True)

print(f"{len(fails)} failed" if fails else "all passed")
sys.exit(1 if fails else 0)
