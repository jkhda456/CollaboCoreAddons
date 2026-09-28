# claude-code add-on

`claude` in the sandbox: Claude Code as a native wasm32 Linux program (Rust, `agent/`). It works on
the guest's files with Claude Code's tools, settings, permissions, hooks, sessions and commands,
and talks to either

- the **Anthropic Messages API** (default `claude-opus-5-5`; aliases `opus`, `sonnet`, `haiku`), or
  any gateway that speaks it, or
- an **OpenAI-compatible Chat Completions server**: a local LLM (Ollama, llama.cpp `llama-server`,
  vLLM, LM Studio, …) or a hosted one.

The Claude Code CLI itself is a closed Node/Bun binary for x64/arm64 hosts and cannot run on the
guest's WebAssembly kernel; this is its own code, following Claude Code's behaviour and names.

## What it does

| Area | |
|---|---|
| Tools | Task (sub-agents), Bash (+ `run_in_background`, BashOutput, KillShell), Read (text, images, notebooks), Write, Edit, MultiEdit, NotebookEdit, LS, Glob, Grep (`-A/-B/-C`, `-n`, `-i`, type, multiline), WebFetch, WebSearch (Anthropic's server tool), TodoWrite, ExitPlanMode, MCP tools (`mcp__server__tool`) |
| Turn processing | streaming answers, extended thinking (`think` / `think hard` / `ultrathink`, Tab, `alwaysThinkingEnabled`), `@file` attachments, read-before-edit checks, background-shell completion notices, auto-compaction near the context window and `/compact`, retries with backoff, Esc / Ctrl-C interruption of the answer or the running command |
| Permissions | modes `default`, `acceptEdits`, `plan`, `bypassPermissions` (Shift+Tab cycles); `permissions.{allow,ask,deny}` rules like `Bash(npm run:*)`, `Read(./src/**)`, `Edit(//etc/**)`, `WebFetch(domain:x)`, `mcp__server`; read-only commands run without asking; the dialog's "don't ask again" saves a rule to `.claude/settings.local.json`; `--allowedTools` / `--disallowedTools`; `additionalDirectories` / `--add-dir` |
| Settings | `~/.claude/settings.json`, `.claude/settings.json`, `.claude/settings.local.json`, `--settings`, plus what the app gave at start; `env`, `model`, `hooks`, `statusLine`, `outputStyle`, `cleanupPeriodDays`, `autoCompactEnabled`, `preferredNotifChannel`, … |
| Hooks | PreToolUse, PostToolUse, UserPromptSubmit, Stop, SubagentStop, SessionStart, SessionEnd, Notification, PreCompact; JSON on stdin; exit 2 blocks; JSON output (`decision`, `permissionDecision`, `additionalContext`, `updatedInput`, `continue`, `systemMessage`) |
| Memory | `CLAUDE.md` from `~/.claude` and each directory down to the working one (`AGENTS.md` where there is no `CLAUDE.md`), `CLAUDE.local.md`, `@path` imports; `#note` adds to one; `/memory` opens one in `$EDITOR`; `/init` writes one |
| Extensions | sub-agents in `.claude/agents/*.md` (+ built-in `general-purpose`, `Explore`); slash commands in `.claude/commands/**/*.md` (`$ARGUMENTS`, `$1`…, ``!`cmd` ``, `allowed-tools`, `model`); output styles (Default, Explanatory, Learning, `.claude/output-styles/*.md`); MCP servers (stdio in the guest, streamable HTTP through the host) from `.mcp.json`, settings or `--mcp-config` |
| Sessions | transcripts in `~/.claude/projects/…/<id>.jsonl`; `--continue`, `--resume [id]`, `/resume`, `/export` |
| Terminal | welcome box, bordered input box (multi-line, wide characters, history, bracketed paste, `/` command menu, `@` path completion, `!` bash mode, `#` memory mode, Esc Esc to clear, Ctrl-C twice to leave), the spinner (verb, time, tokens, the task in progress, a command's latest output), Markdown rendering as it streams, numbered colored diffs, todo checklists, permission and plan dialogs, status line, while Claude works the input box stays: type the next message (Enter queues it, sent when the turn ends), Shift+Tab switches the mode for the next tool call, Esc interrupts; terminal bell |
| Working longer | `/goal CONDITION` (Claude keeps going until a check says the condition holds), `/loop [interval] PROMPT`, checkpoints with `/rewind` or Esc Esc (restore the conversation, the code, or both), Ctrl+B to move a running command to the background, `/tasks` |
| Plugins | marketplaces (`/plugin marketplace add PATH-or-GIT-URL`), `/plugin install NAME@MARKETPLACE`, enable / disable / uninstall, `--plugin-dir`, `claude plugin …`; a plugin's commands (`/plugin:command`), agents, skills, hooks (with `if` rules and `asyncRewake`) and MCP servers. TypeScript "mods" need a JavaScript engine and are not supported |
| Skills | `.claude/skills/NAME/SKILL.md` (and `~/.claude/skills`, plugins): Claude loads one with the Skill tool when a request fits; `/NAME` loads one yourself; `/skills` lists them |
| Output | `-p` with `--output-format text|json|stream-json`, `--input-format stream-json`, `--verbose`, `--max-turns`; `claude mcp …`, `claude config …` |

`/config` opens the settings panel (↑↓ to move, Enter/Space/←→ to change, Delete to reset, Tab for the file changes go to: local, project or user); `/config show` prints the settings, `/config set KEY VALUE` sets one.

Keys: Ctrl+R searches the history, Ctrl+O shows verbose output, Ctrl+T the todo list, Ctrl+B backgrounds a running command, Esc Esc rewinds; `/vim` gives the input box vim keys; `/theme` picks colors; `/copy` copies the last answer (OSC 52).

Slash commands: `/add-dir /agents /bashes /clear /compact /config /context /cost /doctor /exit /export
/help /hooks /init /login /logout /mcp /memory /model /output-style /permissions /quit /release-notes
/resume /review /security-review /status /statusline /think /todos /usage`, and your own.

Not here, because the sandbox has no use for them: sign-in (the key stays on the host, see below),
IDE integration, auto-update, and GitHub app setup.

## How it reaches the model

Requests go through the host's request API (vsock port 1080, the one `hfetch` uses): the guest has
no TLS of its own for this; the host makes the HTTPS request under the sandbox's network policy,
adds the app's API key, and streams the answer back. So:

- **The key stays on the host.** Give it as the add-on's `apiKey` (or as a network `secret`);
  `claude` never sees it.
- **`localhost` means the host computer**, because the host makes the request. A model server on
  the computer running the app is `http://localhost:11434/v1` (Ollama), and needs
  `network.allowHostLoopback: true` (`--allow-loopback` on the engine's command line).
- The network policy applies: the model's host must be allowed.

The add-on lets the guest set the `anthropic-version`, `anthropic-beta`, `mcp-session-id` and
`mcp-protocol-version` headers (its `addon.json`), which the request API otherwise refuses.

## Settings from the app

The app gives them at start; the engine writes them to `/etc/collabo/addons/claude-code.json`, the
lowest settings layer:

| Key | Meaning | Default |
|---|---|---|
| `provider` | `anthropic` or `openai` | `anthropic` |
| `baseUrl` | the API's base URL | `https://api.anthropic.com` / `https://api.openai.com/v1` |
| `model` | the model (or an alias) | `claude-opus-5-5` / the first one the server lists |
| `apiKey` | becomes a host-side secret for `baseUrl`'s host (https); a plain-http `baseUrl` gets it in the guest | none |
| `maxTokens` | longest answer | 32000 / the server's default |
| `contextWindow` | when to compact | 200000 / 32768 |
| `stream` | streamed answers | `true` |
| `permissionMode` | `default`, `acceptEdits`, `plan`, `bypassPermissions` (or `allow` / `ask`) | `default` |
| `appendSystemPrompt`, `maxTurns`, `webSearch`, `alwaysThinkingEnabled`, `maxThinkingTokens` | | |

Any other Claude Code setting (`permissions`, `hooks`, `env`, `mcpServers`, …) can be given the
same way. The environment (`CLAUDE_CODE_PROVIDER`, `ANTHROPIC_BASE_URL`, `ANTHROPIC_MODEL`,
`ANTHROPIC_API_KEY`, `OPENAI_BASE_URL`, `OPENAI_MODEL`, `OPENAI_API_KEY`, `CLAUDE_CODE_MAX_TOKENS`,
`MAX_THINKING_TOKENS`) and `claude`'s options override the files. `claude --show-config` and
`/status` show what is in effect.

```dart
// Anthropic; the key is added by the host.
CollaboConfig(addons: {'claude-code': ClaudeCodeSettings.anthropic(apiKey: key).toJson()})

// A local model on this computer (Ollama).
CollaboConfig(
  addons: {'claude-code': ClaudeCodeSettings.openai(baseUrl: 'http://localhost:11434/v1', model: 'qwen3-coder').toJson()},
  network: NetworkPolicy(allowHostLoopback: true),
)
```

From the engine's command line:

```sh
cd dist/runtime/collabo-core-linux-arm64
bin/collabo-core-engine --kernel app/images/vmlinux.wasm \
  --initramfs app/images/initramfs.cpio --initramfs app/images/python.cpio \
  --addon-dir app/images/addons --allow-loopback \
  --addon-config claude-code:provider=openai \
  --addon-config claude-code:baseUrl=http://localhost:11434/v1 \
  --addon-config claude-code:model=qwen3-coder \
  --mount "$OLDPWD:/work"
# in the guest: cd /work && claude          (Ctrl-] then q leaves the engine)
```

## Using it

```sh
claude                                  # interactive
claude "fix the failing test"           # interactive, starting with that prompt
claude -c / claude -r [id]              # continue the last conversation / resume one
claude -p "summarize README.md"         # one answer on stdout
claude -p --output-format stream-json --verbose "…"
claude -p --allowedTools "Bash(npm test:*) Edit" "make the tests pass"
claude mcp add fs -- python3 -m my_mcp_server       claude mcp list
claude config set model sonnet
```

From the app: `sandbox.run('claude -p --permission-mode acceptEdits "…"', cwd: '/work')`. In `-p`
nobody can answer a permission question, so tools that need one are refused (and listed in the
JSON result's `permission_denials`) unless rules, `--allowedTools` or the mode allow them.

## Building and testing

`./build.sh addons` (after `userspace` and `python`, which builds the guest's Rust toolchain), or
`addons/claude-code/build.sh`; `TEST=1` runs the unit tests natively first. Output:
`out/claude-code.cpio` (`/usr/bin/claude`) and `out/licenses/crates.txt`.

`tests/run.sh` runs the checks against a scripted model (`tests/mock_llm.py`): the unit tests,
`-p` scenarios and the interactive UI in a pseudo-terminal with a native build, then the same in
the guest through the packaged runtime (`tests/run.sh native` for the first half only).

| File | |
|---|---|
| `agent/src/main.rs` | command line, `claude mcp` / `claude config`, `-p` output |
| `agent/src/agent.rs` | a session: the turn loop, permissions, hooks, sub-agents, compaction |
| `agent/src/repl.rs` | the interactive session and the slash commands |
| `agent/src/ui/` | the terminal (`terminal.rs`), the input box (`editor.rs`), the spinner, `-p` output |
| `agent/src/tools/` | the built-in tools |
| `agent/src/llm.rs` | the two APIs (streamed and not); the conversation is kept as Messages API blocks |
| `agent/src/settings.rs`, `config.rs`, `permissions.rs`, `hooks.rs` | settings layers, the connection, rules and modes, hooks |
| `agent/src/prompt.rs`, `extensions.rs` | the system prompt and memory; agents, commands, output styles |
| `agent/src/session.rs`, `mcp.rs` | transcripts; MCP servers |
| `agent/src/render.rs`, `term.rs`, `interrupt.rs` | Markdown and diffs; the terminal; Esc / Ctrl-C |
| `agent/src/bridge.rs`, `sse.rs` | HTTP through the host (vsock 1080); server-sent events |
