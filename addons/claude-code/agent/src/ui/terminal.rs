//! The interactive terminal: the conversation drawn as Claude Code draws it, the permission and
//! plan dialogs, and the key watcher that lets Esc interrupt while Claude works.
//!
//!   ⏺ Claude's answer, rendered Markdown
//!   ⏺ Bash(npm test)
//!     ⎿  PASS src/app.test.js
//!        … +12 lines
//!   ⏺ Update(src/app.js)
//!     ⎿  Updated src/app.js with 1 addition and 1 removal
//!          12 -  old
//!          12 +  new
use std::io::Read as _;
use std::path::Path;
use std::sync::atomic::{AtomicBool, Ordering};

/// Verbose output (Ctrl-O, or the verbose setting): thinking and whole tool results shown.
pub static VERBOSE: AtomicBool = AtomicBool::new(false);

pub fn verbose() -> bool {
    VERBOSE.load(Ordering::Relaxed)
}

pub fn toggle_verbose() -> bool {
    !VERBOSE.fetch_xor(true, Ordering::Relaxed)
}
use std::sync::Arc;

use serde_json::Value;

use super::editor::{Key, Keys};
use super::spinner;
use crate::agent::{Approval, PlanAnswer, Ui};
use crate::permissions::Mode;
use crate::render::{self, Markdown};
use crate::term;
use crate::tools::{self, Display, Outcome};

/// Reads keys while a turn runs: Esc interrupts; everything else goes to the live panel's input
/// box (Enter queues a message, Shift+Tab changes the mode).
pub struct Watcher {
    stop: Arc<AtomicBool>,
    handle: Option<std::thread::JoinHandle<Vec<u8>>>,
    _mode: Option<term::Mode>,
}

impl Watcher {
    /// `typed`: keys already read by the input box and not yet decoded.
    pub fn start(typed: Vec<u8>) -> Watcher {
        let stop = Arc::new(AtomicBool::new(false));
        if !term::stdin_is_tty() {
            return Watcher { stop, handle: None, _mode: None };
        }
        let mode = term::Mode::cbreak();
        let flag = stop.clone();
        let handle = crate::interrupt::spawn_returning(move || {
            let mut keys = Keys::default();
            keys.give(typed);
            while !flag.load(Ordering::SeqCst) {
                if !keys.has_pending() && !term::stdin_ready(50) {
                    continue;
                }
                match keys.read() {
                    Some(Key::Esc) => crate::interrupt::trigger(),
                    Some(key) => spinner::live_key(key),
                    None => break,
                }
            }
            keys.take_pending()
        });
        Watcher { stop, handle: Some(handle), _mode: Some(mode) }
    }

    /// Stops watching; what it read and did not decode yet.
    pub fn finish(mut self) -> Vec<u8> {
        self.stop.store(true, Ordering::SeqCst);
        self.handle.take().and_then(|handle| handle.join().ok()).unwrap_or_default()
    }
}

pub struct TerminalUi {
    markdown: Markdown,
    /// The answer has started (its first line gets ⏺).
    in_message: bool,
    in_thinking: bool,
    pub watcher: Option<Watcher>,
    /// Keys the watcher had read and not decoded when it stopped (for the input box).
    pub leftover: Vec<u8>,
    /// The last turn was interrupted (queued messages then go back to the input box).
    pub was_interrupted: bool,
    /// Text for the next input box: what was being typed when the turn ended.
    pub draft: String,
    pub cwd: std::path::PathBuf,
    /// A tool call whose line is drawn when its result arrives.
    pending_call: Option<String>,
    pub keys: Keys,
}

fn bullet(color: fn(&str) -> String) -> String {
    color("⏺")
}

/// "  ⎿  first" and the next lines indented, at most `max` with "… +N lines".
fn result_lines(text: &str, max: usize, color: fn(&str) -> String) -> String {
    let lines: Vec<&str> = text.trim_end().lines().collect();
    let columns = term::columns().saturating_sub(7).max(20);
    let mut out = String::new();
    for (index, line) in lines.iter().take(max).enumerate() {
        let prefix = if index == 0 { "  ⎿  " } else { "     " };
        out.push_str(&format!("{}{}\n", term::dim(prefix), color(&term::truncate(&line.replace('\t', "  "), columns))));
    }
    if lines.len() > max {
        out.push_str(&format!("{}\n", term::dim(&format!("     … +{} lines", lines.len() - max))));
    }
    if lines.is_empty() {
        out.push_str(&format!("{}\n", term::dim("  ⎿  (No content)")));
    }
    out
}

fn plain(text: &str) -> String {
    text.to_string()
}

impl TerminalUi {
    pub fn new(cwd: &Path) -> TerminalUi {
        TerminalUi { markdown: Markdown::default(), in_message: false, in_thinking: false, watcher: None, leftover: Vec::new(), was_interrupted: false, draft: String::new(), cwd: cwd.to_path_buf(), pending_call: None, keys: Keys::default() }
    }

    fn pause_watcher(&mut self) {
        if let Some(watcher) = self.watcher.take() {
            let leftover = watcher.finish();
            self.leftover.extend(leftover);
        }
    }

    fn resume_watcher(&mut self) {
        if self.watcher.is_none() {
            let typed = std::mem::take(&mut self.leftover);
            self.watcher = Some(Watcher::start(typed));
        }
    }

    fn flush_pending_call(&mut self, ok: bool) {
        if let Some(line) = self.pending_call.take() {
            let mark = if ok { bullet(term::green) } else { bullet(term::red) };
            term::out(&format!("{mark} {line}\n"));
        }
    }

    /// A menu in a box; the chosen option's index, or None for Esc.
    fn choose(&mut self, title: &str, body: &[String], question: &str, options: &[String]) -> Option<usize> {
        spinner::stop();
        self.pause_watcher();
        let _mode = term::Mode::raw();
        let columns = term::columns().max(30);
        let mut selected = 0usize;
        let mut drawn_rows = 0usize;
        let result = loop {
            let mut lines = vec![term::accent(&format!("╭{}╮", "─".repeat(columns - 2)))];
            let mut row = |text: String| {
                let pad = (columns - 4).saturating_sub(term::width(&text));
                lines.push(format!("{} {text}{} {}", term::accent("│"), " ".repeat(pad), term::accent("│")));
            };
            row(term::bold(&term::truncate(title, columns - 4)));
            row(String::new());
            for line in body {
                row(term::truncate(line, columns - 4));
            }
            if !body.is_empty() {
                row(String::new());
            }
            row(question.to_string());
            for (index, option) in options.iter().enumerate() {
                let text = term::truncate(&format!("{}. {option}", index + 1), columns - 8);
                row(match index == selected {
                    true => term::accent(&format!("❯ {text}")),
                    false => format!("  {text}"),
                });
            }
            lines.push(term::accent(&format!("╰{}╯", "─".repeat(columns - 2))));
            let mut out = String::from("\r");
            if drawn_rows > 0 {
                out.push_str(&format!("\x1b[{}A", drawn_rows));
            }
            out.push_str("\x1b[J");
            out.push_str(&lines.join("\r\n"));
            term::raw(&out);
            drawn_rows = lines.len() - 1;
            match self.keys.read() {
                Some(Key::Up) => selected = (selected + options.len() - 1) % options.len(),
                Some(Key::Down) | Some(Key::Tab) => selected = (selected + 1) % options.len(),
                Some(Key::Enter) => break Some(selected),
                Some(Key::Char(c)) if c.is_ascii_digit() => {
                    let n = c.to_digit(10).unwrap_or(0) as usize;
                    if (1..=options.len()).contains(&n) {
                        break Some(n - 1);
                    }
                }
                Some(Key::Esc) | Some(Key::CtrlC) | Some(Key::CtrlD) | None => break None,
                _ => {}
            }
        };
        let mut out = String::from("\r");
        if drawn_rows > 0 {
            out.push_str(&format!("\x1b[{}A", drawn_rows));
        }
        out.push_str("\x1b[J");
        term::raw(&out);
        term::set_line_start(true);
        result
    }

    /// A menu for the REPL's own questions (/model, /resume, #memory).
    pub fn choose_public(&mut self, title: &str, body: &[String], question: &str, options: &[String]) -> Option<usize> {
        let choice = self.choose(title, body, question, options);
        self.pause_watcher();
        choice
    }

    /// A line of feedback after "No" (optional).
    fn feedback(&mut self) -> Option<String> {
        let _mode = term::Mode::raw();
        term::raw(&format!("{} ", term::accent("  What should Claude do instead? (enter to skip) ›")));
        let mut text = String::new();
        loop {
            match self.keys.read() {
                Some(Key::Enter) | None => break,
                Some(Key::Char(c)) => {
                    text.push(c);
                    term::raw(&c.to_string());
                }
                Some(Key::Paste(p)) => {
                    text.push_str(&p);
                    term::raw(&p);
                }
                Some(Key::Backspace) => {
                    if let Some(c) = text.pop() {
                        term::raw(&"\x08 \x08".repeat(term::char_width(c).max(1)));
                    }
                }
                Some(Key::Esc) | Some(Key::CtrlC) => {
                    text.clear();
                    break;
                }
                _ => {}
            }
        }
        term::raw("\r\x1b[2K");
        term::set_line_start(true);
        Some(text).filter(|t| !t.trim().is_empty())
    }

    /// What a call will change, for the dialog.
    fn preview(&self, name: &str, input: &Value) -> Vec<String> {
        let file = |key: &str| input.get(key).and_then(Value::as_str).map(|p| crate::permissions::normalize(Path::new(p), &self.cwd));
        match name {
            "Bash" => {
                let mut lines: Vec<String> = input["command"].as_str().unwrap_or("").lines().map(|l| format!("  {l}")).take(12).collect();
                if let Some(description) = input["description"].as_str() {
                    lines.push(term::dim(&format!("  {description}")));
                }
                lines
            }
            "Write" | "Edit" | "MultiEdit" => {
                let Some(path) = file("file_path") else { return Vec::new() };
                let before = std::fs::read_to_string(&path).unwrap_or_default();
                let after = match name {
                    "Write" => input["content"].as_str().unwrap_or("").to_string(),
                    _ => {
                        let edits = match name {
                            "Edit" => vec![input.clone()],
                            _ => input["edits"].as_array().cloned().unwrap_or_default(),
                        };
                        let mut text = before.clone();
                        for edit in edits {
                            let (old, new) = (edit["old_string"].as_str().unwrap_or(""), edit["new_string"].as_str().unwrap_or(""));
                            if old.is_empty() {
                                continue;
                            }
                            text = match edit["replace_all"].as_bool() == Some(true) {
                                true => text.replace(old, new),
                                false => text.replacen(old, new, 1),
                            };
                        }
                        text
                    }
                };
                let (_, lines) = render::diff(&before, &after, 16);
                lines
            }
            "WebFetch" => vec![format!("  {}", input["url"].as_str().unwrap_or("")), term::dim(&format!("  {}", input["prompt"].as_str().unwrap_or("")))],
            _ => {
                let text = serde_json::to_string_pretty(input).unwrap_or_default();
                text.lines().take(10).map(|l| format!("  {l}")).collect()
            }
        }
    }
}

impl Ui for TerminalUi {
    fn begin_request(&mut self) {
        self.resume_watcher();
        spinner::start();
    }

    fn text(&mut self, text: &str) {
        if self.in_thinking {
            self.in_thinking = false;
            term::out("\n\n");
        }
        let rendered = self.markdown.push(text);
        self.write_answer(&rendered);
    }

    fn thinking(&mut self, text: &str) {
        if text.is_empty() {
            return;
        }
        if !self.in_thinking {
            self.in_thinking = true;
            term::out(&format!("{}\n", term::italic(&term::dim("✻ Thinking…"))));
        }
        if verbose() {
            term::out(&term::italic(&term::dim(text)));
        }
    }

    fn end_message(&mut self) {
        if self.in_thinking {
            self.in_thinking = false;
            term::out("\n");
        }
        let rest = self.markdown.finish();
        self.write_answer(&rest);
        if self.in_message {
            term::out("\n");
        }
        self.in_message = false;
    }

    fn notice(&mut self, text: &str) {
        term::out(&format!("{}\n", term::dim(&format!("  ⎿  {text}"))));
    }

    fn error(&mut self, text: &str) {
        term::out(&format!("{}\n", term::red(&format!("  ⎿  {text}"))));
    }

    fn tool_call(&mut self, name: &str, _input: &Value, summary: &str) {
        self.flush_pending_call(true);
        let line = match summary.is_empty() {
            true => term::bold(&tools::display_name(name)),
            false => format!("{}({summary})", term::bold(&tools::display_name(name))),
        };
        if name == "Task" {
            term::out(&format!("{} {line}\n", bullet(term::accent)));
        } else {
            self.pending_call = Some(line);
            spinner::set_note(Some(match name {
                "Bash" => "Running… (ctrl+b to run in background)".to_string(),
                other => format!("Running {}…", tools::display_name(other)),
            }));
        }
    }

    fn sub_step(&mut self, _agent: &str, line: &str) {
        term::out(&format!("{}\n", term::dim(&format!("  ⎿  {}", term::truncate(line, term::columns().saturating_sub(8))))));
    }

    fn tool_result(&mut self, name: &str, input: &Value, outcome: &Outcome) {
        spinner::set_note(None);
        self.flush_pending_call(!outcome.is_error);
        let first_line = outcome.text.lines().next().unwrap_or("");
        let text = if outcome.is_error {
            let shown = if outcome.text.starts_with("The user doesn't want to proceed") {
                "User rejected the tool use".to_string()
            } else {
                outcome.text.clone()
            };
            result_lines(&shown, 6, term::red)
        } else {
            match (&outcome.display, name) {
                (Some(Display::Diff { path, before, after }), _) => {
                    let shown = path.strip_prefix(&self.cwd).map(|p| p.display().to_string()).unwrap_or_else(|_| path.display().to_string());
                    match before.is_empty() && name == "Write" {
                        true => {
                            let count = after.lines().count();
                            let mut out = format!("{}Wrote {count} lines to {}\n", term::dim("  ⎿  "), term::bold(&shown));
                            for (i, line) in after.lines().take(8).enumerate() {
                                out.push_str(&format!("     {}\n", term::dim(&format!("{:>4} {}", i + 1, term::truncate(line, term::columns().saturating_sub(12))))));
                            }
                            if count > 8 {
                                out.push_str(&format!("{}\n", term::dim(&format!("     … +{} lines", count - 8))));
                            }
                            out
                        }
                        false => {
                            let (summary, lines) = render::diff(before, after, 40);
                            let mut out = format!("{}Updated {} with {summary}\n", term::dim("  ⎿  "), term::bold(&shown));
                            for line in lines {
                                out.push_str(&format!("     {line}\n"));
                            }
                            out
                        }
                    }
                }
                (Some(Display::Todos(todos)), _) => {
                    spinner::set_task(crate::tools::todo::active(todos).map(str::to_string));
                    spinner::set_todos(todos.iter().map(|t| (t.content.clone(), t.status.clone())).collect());
                    let mut out = String::new();
                    for (index, todo) in todos.iter().enumerate() {
                        let prefix = if index == 0 { "  ⎿  " } else { "     " };
                        let line = match todo.status.as_str() {
                            "completed" => term::dim(&format!("☒ \x1b[9m{}\x1b[29m", todo.content)),
                            "in_progress" => term::bold(&format!("☐ {}", todo.content)),
                            _ => format!("☐ {}", todo.content),
                        };
                        out.push_str(&format!("{}{line}\n", term::dim(prefix)));
                    }
                    out
                }
                (Some(Display::Note(note)), _) => format!("{}\n", term::dim(&format!("  ⎿  {note}"))),
                (_, "Read") => {
                    let count = outcome.text.lines().filter(|l| l.len() > 7 && l.as_bytes()[6] == b'\t').count();
                    let what = if outcome.content.is_some() { "Read image".to_string() } else { format!("Read {} lines", count) };
                    format!("{}\n", term::dim(&format!("  ⎿  {what}")))
                }
                (_, "Glob") => {
                    let count = if first_line == "No files found" { 0 } else { outcome.text.lines().filter(|l| !l.starts_with('(')).count() };
                    format!("{}\n", term::dim(&format!("  ⎿  Found {count} files")))
                }
                (_, "Grep") => {
                    let summary = if outcome.text.starts_with("Found ") { first_line.to_string() } else if first_line == "No matches found" { first_line.to_string() } else { format!("Found {} lines", outcome.text.lines().count()) };
                    format!("{}\n", term::dim(&format!("  ⎿  {summary}")))
                }
                (_, "LS") => format!("{}\n", term::dim(&format!("  ⎿  Listed {} paths", outcome.text.lines().count().saturating_sub(1)))),
                (_, "WebFetch") => format!("{}\n", term::dim(&format!("  ⎿  Received {} KB", outcome.text.len() / 1024))),
                (_, "web_search") => format!("{}\n", term::dim(&format!("  ⎿  {first_line}"))),
                _ => result_lines(&outcome.text, if verbose() { 200 } else { 4 }, plain),
            }
        };
        let _ = input;
        term::out(&text);
    }

    fn ask(&mut self, name: &str, input: &Value, summary: &str, suggestion: &str) -> Approval {
        self.flush_pending_call(true);
        let title = match name {
            "Bash" => "Bash command".to_string(),
            "Edit" | "MultiEdit" => "Edit file".to_string(),
            "Write" => if Path::new(input["file_path"].as_str().unwrap_or("")).exists() { "Overwrite file".into() } else { "Create file".into() },
            "WebFetch" => "Fetch".to_string(),
            other => format!("Tool use: {}", tools::display_name(other)),
        };
        let mut body = self.preview(name, input);
        if body.is_empty() {
            body.push(format!("  {summary}"));
        }
        let edit = crate::permissions::is_edit(name);
        let options = match edit {
            true => vec!["Yes".to_string(), "Yes, allow all edits during this session (shift+tab)".to_string(), "No, and tell Claude what to do differently (esc)".to_string()],
            false => vec![
                "Yes".to_string(),
                format!("Yes, and don't ask again for {suggestion} in {}", self.cwd.display()),
                "No, and tell Claude what to do differently (esc)".to_string(),
            ],
        };
        let question = match name {
            "Write" | "Edit" | "MultiEdit" => format!("Do you want to make this edit to {}?", Path::new(input["file_path"].as_str().unwrap_or("")).file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default()),
            _ => "Do you want to proceed?".to_string(),
        };
        let choice = self.choose(&title, &body, &question, &options);
        let answer = match choice {
            Some(0) => Approval::Once,
            Some(1) if edit => Approval::AcceptEdits,
            Some(1) => Approval::Always,
            _ => Approval::No(self.feedback()),
        };
        self.resume_watcher();
        spinner::start();
        answer
    }

    fn approve_plan(&mut self, plan: &str) -> PlanAnswer {
        let mut markdown = Markdown::default();
        let mut rendered = markdown.push(plan);
        rendered.push_str(&markdown.finish());
        let body: Vec<String> = rendered.lines().map(|l| l.to_string()).collect();
        let options = ["Yes, and auto-accept edits".to_string(), "Yes, and manually approve edits".to_string(), "No, keep planning".to_string()];
        let answer = match self.choose("Ready to code?", &body, "Here is Claude's plan. Would you like to proceed?", &options) {
            Some(0) => PlanAnswer::Approve(Mode::AcceptEdits),
            Some(1) => PlanAnswer::Approve(Mode::Default),
            _ => PlanAnswer::KeepPlanning(self.feedback()),
        };
        self.resume_watcher();
        spinner::start();
        answer
    }

    fn sync_mode(&mut self, current: Mode) -> Mode {
        spinner::sync_mode(current)
    }

    fn usage(&mut self, total_output: u64) {
        spinner::set_tokens(total_output);
    }

    fn interrupted(&mut self) {
        self.was_interrupted = true;
        spinner::stop();
        term::out(&format!("{}\n", term::red("  ⎿  Interrupted by user")));
    }
}

impl TerminalUi {
    fn write_answer(&mut self, rendered: &str) {
        if rendered.is_empty() {
            return;
        }
        let mut out = String::new();
        for line in rendered.split_inclusive('\n') {
            if !self.in_message {
                self.in_message = true;
                out.push_str(&format!("{} ", bullet(plain)));
            } else {
                out.push_str("  ");
            }
            out.push_str(line);
        }
        term::out(&out);
    }

    /// After a turn: the spinner and the watcher stop.
    pub fn end_turn(&mut self) {
        spinner::stop();
        self.flush_pending_call(true);
        self.pause_watcher();
    }
}

/// Reads all of stdin (for -p with a piped prompt).
pub fn read_all_stdin() -> String {
    let mut text = String::new();
    let _ = std::io::stdin().read_to_string(&mut text);
    text
}
