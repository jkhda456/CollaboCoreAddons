//! The interactive session: the welcome, the input box, and what a line can be — a prompt, a
//! slash command (built in or from .claude/commands), "!command" run directly, or "#note" added to
//! memory.
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

use serde_json::{json, Value};

use crate::agent::{Session, Ui};
use crate::extensions;
use crate::permissions::Mode;
use crate::session;
use crate::settings::Scope;
use crate::term;
use crate::ui::editor::{self, Editor, Read};
use crate::ui::spinner;
use crate::ui::terminal::{TerminalUi, Watcher};
use crate::util;

pub const VERSION: &str = env!("CARGO_PKG_VERSION");
/// Said wherever the program names itself: this is not Anthropic's Claude Code.
pub const EDITION: &str = "WASM native port for CollaboCore";

const BUILTIN: &[(&str, &str)] = &[
    ("add-dir", "Add a new working directory"),
    ("agents", "List the sub-agents Task can use"),
    ("bashes", "List and kill background shells (/bashes kill ID)"),
    ("clear", "Clear conversation history and free up context"),
    ("compact", "Clear conversation history but keep a summary in context. Optional: /compact [instructions for summarization]"),
    ("config", "Open the settings panel (/config show prints them, /config set KEY VALUE sets one)"),
    ("context", "Visualize current context usage"),
    ("copy", "Copy Claude's last answer to the clipboard (OSC 52) and to a file"),
    ("cost", "Show the tokens used and the time of the current session"),
    ("doctor", "Check the setup: model connection, settings, hooks, MCP servers"),
    ("exit", "Exit the REPL"),
    ("export", "Export the current conversation to a file"),
    ("goal", "Set a goal Claude works toward until it holds (/goal clear to drop it)"),
    ("help", "Show help and available commands"),
    ("hooks", "Show the hooks the settings configure"),
    ("init", "Initialize a new CLAUDE.md file with codebase documentation"),
    ("login", "How the API key is given in this sandbox"),
    ("loop", "Run a prompt again and again: /loop [30s|5m|1h] PROMPT (esc stops)"),
    ("logout", "How the API key is given in this sandbox"),
    ("mcp", "Show MCP servers and their tools"),
    ("memory", "Edit Claude memory files"),
    ("model", "Set the AI model for Claude Code"),
    ("output-style", "Set the output style (default, Explanatory, Learning, or your own)"),
    ("permissions", "Manage allow, ask & deny tool permission rules"),
    ("plugin", "Manage plugins: list, marketplace add, install, enable, disable, uninstall"),
    ("quit", "Exit the REPL"),
    ("release-notes", "What this version can do"),
    ("rename", "Name this conversation (shown by /resume)"),
    ("resume", "Resume a conversation"),
    ("review", "Review the current changes"),
    ("rewind", "Go back to an earlier message: the conversation, the code, or both (also esc esc)"),
    ("security-review", "Complete a security review of the pending changes"),
    ("skills", "List the skills Claude can load (.claude/skills)"),
    ("status", "Show the version, model, connection and tools"),
    ("tasks", "Background tasks: running shells and their output (/tasks kill ID)"),
    ("think", "Turn extended thinking on or off (also Tab)"),
    ("todos", "List the current todo items"),
    ("usage", "Show the tokens used (same as /cost)"),
    ("vim", "Toggle vim keys in the input box"),
    ("theme", "Change the color theme"),
    ("code-review", "Review the pending changes for bugs, with sub-agents"),
    ("simplify", "Clean up the changed code: reuse, simplification, efficiency"),
    ("terminal-setup", "How to make Shift+Enter insert a newline in your terminal"),
    ("statusline", "Set up a status line above the input (describe it, or let Claude pick)"),
    ("agents-new", "Create a sub-agent: /agents-new WHAT IT SHOULD DO"),
];

const INIT_PROMPT: &str = "Please analyze this codebase and create a CLAUDE.md file, which will be given to future instances of Claude Code to operate in this repository.

What to add:
1. Commands that will be commonly used, such as how to build, lint, and run tests. Include the necessary commands to develop in this codebase, such as how to run a single test.
2. High-level code architecture and structure so that future instances can be productive more quickly. Focus on the \"big picture\" architecture that requires reading multiple files to understand.

Usage notes:
- If there's already a CLAUDE.md, suggest improvements to it.
- When you make the initial CLAUDE.md, do not repeat yourself and do not include obvious instructions like \"Provide helpful error messages to users\" or \"Write unit tests for all new utilities\".
- Avoid listing every component or file structure that can be easily discovered.
- Don't include generic development practices.
- If there are Cursor rules (.cursor/rules/ or .cursorrules), Copilot rules (.github/copilot-instructions.md) or an AGENTS.md, include the important parts.
- If there is a README.md, include the important parts.
- Do not make up information such as \"Common Development Tasks\" unless it is expressly included in other files that you read.
- Be sure to prefix the file with the following text:

```
# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.
```";

const REVIEW_PROMPT: &str = "You are an expert code reviewer. Review the current changes in this repository:
1. Run `git status` and `git diff HEAD` (or `git diff` if there is no commit yet) to see what changed.
2. Read the changed files where the diff alone does not show enough.
3. Report, most important first: correctness bugs, security problems, missed edge cases, then code quality and consistency with the codebase's conventions. Be specific: file_path:line, what is wrong, and how to fix it. Skip nitpicks that do not matter.
If there are no changes, say so.";

const CODE_REVIEW_PROMPT: &str = "Review the pending changes in this repository for bugs. Work like this:
1. Get the changes: `git status`, `git diff HEAD` (or `git diff` without commits). If there are none, say so and stop.
2. Launch several sub-agents with the Task tool (one answer, several calls), each reviewing the whole diff from one angle: (a) correctness bugs and edge cases, (b) the project's own rules in CLAUDE.md and its conventions, (c) error handling, resources and concurrency, (d) security. Give each the diff's file list and what to look for; each reports findings with file:line and why.
3. Check every finding yourself against the code: keep only real problems that would cause wrong behaviour, drop style nits and false positives.
4. Report the confirmed findings, most severe first: file:line, the problem, a concrete failure scenario, and the fix. If none survive, say the changes look correct.";

const SIMPLIFY_PROMPT: &str = "Clean up the code changed in this repository (`git diff HEAD`, or the files the user names), without changing behaviour:
- reuse: code that duplicates helpers or patterns the codebase already has → use those
- simplification: needless indirection, dead code, over-general structures, long conditionals → simpler equivalents
- efficiency: repeated work, wasted allocations or passes → cheaper equivalents
- fit: names, comments and structure like the surrounding code
Read the surrounding code first. Make the changes with Edit, keep each one small and safe, then run the tests or the build if the project has them. Finish with a short list of what changed and why.";

const STATUSLINE_PROMPT: &str = "Set up a status line for Claude Code in this sandbox. The settings key is \"statusLine\": {\"type\": \"command\", \"command\": \"…\"}; the command gets the session as JSON on stdin (session_id, cwd, model.id, workspace.current_dir, workspace.project_dir, version, permission_mode) and its first line of output is shown above the input box. It runs in /bin/sh (busybox), so no jq: use sed or a small script (python3 if it is there). Unless the user said otherwise, show the current directory's name, the git branch if any, and the model. Write the script to ~/.claude/statusline.sh (chmod +x) and the setting to ~/.claude/settings.json (keep the file's other keys), test it by piping a sample JSON to it, then say what it shows.";

const AGENT_PROMPT: &str = "Create a Claude Code sub-agent from the user's description. Write it as .claude/agents/NAME.md (NAME: short, lowercase, hyphens) with this front matter:
---
name: NAME
description: when to use this agent, written so the main agent can decide (\"Use this agent when …\"), with an example or two
tools: a comma list of only the tools it needs (Read, Grep, Glob, LS, Bash, Edit, Write, WebFetch, …), or leave the line out for all
model: inherit (or sonnet / opus / haiku)
---
Then the system prompt: its role, how it works step by step, what to check, and what its final report must contain. Keep it focused on one job. After writing it, say how to use it (the main agent picks it by its description, or ask for it by name).";

const TERMINAL_SETUP: &str = "Most terminals send Shift+Enter as plain Enter, so it submits. Newlines that always work here: \\ then Enter, Alt+Enter (Option+Enter), Ctrl+J, or pasting.

To make Shift+Enter insert a newline:
- iTerm2: Settings → Profiles → Keys → Key Mappings → + : Shortcut Shift+Enter, Action \"Send Text\", text \"\\\\\\r\" (a backslash and a return) — or \"Send Escape Sequence\" [13;2u
- WezTerm: keys = { { key = 'Enter', mods = 'SHIFT', action = wezterm.action.SendString '\\x1b[13;2u' } }
- kitty: map shift+enter send_text all \\x1b[13;2u
- Alacritty: [[keyboard.bindings]] key = \"Return\", mods = \"Shift\", chars = \"\\u001b[13;2u\"
- Ghostty: keybind = shift+enter=text:\\x1b[13;2u
- VS Code terminal: a keybinding for shift+enter with \"workbench.action.terminal.sendSequence\" and text \"\\u001b[13;2u\"
- macOS Terminal: no per-key text; use Option+Enter with \"Use Option as Meta key\" (Settings → Profiles → Keyboard).
Through the collaboCore engine the keys pass straight to the sandbox, so the terminal's setting is all it takes.";

const SECURITY_PROMPT: &str = "Complete a security review of the pending changes in this repository (`git diff HEAD`, or the files the user names). Look for vulnerabilities an attacker could actually exploit: injection (SQL, command, path traversal), authentication and authorization flaws, secrets in code, unsafe deserialization, SSRF, XSS, insecure cryptography, and data exposure. For each finding give file_path:line, severity (high/medium/low), how it could be exploited, and the fix. Leave out theoretical issues with no realistic exploit path. If nothing is found, say so.";

struct Host<'a> {
    session: &'a mut Session,
    commands: Vec<(String, String)>,
}

impl editor::Host for Host<'_> {
    fn commands(&self) -> Vec<(String, String)> {
        self.commands.clone()
    }
    fn cycle_mode(&mut self) {
        self.session.mode = self.session.mode.next(self.session.bypass_available);
    }
    fn toggle_thinking(&mut self) {
        self.session.thinking_on = !self.session.thinking_on;
    }
    fn footer(&mut self) -> (String, String) {
        let left = match self.session.mode.banner() {
            Some(banner) => {
                let text = format!("{banner} (shift+tab to cycle)");
                match self.session.mode {
                    Mode::Plan => term::cyan(&text),
                    Mode::Bypass => term::red(&text),
                    _ => term::magenta(&text),
                }
            }
            None => term::dim("? for shortcuts"),
        };
        let mut right = Vec::new();
        let running = self.session.context.shells.running();
        if running > 0 {
            right.push(term::cyan(&format!("{running} background task{}", if running == 1 { "" } else { "s" })));
        }
        let window = self.session.config.context_window.max(1);
        let left_percent = 100u64.saturating_sub(self.session.context_tokens * 100 / window);
        if left_percent < 30 {
            right.push(term::dim(&format!("Context left until auto-compact: {}%", left_percent.saturating_sub(15))));
        }
        if crate::ui::terminal::verbose() {
            right.push(term::dim("verbose output on (ctrl+o)"));
        }
        if let Some(goal) = &self.session.goal {
            right.push(term::cyan(&format!("◎ {}", term::truncate(goal, 30))));
        }
        if self.session.thinking_on || self.session.config.always_thinking {
            right.push(term::accent("✻ Thinking on"));
        }
        (left, right.join("  "))
    }
    fn cwd(&self) -> PathBuf {
        self.session.context.cwd.clone()
    }
    fn vim(&self) -> bool {
        self.session.settings.str("/editorMode").as_deref() == Some("vim")
    }
}

fn welcome(session: &Session) {
    let columns = term::columns().clamp(40, 78);
    let cwd = session.context.cwd.display().to_string();
    let lines = [
        format!("{} {}", term::accent("✻"), term::bold("Welcome to Claude Code!")),
        term::dim(&format!("  {EDITION} · v{VERSION}")),
        String::new(),
        term::dim(&format!("  /help for help, /status for your current setup")),
        String::new(),
        term::dim(&format!("  cwd: {cwd}")),
        term::dim(&format!("  model: {} ({})", session.model, session.config.provider.name())),
    ];
    let mut out = format!("{}\n", term::accent(&format!("╭{}╮", "─".repeat(columns - 2))));
    for line in lines {
        let pad = (columns - 4).saturating_sub(term::width(&line));
        out.push_str(&format!("{} {line}{} {}\n", term::accent("│"), " ".repeat(pad), term::accent("│")));
    }
    out.push_str(&format!("{}\n\n", term::accent(&format!("╰{}╯", "─".repeat(columns - 2)))));
    if session.messages.is_empty() {
        out.push_str(&term::dim(" Tips for getting started:\n\n 1. Ask Claude to create a new app or clone a repository\n 2. Use Claude to help with file analysis, editing, bash commands and git\n 3. Be as specific as you would with another engineer for the best results\n"));
        if !session.context.cwd.join("CLAUDE.md").exists() {
            out.push_str(&term::dim(" 4. Run /init to create a CLAUDE.md file with instructions for Claude\n"));
        }
        out.push('\n');
    }
    term::out(&out);
}

/// The settings' statusLine command's first line, shown above the input.
fn status_line(session: &Session) -> Option<String> {
    let command = session.settings.get("/statusLine/command").and_then(Value::as_str)?;
    let input = json!({
        "session_id": session.id,
        "cwd": session.context.cwd.display().to_string(),
        "model": {"id": session.model, "display_name": session.model},
        "workspace": {"current_dir": session.context.cwd.display().to_string(), "project_dir": session.settings.project_root.display().to_string()},
        "version": VERSION,
        "permission_mode": session.mode.name(),
    });
    let file = std::env::temp_dir().join(format!(".claude-status-{}", std::process::id()));
    std::fs::write(&file, input.to_string()).ok()?;
    let output = std::process::Command::new("/bin/sh").arg("-c").arg(command).stdin(std::fs::File::open(&file).ok()?).stderr(std::process::Stdio::null()).output().ok();
    let _ = std::fs::remove_file(&file);
    let text = String::from_utf8_lossy(&output?.stdout).lines().next()?.to_string();
    Some(text).filter(|t| !t.trim().is_empty())
}

fn show_user_line(text: &str) {
    let lines: Vec<&str> = text.lines().collect();
    let mut out = String::new();
    for (index, line) in lines.iter().enumerate() {
        out.push_str(&format!("{}{}\n", term::dim(if index == 0 { "> " } else { "  " }), term::dim(line)));
    }
    term::out(&out);
}

/// Runs a prompt, then the messages queued while it ran (unless it was interrupted: they go
/// back to the input box). What the user was still typing waits in `ui.draft`.
fn run_prompt(session: &mut Session, ui: &mut TerminalUi, editor: &mut Editor, text: &str) {
    let mut next = Some(text.to_string());
    while let Some(prompt) = next.take() {
        spinner::live_begin(session.mode, session.bypass_available);
        ui.was_interrupted = false;
        ui.watcher = Some(Watcher::start(editor.keys.take_pending()));
        let started = Instant::now();
        let result = session.prompt(&prompt, ui);
        ui.end_turn();
        editor.keys.give(std::mem::take(&mut ui.leftover));
        if let Some((queued, draft, mode)) = spinner::live_end() {
            session.mode = mode;
            let queued = queued.join("\n\n");
            match (ui.was_interrupted, queued.is_empty()) {
                (_, true) => ui.draft = draft,
                (true, false) => ui.draft = if draft.is_empty() { queued } else { format!("{queued}\n{draft}") },
                (false, false) => {
                    ui.draft = draft;
                    next = Some(queued);
                }
            }
        }
        if let Err(message) = result {
            term::out(&format!("{}\n", term::red(&format!("  ⎿  API Error: {message}"))));
        }
        if started.elapsed() > Duration::from_secs(15) && session.settings.str("/preferredNotifChannel").as_deref() == Some("terminal_bell") {
            term::raw("\x07");
        }
        term::out("\n");
        if let Some(queued) = &next {
            show_user_line(queued);
        }
    }
}

pub fn run(session: &mut Session, first: Option<String>) {
    crate::interrupt::install();
    let mut ui = TerminalUi::new(&session.context.cwd);
    crate::ui::terminal::VERBOSE.store(session.settings.bool("/verbose").unwrap_or(false), std::sync::atomic::Ordering::Relaxed);
    term::set_theme(&session.settings.str("/theme").unwrap_or_else(|| "dark".into()));
    let history = crate::settings::user_dir().join("history.jsonl");
    let mut editor = Editor::new(Some(history));
    welcome(session);
    if !session.messages.is_empty() {
        replay(session);
    }
    let mut pending = first.filter(|t| !t.trim().is_empty());
    let mut last_cancel: Option<Instant> = None;
    loop {
        session.context.cwd = session.context.cwd.clone();
        ui.cwd = session.context.cwd.clone();
        let line = match pending.take() {
            Some(text) => text,
            None => {
                if let Some(status) = status_line(session) {
                    term::out(&format!("{}\n", term::dim(&status)));
                }
                let custom = extensions::commands(&session.settings.project_root);
                let mut commands: Vec<(String, String)> = BUILTIN.iter().map(|(n, d)| (format!("/{n}"), d.to_string())).collect();
                commands.extend(host_skills(session));
                commands.extend(custom.iter().map(|c| {
                    let hint = if c.argument_hint.is_empty() { String::new() } else { format!(" {}", c.argument_hint) };
                    (format!("/{}", c.name), format!("{}{hint}", c.description))
                }));
                commands.sort();
                let mut host = Host { session, commands };
                let draft = std::mem::take(&mut ui.draft);
                match editor.read(&mut host, &draft) {
                    Read::Line(line) => {
                        last_cancel = None;
                        line
                    }
                    Read::Cancel => {
                        if last_cancel.is_some_and(|t| t.elapsed() < Duration::from_secs(2)) {
                            break;
                        }
                        last_cancel = Some(Instant::now());
                        term::out(&format!("{}\n", term::dim("  Press Ctrl-C again to exit")));
                        continue;
                    }
                    Read::Eof => break,
                    Read::Rewind => {
                        rewind(session, &mut ui);
                        continue;
                    }
                }
            }
        };
        let text = line.trim_end().to_string();
        if text.trim().is_empty() {
            continue;
        }
        show_user_line(&text);
        if let Some(command) = text.strip_prefix('!') {
            bash_mode(session, &mut ui, command.trim());
            continue;
        }
        if let Some(note) = text.strip_prefix('#') {
            memorize(session, &mut ui, note.trim());
            continue;
        }
        if let Some(command) = text.strip_prefix('/') {
            let (name, args) = command.split_once(char::is_whitespace).map_or((command, ""), |(n, a)| (n, a.trim()));
            match slash(session, &mut ui, &mut editor, name, args) {
                Flow::Continue => continue,
                Flow::Exit => break,
                Flow::Prompt(prompt) => {
                    run_prompt(session, &mut ui, &mut editor, &prompt);
                    continue;
                }
            }
        }
        run_prompt(session, &mut ui, &mut editor, &text);
    }
    spinner::stop();
    let cwd = session.context.cwd.clone();
    session.hooks.run("SessionEnd", None, &cwd, json!({"reason": "prompt_input_exit"}));
    if session.transcript.is_some() && !session.messages.is_empty() {
        term::out(&term::dim(&format!("\nResume this session with: claude --resume {}\n", session.id)));
    }
}

/// The conversation so far, briefly (after --resume/--continue).
fn replay(session: &Session) {
    let mut out = String::new();
    for message in session.messages.iter().rev().take(6).collect::<Vec<_>>().into_iter().rev() {
        let texts: Vec<String> = match &message["content"] {
            Value::String(text) => vec![text.clone()],
            Value::Array(blocks) => blocks.iter().filter(|b| b["type"] == "text").filter_map(|b| b["text"].as_str().map(str::to_string)).filter(|t| !t.starts_with("<system-reminder>")).collect(),
            _ => Vec::new(),
        };
        for text in texts {
            let first = term::truncate(text.lines().next().unwrap_or(""), term::columns() - 4);
            out.push_str(&match message["role"].as_str() {
                Some("assistant") => format!("⏺ {first}\n"),
                _ => format!("{}\n", term::dim(&format!("> {first}"))),
            });
        }
    }
    out.push_str(&format!("{}\n\n", term::dim(&format!("  (resumed: {} messages)", session.messages.len()))));
    term::out(&out);
}

fn bash_mode(session: &mut Session, ui: &mut TerminalUi, command: &str) {
    if command.is_empty() {
        return;
    }
    crate::interrupt::clear();
    let watcher = Watcher::start(Vec::new());
    let outcome = crate::tools::bash::bash(&json!({"command": command}), &mut session.context);
    let _ = watcher.finish();
    let _ = ui;
    let color: fn(&str) -> String = if outcome.is_error { term::red } else { term::dim };
    let mut out = String::new();
    for (index, line) in outcome.text.lines().take(40).enumerate() {
        out.push_str(&format!("{}{}\n", term::dim(if index == 0 { "  ⎿  " } else { "     " }), color(line)));
    }
    term::out(&out);
    // The model sees it with the next prompt.
    let block = json!({"type": "text", "text": format!("<bash-input>{command}</bash-input>\n<bash-stdout>{}</bash-stdout><bash-stderr></bash-stderr>", outcome.text)});
    struct Silent;
    impl Ui for Silent {
        fn text(&mut self, _: &str) {}
        fn notice(&mut self, _: &str) {}
        fn error(&mut self, _: &str) {}
        fn ask(&mut self, _: &str, _: &Value, _: &str, _: &str) -> crate::agent::Approval {
            crate::agent::Approval::No(None)
        }
        fn approve_plan(&mut self, _: &str) -> crate::agent::PlanAnswer {
            crate::agent::PlanAnswer::KeepPlanning(None)
        }
    }
    session.push_user(vec![block], &mut Silent);
}

fn append_line(path: &Path, line: &str) -> std::io::Result<()> {
    use std::io::Write;
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir)?;
    }
    let mut existing = std::fs::read_to_string(path).unwrap_or_default();
    if !existing.is_empty() && !existing.ends_with('\n') {
        existing.push('\n');
    }
    let mut file = std::fs::File::create(path)?;
    write!(file, "{existing}{line}\n")
}

fn memorize(session: &mut Session, ui: &mut TerminalUi, note: &str) {
    if note.is_empty() {
        return;
    }
    let project = session.settings.project_root.join("CLAUDE.md");
    let user = crate::settings::user_dir().join("CLAUDE.md");
    let local = session.settings.project_root.join("CLAUDE.local.md");
    let options = [
        format!("Project memory   Checked in at {}", project.display()),
        format!("Project memory (local)   Gitignored, at {}", local.display()),
        format!("User memory   Saved in {}", user.display()),
    ];
    let Some(choice) = ui_choose(ui, "Where should this memory be saved?", &[format!("  {note}")], &options) else { return };
    let path = [project, local, user][choice].clone();
    match append_line(&path, &format!("- {note}")) {
        Ok(()) => term::out(&format!("{}\n\n", term::dim(&format!("  ⎿  Got it. Added to {}", path.display())))),
        Err(error) => ui.error(&format!("{}: {error}", path.display())),
    }
}

fn ui_choose(ui: &mut TerminalUi, title: &str, body: &[String], options: &[String]) -> Option<usize> {
    ui.choose_public(title, body, "", options)
}

enum Flow {
    Continue,
    Exit,
    Prompt(String),
}

fn say(text: &str) {
    let mut out = String::new();
    for (index, line) in text.lines().enumerate() {
        out.push_str(&format!("{}{line}\n", term::dim(if index == 0 { "  ⎿  " } else { "     " })));
    }
    term::out(&(out + "\n"));
}

fn slash(session: &mut Session, ui: &mut TerminalUi, editor: &mut Editor, name: &str, args: &str) -> Flow {
    match name {
        "exit" | "quit" => return Flow::Exit,
        "help" => {
            let custom = extensions::commands(&session.settings.project_root);
            let mut text = format!("{}\n\nAlways review Claude's responses, especially when running code.\n\n", term::bold(&format!("Claude Code v{VERSION} ({EDITION})")));
            text.push_str(&term::bold("Usage modes:\n"));
            text.push_str("  REPL: claude (interactive session)\n  Non-interactive: claude -p \"question\"\n\n");
            text.push_str(&term::bold("Common tasks:\n"));
            text.push_str("  Ask questions about your codebase    > How does foo.py work?\n  Edit files                           > Update bar.ts to...\n  Fix errors                           > cargo build\n  Run commands                         > !ls\n  Add to memory                        > #Use 2 spaces for indentation\n\n");
            text.push_str(&term::bold("Interactive mode commands:\n"));
            for (command, description) in BUILTIN {
                text.push_str(&format!("  /{command:<16} {description}\n"));
            }
            if !custom.is_empty() {
                text.push_str(&term::bold("\nCustom commands:\n"));
                for command in custom {
                    text.push_str(&format!("  /{:<16} {}\n", command.name, command.description));
                }
            }
            text.push_str("\nShortcuts: ! bash mode · # memorize · @ file paths · shift+tab modes · tab thinking · esc interrupt · esc esc clear · \\⏎ newline · ↑↓ history\n");
            term::out(&format!("{text}\n"));
        }
        "clear" => {
            session.clear(ui);
            term::raw("\x1b[2J\x1b[H");
            term::set_line_start(true);
            welcome(session);
        }
        "compact" => {
            ui.watcher = Some(Watcher::start(editor.keys.take_pending()));
            spinner::start();
            let result = session.compact(Some(args).filter(|a| !a.is_empty()), false, ui);
            ui.end_turn();
            editor.keys.give(std::mem::take(&mut ui.leftover));
            match result {
                Ok(()) => say(&format!("Compacted. The conversation is now a summary (/context to see the space it takes).")),
                Err(message) => ui.error(&format!("Error compacting conversation: {message}")),
            }
        }
        "context" => say(&context_report(session)),
        "cost" | "usage" => say(&cost_report(session)),
        "status" => say(&status_report(session)),
        "doctor" => say(&doctor_report(session)),
        "model" => {
            if args.is_empty() {
                let mut options: Vec<String> = Vec::new();
                let mut ids: Vec<String> = Vec::new();
                match session.config.provider {
                    crate::config::Provider::Anthropic => {
                        for (alias, id, description) in crate::config::ALIASES {
                            options.push(format!("{alias}   {description} ({id})"));
                            ids.push(id.to_string());
                        }
                    }
                    crate::config::Provider::OpenAi => {
                        for id in crate::llm::list_models(&session.config).unwrap_or_default().into_iter().take(9) {
                            options.push(id.clone());
                            ids.push(id);
                        }
                    }
                }
                if options.is_empty() {
                    say(&format!("Model: {} (set one with /model NAME)", session.model));
                } else if let Some(choice) = ui_choose(ui, &format!("Select model (now: {})", session.model), &[], &options) {
                    session.model = ids[choice].clone();
                    say(&format!("Set model to {}", term::bold(&session.model)));
                }
            } else {
                session.model = crate::config::resolve_model(session.config.provider, args);
                say(&format!("Set model to {}", term::bold(&session.model)));
            }
        }
        "goal" => match args {
            "" => say(&match &session.goal {
                Some(goal) => format!("◎ Goal: {goal}\n(/goal clear drops it)"),
                None => "No goal set. /goal CONDITION: Claude works until the condition holds, checked each time it would stop.".into(),
            }),
            "clear" | "off" | "none" | "stop" => {
                let had = session.goal.take();
                say(&match had {
                    Some(goal) => format!("Goal cleared: {goal}"),
                    None => "No goal was set".into(),
                });
            }
            condition => {
                session.goal = Some(condition.to_string());
                say(&format!("◎ Goal set: {condition}"));
                return Flow::Prompt(format!("Work toward this goal until it is achieved: {condition}"));
            }
        },
        "loop" => {
            let (every, prompt) = match args.split_once(char::is_whitespace).map(|(first, rest)| (parse_interval(first), rest.trim())) {
                Some((Some(every), rest)) => (every, rest.to_string()),
                _ => (Duration::from_secs(600), args.to_string()),
            };
            if prompt.is_empty() {
                say("Usage: /loop [30s|5m|1h] PROMPT — runs PROMPT, waits, runs it again; esc while waiting stops, enter runs it now");
                return Flow::Continue;
            }
            say(&format!("⟳ Looping every {}: {prompt}", util::duration(every)));
            let mut rounds = 0;
            loop {
                rounds += 1;
                run_prompt(session, ui, editor, &prompt);
                if ui.was_interrupted || !wait_for_next(ui, every) {
                    break;
                }
                show_user_line(&prompt);
            }
            say(&format!("⟳ Loop stopped after {rounds} run{}", if rounds == 1 { "" } else { "s" }));
        }
        "skills" => {
            if session.skills.is_empty() {
                say("No skills. Add one as .claude/skills/NAME/SKILL.md (project) or ~/.claude/skills/NAME/SKILL.md (you): front matter name and description, then the instructions; scripts and references can sit beside it.");
            } else {
                let mut text = String::new();
                for skill in &session.skills {
                    text.push_str(&format!("{} ({})  {}\n    {}\n", term::bold(&skill.name), skill.source, term::truncate(&skill.description, 80), term::dim(&skill.dir.display().to_string())));
                }
                text.push_str(&term::dim("Claude loads a skill when a request fits it; /NAME loads one yourself."));
                say(&text);
            }
        }
        "rewind" | "checkpoint" => rewind(session, ui),
        "plugin" | "plugins" => {
            let words: Vec<&str> = args.split_whitespace().collect();
            let cwd = session.context.cwd.clone();
            let dirs = session.plugin_dirs.clone();
            let result = crate::plugins::command(&words, &mut session.settings, &cwd, &dirs);
            if words.first().is_some_and(|w| matches!(*w, "install" | "uninstall" | "enable" | "disable")) {
                let _ = session.reload_settings();
            }
            match result {
                Ok(text) => say(&text),
                Err(message) => ui.error(&message),
            }
        }
        "vim" => {
            let on = session.settings.str("/editorMode").as_deref() != Some("vim");
            let result = session.settings.set_value(Scope::User, "/editorMode", Some(json!(if on { "vim" } else { "normal" })));
            say(&match result {
                Ok(_) => format!("Editor mode set to {} (Esc for normal mode, i to insert)", if on { "vim" } else { "normal" }),
                Err(message) => message,
            });
        }
        "theme" => {
            let options: Vec<String> = term::THEMES.iter().map(|(name, about, accent, removed, added, _)| {
                format!("{name:<18} {about}   {}", [term::paint(accent, "accent"), term::paint(removed, " -old "), term::paint(added, " +new ")].join(" "))
            }).collect();
            if let Some(choice) = ui_choose(ui, &format!("Choose a theme (now: {})", term::theme()), &[], &options) {
                let name = term::THEMES[choice].0;
                term::set_theme(name);
                let _ = session.settings.set_value(Scope::User, "/theme", Some(json!(name)));
                say(&format!("Theme set to {name}"));
            }
        }
        "code-review" => return Flow::Prompt(format!("{CODE_REVIEW_PROMPT}{}", if args.is_empty() { String::new() } else { format!("\n\nScope: {args}") })),
        "simplify" => return Flow::Prompt(format!("{SIMPLIFY_PROMPT}{}", if args.is_empty() { String::new() } else { format!("\n\nFocus: {args}") })),
        "statusline" => return Flow::Prompt(format!("{STATUSLINE_PROMPT}{}", if args.is_empty() { String::new() } else { format!("\n\nThe user wants: {args}") })),
        "agents-new" => {
            if args.is_empty() {
                say("Usage: /agents-new WHAT THE AGENT SHOULD DO (e.g. /agents-new review Rust code for unsafe blocks)");
            } else {
                return Flow::Prompt(format!("{AGENT_PROMPT}\n\nThe agent the user wants: {args}"));
            }
        }
        "terminal-setup" => say(TERMINAL_SETUP),
        "think" => {
            session.thinking_on = !session.thinking_on;
            say(&format!("Extended thinking {}", if session.thinking_on { "on" } else { "off" }));
        }
        "permissions" => say(&permissions_command(session, args)),
        "hooks" => say(&hooks_report(session)),
        "mcp" => say(&mcp_report(session)),
        "agents" => {
            let mut text = String::new();
            for agent in &session.agents {
                text.push_str(&format!("{} ({})  {}\n", term::bold(&agent.name), agent.source, term::truncate(&agent.description, 90)));
            }
            text.push_str(&term::dim("Add your own as Markdown in .claude/agents/ (project) or ~/.claude/agents/ (you): front matter name, description, tools, model; the body is the prompt."));
            say(&text);
        }
        "todos" => {
            if session.context.todos.is_empty() {
                say("No todos currently tracked");
            } else {
                let text = session.context.todos.iter().map(|t| format!("{} {}", if t.status == "completed" { "☒" } else { "☐" }, t.content)).collect::<Vec<_>>().join("\n");
                say(&text);
            }
        }
        "copy" => {
            let last = session
                .messages
                .iter()
                .rev()
                .find(|m| m["role"] == "assistant" && m["content"].as_array().is_some_and(|c| c.iter().any(|b| b["type"] == "text")))
                .map(|m| m["content"].as_array().unwrap().iter().filter(|b| b["type"] == "text").filter_map(|b| b["text"].as_str()).collect::<Vec<_>>().join("\n"));
            match last {
                None => say("No answer to copy yet"),
                Some(text) => {
                    // OSC 52: the terminal (on the host) puts it on its clipboard, if it allows that.
                    term::raw(&format!("\x1b]52;c;{}\x07", util::base64(text.as_bytes())));
                    let file = std::env::temp_dir().join("claude-last-answer.md");
                    let _ = std::fs::write(&file, &text);
                    say(&format!("Copied {} characters to the clipboard (terminals that allow OSC 52) and to {}", text.chars().count(), file.display()));
                }
            }
        }
        "rename" => {
            if args.is_empty() {
                say("Usage: /rename NAME");
            } else {
                if let Some(transcript) = &mut session.transcript {
                    transcript.title(args);
                }
                say(&format!("Conversation renamed to: {args}"));
            }
        }
        "bashes" | "tasks" => {
            let mut words = args.split_whitespace();
            if words.next() == Some("kill") {
                let id = words.next().unwrap_or("");
                let outcome = crate::tools::bash::kill_shell(&json!({"shell_id": id}), &mut session.context);
                say(&outcome.text);
            } else if session.context.shells.list.is_empty() {
                say("No background shells");
            } else {
                let mut text = String::new();
                for shell in session.context.shells.list.iter_mut() {
                    let status = shell.status();
                    text.push_str(&format!("{}  {}  ({status}, {})\n", term::bold(&shell.id), shell.command, util::duration(shell.started.elapsed())));
                    for line in shell.tail(3).lines() {
                        text.push_str(&format!("    {}\n", term::dim(line)));
                    }
                }
                text.push_str(&term::dim("Stop one with /bashes kill ID"));
                say(&text);
            }
        }
        "add-dir" => {
            if args.is_empty() {
                say("Usage: /add-dir PATH");
            } else {
                let path = session.context.resolve(args);
                if path.is_dir() {
                    session.extra_dirs.push(path.clone());
                    session.reload_rules();
                    say(&format!("Added {} as a working directory", path.display()));
                } else {
                    ui.error(&format!("{} is not a directory", path.display()));
                }
            }
        }
        "config" | "settings" if args.is_empty() => crate::ui::settings_panel::open(session, ui),
        "config" | "settings" => say(&config_command(session, args.strip_prefix("show").unwrap_or(args).trim())),
        "output-style" => {
            let styles = extensions::styles(&session.settings.project_root);
            let chosen = match args {
                "" => {
                    let options: Vec<String> = styles.iter().map(|s| format!("{}   {}", s.name, s.description)).collect();
                    ui_choose(ui, "Choose an output style", &[], &options).map(|i| styles[i].clone())
                }
                name => styles.iter().find(|s| s.name.eq_ignore_ascii_case(name)).cloned(),
            };
            match chosen {
                Some(style) => {
                    let name = style.name.clone();
                    let _ = session.settings.update(Scope::Local, |object| {
                        object.insert("outputStyle".into(), json!(name));
                    });
                    session.style = Some(style);
                    say(&format!("Output style set to {}", term::bold(&session.style.as_ref().unwrap().name)));
                }
                None if !args.is_empty() => ui.error(&format!("No output style {args}")),
                None => {}
            }
        }
        "memory" => {
            let files = vec![
                session.settings.project_root.join("CLAUDE.md"),
                session.settings.project_root.join("CLAUDE.local.md"),
                crate::settings::user_dir().join("CLAUDE.md"),
            ];
            let options: Vec<String> = files.iter().map(|p| format!("{}{}", p.display(), if p.exists() { "" } else { " (new)" })).collect();
            if let Some(choice) = ui_choose(ui, "Select memory to edit", &[], &options) {
                let path = &files[choice];
                if let Some(dir) = path.parent() {
                    let _ = std::fs::create_dir_all(dir);
                }
                let editor_program = std::env::var("VISUAL").or_else(|_| std::env::var("EDITOR")).unwrap_or_else(|_| "vi".into());
                let status = std::process::Command::new("/bin/sh").arg("-c").arg(format!("{editor_program} \"$1\"")).arg("sh").arg(path).status();
                term::set_line_start(true);
                match status {
                    Ok(_) => say(&format!("Opened memory file at {}\n\n> To use a different editor, set the $EDITOR or $VISUAL environment variable.", path.display())),
                    Err(error) => ui.error(&format!("cannot run {editor_program}: {error}")),
                }
            }
        }
        "init" => return Flow::Prompt(INIT_PROMPT.into()),
        "review" => return Flow::Prompt(if args.is_empty() { REVIEW_PROMPT.to_string() } else { format!("{REVIEW_PROMPT}\n\nFocus: {args}") }),
        "security-review" => return Flow::Prompt(SECURITY_PROMPT.into()),
        "resume" => {
            let sessions = session::list(&session.context.cwd);
            let sessions: Vec<_> = sessions.into_iter().filter(|s| s.id != session.id).collect();
            let chosen = match args {
                "" => {
                    if sessions.is_empty() {
                        say("No conversations found to resume");
                        return Flow::Continue;
                    }
                    let options: Vec<String> = sessions.iter().take(9).map(|s| {
                        let ago = s.modified.elapsed().map(util::duration).unwrap_or_default();
                        format!("{} ago · {} messages · {}", ago, s.messages, term::truncate(s.title.as_deref().unwrap_or(&s.first_prompt), 50))
                    }).collect();
                    ui_choose(ui, "Resume Session", &[], &options).map(|i| sessions[i].path.clone())
                }
                id => session::find(&session.context.cwd, id),
            };
            if let Some(path) = chosen {
                match session::load(&path) {
                    Ok(messages) => {
                        session.messages = messages;
                        session.transcript = Some(session::Transcript::reopen(&session.context.cwd, &path));
                        if let Some(t) = &session.transcript {
                            session.id = t.id.clone();
                            session.hooks.session_id = t.id.clone();
                            session.hooks.transcript_path = t.path.display().to_string();
                        }
                        replay(session);
                    }
                    Err(message) => ui.error(&message),
                }
            } else if !args.is_empty() {
                ui.error(&format!("No conversation {args} in this project"));
            }
        }
        "export" => {
            let file = if args.is_empty() { format!("conversation-{}.txt", util::today()) } else { args.to_string() };
            let path = session.context.resolve(&file);
            match std::fs::write(&path, export_text(session)) {
                Ok(()) => say(&format!("Conversation exported to: {}", path.display())),
                Err(error) => ui.error(&format!("{}: {error}", path.display())),
            }
        }
        "login" | "logout" => say("In collaboCore the API key stays on the host: the app gives it when it starts the sandbox (addons.claude-code.apiKey, or a network secret), and the host adds it to requests. Nothing to log in or out of here."),
        "release-notes" => say(&format!("Claude Code v{VERSION} for collaboCore: a native wasm32 build.\nTools: Task (sub-agents), Bash (background shells: BashOutput, KillShell), Read, Write, Edit, MultiEdit, NotebookEdit, LS, Glob, Grep, WebFetch, WebSearch (Anthropic), TodoWrite, ExitPlanMode, MCP tools.\nSettings, permission rules and modes, hooks, custom commands, sub-agents, output styles, sessions (--continue/--resume), compaction, extended thinking, Anthropic and OpenAI-compatible models.")),
        other => {
            let custom = extensions::commands(&session.settings.project_root);
            if let Some(command) = custom.iter().find(|c| c.name == other) {
                let prompt = extensions::expand_command(command, args, &session.context.cwd);
                // The command's allowed-tools and model hold for this prompt.
                let (allow_before, model_before) = (session.cli_allow.clone(), session.model.clone());
                session.cli_allow.extend(command.allowed_tools.iter().cloned());
                if let Some(model) = &command.model {
                    session.model = crate::config::resolve_model(session.config.provider, model);
                }
                run_prompt(session, ui, editor, &prompt);
                session.cli_allow = allow_before;
                session.model = model_before;
                return Flow::Continue;
            }
            if let Some(skill) = session.skills.iter().find(|s| s.name == other).cloned() {
                let prompt = format!(
                    "The user invoked the skill \"{}\". Base directory for this skill: {}\n\n{}{}",
                    skill.name,
                    skill.dir.display(),
                    skill.body,
                    if args.is_empty() { String::new() } else { format!("\n\nARGUMENTS: {args}") }
                );
                session.skill_allow.extend(skill.allowed_tools.iter().cloned());
                return Flow::Prompt(prompt);
            }
            ui.error(&format!("Unknown slash command: /{other} (/help)"));
        }
    }
    Flow::Continue
}

/// /rewind: pick an earlier prompt, then what to restore.
fn rewind(session: &mut Session, ui: &mut TerminalUi) {
    if session.checkpoints.is_empty() {
        say("Nothing to rewind to yet");
        return;
    }
    let order: Vec<usize> = (0..session.checkpoints.len()).rev().take(9).collect();
    let options: Vec<String> = order
        .iter()
        .map(|&i| {
            let checkpoint = &session.checkpoints[i];
            let ago = checkpoint.at.elapsed().map(util::duration).unwrap_or_default();
            let changed: usize = session.checkpoints[i..].iter().map(|c| c.files.len()).sum();
            let files = if changed == 0 { "no file changes".to_string() } else { format!("{changed} file change{} since", if changed == 1 { "" } else { "s" }) };
            format!("{} · {ago} ago · {files}", term::truncate(checkpoint.prompt.lines().next().unwrap_or(""), 50))
        })
        .collect();
    let Some(choice) = ui.choose_public("Rewind", &[term::dim("Restore the conversation and/or the code to the point before a message.")], "", &options) else { return };
    let index = order[choice];
    let has_files = session.checkpoints[index..].iter().any(|c| !c.files.is_empty());
    let mut actions = vec!["Restore code and conversation", "Restore conversation", "Restore code", "Never mind"];
    if !has_files {
        actions = vec!["Restore conversation", "Never mind"];
    }
    let labels: Vec<String> = actions.iter().map(|a| a.to_string()).collect();
    let Some(action) = ui.choose_public(&format!("Rewind to: {}", term::truncate(&session.checkpoints[index].prompt, 60)), &[], "", &labels) else { return };
    let (code, conversation) = match actions[action] {
        "Restore code and conversation" => (true, true),
        "Restore conversation" => (false, true),
        "Restore code" => (true, false),
        _ => return,
    };
    let mut said = Vec::new();
    if code {
        let files = session.rewind_code(index);
        said.push(format!("Restored {} file{}", files.len(), if files.len() == 1 { "" } else { "s" }));
    }
    if conversation {
        let prompt = session.rewind_conversation(index);
        ui.draft = prompt;
        said.push("Conversation rewound; the message is back in the input box".into());
    }
    say(&said.join(" · "));
}

/// "/name args" as a prompt, when name is a custom or plugin command or a skill (for -p).
pub fn expand_slash(session: &mut Session, text: &str) -> Option<String> {
    let command = text.strip_prefix('/')?;
    let (name, args) = command.split_once(char::is_whitespace).map_or((command, ""), |(n, a)| (n, a.trim()));
    if let Some(custom) = extensions::commands(&session.settings.project_root).into_iter().find(|c| c.name == name) {
        session.cli_allow.extend(custom.allowed_tools.iter().cloned());
        return Some(extensions::expand_command(&custom, args, &session.context.cwd));
    }
    let skill = session.skills.iter().find(|s| s.name == name)?.clone();
    session.skill_allow.extend(skill.allowed_tools.iter().cloned());
    Some(format!(
        "The user invoked the skill \"{}\". Base directory for this skill: {}\n\n{}{}",
        skill.name,
        skill.dir.display(),
        skill.body,
        if args.is_empty() { String::new() } else { format!("\n\nARGUMENTS: {args}") }
    ))
}

fn host_skills(session: &Session) -> Vec<(String, String)> {
    session.skills.iter().map(|s| (format!("/{}", s.name), format!("{} (skill)", s.description))).collect()
}

/// "30s", "5m", "1h" → a duration (10 s at least).
fn parse_interval(text: &str) -> Option<Duration> {
    let (number, unit) = text.split_at(text.len().checked_sub(1)?);
    let n: u64 = number.parse().ok()?;
    let seconds = match unit {
        "s" => n,
        "m" => n * 60,
        "h" => n * 3600,
        _ => return None,
    };
    Some(Duration::from_secs(seconds.max(10)))
}

/// Between /loop runs: a countdown. True to run again (the time came, or Enter); false when
/// the user stops the loop (Esc, Ctrl-C, q).
fn wait_for_next(ui: &mut TerminalUi, every: Duration) -> bool {
    let _mode = term::Mode::raw();
    let until = Instant::now() + every;
    let result = loop {
        let left = until.saturating_duration_since(Instant::now());
        if left.is_zero() {
            break true;
        }
        term::raw(&format!("\r\x1b[2K{}", term::cyan(&format!("⟳ next run in {} · enter to run now · esc to stop", util::duration(left + Duration::from_millis(999))))));
        if term::stdin_ready(1000) {
            match ui.keys.read() {
                Some(crate::ui::editor::Key::Enter) => break true,
                Some(crate::ui::editor::Key::Esc | crate::ui::editor::Key::CtrlC | crate::ui::editor::Key::CtrlD | crate::ui::editor::Key::Char('q')) | None => break false,
                _ => {}
            }
        }
    };
    term::raw("\r\x1b[2K");
    term::set_line_start(true);
    result
}

fn estimate_tokens(text: &str) -> u64 {
    (text.len() as u64).div_ceil(4)
}

fn context_report(session: &Session) -> String {
    let window = session.config.context_window.max(1);
    let system = estimate_tokens(&session.system_prompt());
    let tools = estimate_tokens(&serde_json::to_string(&session.tool_specs()).unwrap_or_default());
    let memory: u64 = crate::prompt::memory_files(&session.context.cwd).iter().map(|(_, t)| estimate_tokens(t)).sum();
    let messages = estimate_tokens(&serde_json::to_string(&session.messages).unwrap_or_default());
    let used = if session.context_tokens > 0 { session.context_tokens } else { system + tools + messages };
    let cells = 50usize;
    let part = |n: u64| ((n as f64 / window as f64) * cells as f64).round() as usize;
    let (s, t, m) = (part(system.saturating_sub(memory)), part(tools), part(messages));
    let mem = part(memory);
    let mut grid = String::new();
    for (count, color) in [(s, term::dim as fn(&str) -> String), (mem, term::yellow), (t, term::cyan), (m, term::magenta)] {
        grid.push_str(&color(&"⛁ ".repeat(count.min(cells))));
    }
    let filled = (s + mem + t + m).min(cells);
    grid.push_str(&term::dim(&"⛶ ".repeat(cells - filled)));
    let pct = |n: u64| format!("{:.1}%", n as f64 * 100.0 / window as f64);
    format!(
        "{}\n{grid}\n\n{} {}/{} tokens ({})\n{} System prompt: {} ({})\n{} Memory files: {} ({})\n{} Tools: {} ({})\n{} Messages: {} ({})\n{} Free space: {} ({})",
        term::bold("Context Usage"),
        term::bold(&session.model),
        util::tokens(used),
        util::tokens(window),
        pct(used),
        term::dim("⛁"),
        util::tokens(system.saturating_sub(memory)),
        pct(system.saturating_sub(memory)),
        term::yellow("⛁"),
        util::tokens(memory),
        pct(memory),
        term::cyan("⛁"),
        util::tokens(tools),
        pct(tools),
        term::magenta("⛁"),
        util::tokens(messages),
        pct(messages),
        term::dim("⛶"),
        util::tokens(window.saturating_sub(used)),
        pct(window.saturating_sub(used)),
    )
}

fn cost_report(session: &Session) -> String {
    let u = session.usage;
    format!(
        "Total tokens:          {} input, {} output\nCache:                 {} read, {} written\nTotal duration (API):  {}\nTotal duration (wall): {}\n(Cost in dollars is not tracked here: see the provider's console.)",
        util::tokens(u.input),
        util::tokens(u.output),
        util::tokens(u.cache_read),
        util::tokens(u.cache_write),
        util::duration(session.api_time),
        util::duration(session.started.elapsed()),
    )
}

fn status_report(session: &Session) -> String {
    let files: Vec<String> = session
        .settings
        .layers
        .iter()
        .filter(|l| l.value.as_object().is_some_and(|o| !o.is_empty()))
        .map(|l| format!("{} ({})", l.path.as_ref().map(|p| p.display().to_string()).unwrap_or_else(|| "--settings".into()), l.scope.name()))
        .collect();
    let mcp = session.mcp.borrow();
    let connected = mcp.servers.iter().filter(|s| s.status == "connected").count();
    let memory = crate::prompt::memory_files(&session.context.cwd).iter().map(|(p, _)| p.display().to_string()).collect::<Vec<_>>();
    format!(
        "Version:          {VERSION} (collaboCore, wasm32)\nSession ID:       {}\ncwd:              {}\n\n{}\n\nModel:            {}\nPermission mode:  {}\nSetting sources:  {}\nMemory:           {}\nMCP servers:      {connected} connected of {}\nTools:            {}",
        session.id,
        session.context.cwd.display(),
        session.config.describe(),
        session.model,
        session.mode.name(),
        if files.is_empty() { "none".into() } else { files.join(", ") },
        if memory.is_empty() { "none".into() } else { memory.join(", ") },
        mcp.servers.len(),
        session.tool_specs().len(),
    )
}

fn doctor_report(session: &Session) -> String {
    let mut lines = Vec::new();
    let check = |ok: bool, text: String| format!("{} {text}", if ok { term::green("✓") } else { term::red("✗") });
    lines.push(check(Path::new("/bin/sh").exists(), "/bin/sh".into()));
    for layer in &session.settings.layers {
        if let Some(path) = &layer.path {
            if path.exists() {
                let valid = std::fs::read(path).ok().and_then(|b| serde_json::from_slice::<Value>(&b).ok()).is_some();
                lines.push(check(valid, format!("{} settings: {}", layer.scope.name(), path.display())));
            }
        }
    }
    let connection = match crate::llm::list_models(&session.config) {
        Ok(models) => check(true, format!("{} reachable ({} models listed)", session.config.base_url, models.len())),
        Err(error) => check(false, format!("{}: {}", session.config.base_url, error.message)),
    };
    lines.push(connection);
    if let Some(Value::Object(hooks)) = session.settings.get("/hooks") {
        for (event, groups) in hooks {
            for command in groups.as_array().into_iter().flatten().flat_map(|g| g["hooks"].as_array().cloned().unwrap_or_default()).filter_map(|h| h["command"].as_str().map(str::to_string)) {
                let program = command.split_whitespace().next().unwrap_or("").to_string();
                let found = program.contains('/') && Path::new(&program).exists()
                    || std::process::Command::new("/bin/sh").arg("-c").arg(format!("command -v {program} >/dev/null")).status().is_ok_and(|s| s.success());
                lines.push(check(found, format!("{event} hook: {command}")));
            }
        }
    }
    for server in &session.mcp.borrow().servers {
        lines.push(check(server.status == "connected", format!("MCP {}: {}", server.name, server.status)));
    }
    lines.join("\n")
}

fn permissions_command(session: &mut Session, args: &str) -> String {
    let words: Vec<&str> = args.split_whitespace().collect();
    let scope_of = |words: &[&str]| match words.iter().find(|w| w.starts_with("--")) {
        Some(&"--user") => Scope::User,
        Some(&"--project") => Scope::Project,
        _ => Scope::Local,
    };
    match words.as_slice() {
        ["add", kind @ ("allow" | "ask" | "deny"), rest @ ..] if !rest.is_empty() => {
            let rule = rest.iter().filter(|w| !w.starts_with("--")).cloned().collect::<Vec<_>>().join(" ");
            match session.settings.add_rule(scope_of(rest), kind, &rule) {
                Ok(path) => {
                    session.reload_rules();
                    format!("Added {kind} rule {rule} to {}", path.display())
                }
                Err(message) => message,
            }
        }
        ["remove", kind @ ("allow" | "ask" | "deny"), rest @ ..] if !rest.is_empty() => {
            let rule = rest.iter().filter(|w| !w.starts_with("--")).cloned().collect::<Vec<_>>().join(" ");
            match session.settings.remove_rule(scope_of(rest), kind, &rule) {
                Ok(true) => {
                    session.reload_rules();
                    format!("Removed {kind} rule {rule}")
                }
                Ok(false) => format!("No {kind} rule {rule} in the {} settings", scope_of(rest).name()),
                Err(message) => message,
            }
        }
        _ => {
            let mut text = format!("Permission mode: {}\n", term::bold(session.mode.name()));
            for kind in ["allow", "ask", "deny"] {
                text.push_str(&format!("\n{}\n", term::bold(&format!("{kind}:"))));
                let mut any = false;
                for layer in &session.settings.layers {
                    for rule in layer.value.pointer(&format!("/permissions/{kind}")).and_then(Value::as_array).into_iter().flatten().filter_map(Value::as_str) {
                        text.push_str(&format!("  {rule}  {}\n", term::dim(&format!("({})", layer.scope.name()))));
                        any = true;
                    }
                }
                let none = Vec::new();
                for rule in match kind { "allow" => &session.cli_allow, "deny" => &session.cli_deny, _ => &none } {
                    text.push_str(&format!("  {rule}  {}\n", term::dim("(command line)")));
                    any = true;
                }
                if !any {
                    text.push_str(&term::dim("  (none)\n"));
                }
            }
            text.push_str(&format!("\nWorking directories: {}\n", session.directories().iter().map(|d| d.display().to_string()).collect::<Vec<_>>().join(", ")));
            text.push_str(&term::dim("Change: /permissions add|remove allow|ask|deny RULE [--local|--project|--user]   e.g. /permissions add allow Bash(npm test:*)"));
            text
        }
    }
}

fn hooks_report(session: &Session) -> String {
    let Some(Value::Object(hooks)) = session.settings.get("/hooks") else {
        return "No hooks configured. Add them to .claude/settings.json:\n  \"hooks\": {\"PreToolUse\": [{\"matcher\": \"Bash\", \"hooks\": [{\"type\": \"command\", \"command\": \"…\"}]}]}\nEvents: PreToolUse, PostToolUse, UserPromptSubmit, Stop, SubagentStop, SessionStart, SessionEnd, Notification, PreCompact".into();
    };
    let mut text = String::new();
    for (event, groups) in hooks {
        text.push_str(&format!("{}\n", term::bold(event)));
        for group in groups.as_array().into_iter().flatten() {
            let matcher = group["matcher"].as_str().filter(|m| !m.is_empty()).unwrap_or("*");
            for hook in group["hooks"].as_array().into_iter().flatten() {
                text.push_str(&format!("  [{matcher}] {}\n", hook["command"].as_str().unwrap_or("")));
            }
        }
    }
    text
}

fn mcp_report(session: &Session) -> String {
    let mcp = session.mcp.borrow();
    if mcp.servers.is_empty() {
        return "No MCP servers configured. Add them with `claude mcp add NAME -- COMMAND ARGS…`, in .mcp.json, or with --mcp-config.".into();
    }
    let mut text = String::new();
    for server in &mcp.servers {
        let mark = if server.status == "connected" { term::green("✔") } else { term::red("✘") };
        text.push_str(&format!("{mark} {} ({})  {}\n", term::bold(&server.name), server.scope, server.status));
        for tool in &server.tools {
            text.push_str(&format!("    {}\n", term::dim(tool["name"].as_str().unwrap_or(""))));
        }
    }
    text
}

fn config_command(session: &mut Session, args: &str) -> String {
    let words: Vec<&str> = args.split_whitespace().collect();
    if let ["set", key, rest @ ..] = words.as_slice() {
        let value_text = rest.iter().filter(|w| !w.starts_with("--")).cloned().collect::<Vec<_>>().join(" ");
        let value: Value = serde_json::from_str(&value_text).unwrap_or(json!(value_text));
        let scope = if rest.contains(&"--user") || rest.contains(&"--global") { Scope::User } else if rest.contains(&"--project") { Scope::Project } else { Scope::Local };
        let key = key.to_string();
        return match session.settings.update(scope, |object| {
            object.insert(key.clone(), value.clone());
        }) {
            Ok(path) => match session.reload_settings() {
                Ok(()) => format!("Set {key} in {} (in effect now)", path.display()),
                Err(message) => format!("Set {key} in {}, but the settings do not work: {message}", path.display()),
            },
            Err(message) => message,
        };
    }
    let mut merged = session.settings.merged.clone();
    if let Some(object) = merged.as_object_mut() {
        if object.contains_key("apiKey") {
            object.insert("apiKey".into(), json!("***"));
        }
    }
    format!("{}\n\n{}\n\n{}", session.config.describe(), serde_json::to_string_pretty(&merged).unwrap_or_default(), term::dim("Change: /config set KEY VALUE [--local|--project|--user]"))
}

fn export_text(session: &Session) -> String {
    let mut out = String::new();
    for message in &session.messages {
        let role = message["role"].as_str().unwrap_or("user");
        for block in message["content"].as_array().cloned().unwrap_or_else(|| vec![json!({"type": "text", "text": message["content"]})]) {
            match block["type"].as_str() {
                Some("text") => out.push_str(&format!("{} {}\n\n", if role == "user" { ">" } else { "⏺" }, block["text"].as_str().unwrap_or(""))),
                Some("tool_use") => out.push_str(&format!("⏺ {}({})\n", block["name"].as_str().unwrap_or(""), block["input"])),
                Some("tool_result") => {
                    let text = match &block["content"] {
                        Value::String(t) => t.clone(),
                        other => other.to_string(),
                    };
                    out.push_str(&format!("  ⎿  {}\n\n", text.lines().take(10).collect::<Vec<_>>().join("\n     ")));
                }
                _ => {}
            }
        }
    }
    out
}
