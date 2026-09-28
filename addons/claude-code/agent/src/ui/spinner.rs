//! The live panel under the conversation while Claude works: the spinner (a glyph that
//! breathes, a verb, the time, the tokens, "esc to interrupt", what is happening), messages
//! queued for after the turn, the input box the user keeps typing in, and the permission mode
//! (Shift+Tab changes it as Claude works).
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Mutex;
use std::time::{Duration, Instant};

use crate::permissions::Mode;
use crate::term;
use crate::ui::editor::Key;

const FRAMES: [&str; 10] = ["·", "✢", "✳", "✶", "✻", "✽", "✻", "✶", "✳", "✢"];
const VERBS: [&str; 40] = [
    "Accomplishing", "Actioning", "Actualizing", "Baking", "Brewing", "Calculating", "Cerebrating", "Churning", "Clauding", "Coalescing",
    "Cogitating", "Computing", "Conjuring", "Considering", "Cooking", "Crafting", "Creating", "Crunching", "Deliberating", "Determining",
    "Doing", "Effecting", "Finagling", "Forging", "Forming", "Generating", "Hatching", "Herding", "Honking", "Hustling",
    "Ideating", "Inferring", "Manifesting", "Marinating", "Moseying", "Mulling", "Mustering", "Musing", "Noodling", "Percolating",
];

struct State {
    active: bool,
    /// Lines on screen now (0, 1 or 2).
    drawn: usize,
    verb: String,
    started: Option<Instant>,
    tokens: u64,
    note: Option<String>,
    /// The task in progress (TodoWrite's activeForm), in place of a random verb.
    task: Option<String>,
    frame: usize,
    /// Rows from the top of the drawing to the row the cursor was left on.
    cursor_row: usize,
    live: Option<Live>,
    /// The session's todo list (content, status).
    todos: Vec<(String, String)>,
}

/// What the user does while a turn runs.
struct Live {
    buffer: Vec<char>,
    cursor: usize,
    queued: Vec<String>,
    mode: Mode,
    bypass: bool,
    /// The user changed the mode since the session last looked.
    mode_changed: bool,
}

static STATE: Mutex<State> = Mutex::new(State { active: false, drawn: 0, verb: String::new(), started: None, tokens: 0, note: None, task: None, frame: 0, cursor_row: 0, live: None, todos: Vec::new() });
static THREAD: AtomicBool = AtomicBool::new(false);
/// Ctrl-T: the todo list under the spinner.
pub static SHOW_TODOS: AtomicBool = AtomicBool::new(true);

fn state() -> std::sync::MutexGuard<'static, State> {
    STATE.lock().unwrap_or_else(|poison| poison.into_inner())
}

/// Starts (or keeps) the spinner; a new verb each time it starts.
pub fn start() {
    if !term::color() {
        return;
    }
    {
        let mut s = state();
        if !s.active {
            let pick = crate::util::random_bytes(1)[0] as usize % VERBS.len();
            s.verb = VERBS[pick].to_string();
            s.started = Some(Instant::now());
            s.active = true;
        }
    }
    if !THREAD.swap(true, Ordering::SeqCst) {
        crate::interrupt::spawn(|| loop {
            std::thread::sleep(Duration::from_millis(120));
            let _guard = term::lock();
            draw_locked();
        });
    }
}

pub fn stop() {
    let _guard = term::lock();
    erase_locked();
    let mut s = state();
    s.active = false;
    s.note = None;
    s.tokens = 0;
}

pub fn set_tokens(tokens: u64) {
    state().tokens = tokens;
}

pub fn set_note(note: Option<String>) {
    state().note = note;
}

pub fn set_task(task: Option<String>) {
    state().task = task;
}

pub fn set_todos(todos: Vec<(String, String)>) {
    state().todos = todos;
}

pub fn toggle_todos() -> bool {
    !SHOW_TODOS.fetch_xor(true, Ordering::Relaxed)
}

/// The input box and mode for the turn about to run (interactive mode).
pub fn live_begin(mode: Mode, bypass: bool) {
    state().live = Some(Live { buffer: Vec::new(), cursor: 0, queued: Vec::new(), mode, bypass, mode_changed: false });
}

/// The turn is over: the messages queued, what is still being typed, and the mode.
pub fn live_end() -> Option<(Vec<String>, String, Mode)> {
    let _guard = term::lock();
    erase_locked();
    state().live.take().map(|live| (live.queued, live.buffer.into_iter().collect(), live.mode))
}

/// The session's mode, or the user's if they changed it while Claude worked.
pub fn sync_mode(current: Mode) -> Mode {
    let mut s = state();
    match s.live.as_mut() {
        Some(live) if live.mode_changed => {
            live.mode_changed = false;
            live.mode
        }
        Some(live) => {
            live.mode = current;
            current
        }
        None => current,
    }
}

/// A key typed while Claude works (not Esc: that interrupts).
pub fn live_key(key: Key) {
    let mut s = state();
    let Some(live) = s.live.as_mut() else { return };
    match key {
        Key::Char(c) => {
            live.buffer.insert(live.cursor, c);
            live.cursor += 1;
        }
        Key::Paste(text) => {
            for c in text.chars() {
                live.buffer.insert(live.cursor, c);
                live.cursor += 1;
            }
        }
        Key::NewLine => {
            live.buffer.insert(live.cursor, '\n');
            live.cursor += 1;
        }
        Key::Backspace if live.cursor > 0 => {
            live.cursor -= 1;
            live.buffer.remove(live.cursor);
        }
        Key::Delete if live.cursor < live.buffer.len() => {
            live.buffer.remove(live.cursor);
        }
        Key::Left => live.cursor = live.cursor.saturating_sub(1),
        Key::CtrlB => crate::interrupt::request_background(),
        Key::CtrlO => {
            crate::ui::terminal::toggle_verbose();
        }
        Key::CtrlT => {
            toggle_todos();
        }
        Key::Right => live.cursor = (live.cursor + 1).min(live.buffer.len()),
        Key::Home => live.cursor = 0,
        Key::End => live.cursor = live.buffer.len(),
        Key::KillStart => {
            live.buffer.drain(..live.cursor);
            live.cursor = 0;
        }
        Key::KillEnd => live.buffer.truncate(live.cursor),
        Key::DeleteWord => {
            let mut start = live.cursor;
            while start > 0 && live.buffer[start - 1].is_whitespace() {
                start -= 1;
            }
            while start > 0 && !live.buffer[start - 1].is_whitespace() {
                start -= 1;
            }
            live.buffer.drain(start..live.cursor);
            live.cursor = start;
        }
        Key::Enter => {
            let text: String = live.buffer.iter().collect();
            if !text.trim().is_empty() {
                live.queued.push(text.trim_end().to_string());
            }
            live.buffer.clear();
            live.cursor = 0;
        }
        Key::ShiftTab => {
            live.mode = live.mode.next(live.bypass);
            live.mode_changed = true;
        }
        _ => {}
    }
    drop(s);
    // Show the key at once rather than at the next tick.
    let _guard = term::lock();
    draw_locked();
}

/// Clears the panel; the caller holds the terminal lock.
pub fn erase_locked() {
    let mut s = state();
    if s.drawn > 0 {
        let mut text = String::from("\r");
        if s.cursor_row > 0 {
            text.push_str(&format!("\x1b[{}A", s.cursor_row));
        }
        text.push_str("\x1b[J");
        term::raw(&text);
    }
    s.drawn = 0;
    s.cursor_row = 0;
}

/// The input text wrapped to `width` columns, and the cursor's (row, column) in it.
fn wrap_input(buffer: &[char], cursor: usize, width: usize) -> (Vec<String>, usize, usize) {
    let mut rows = vec![String::new()];
    let (mut used, mut at) = (0usize, (0usize, 0usize));
    for (index, &c) in buffer.iter().enumerate() {
        if index == cursor {
            at = (rows.len() - 1, used);
        }
        if c == '\n' {
            rows.push(String::new());
            used = 0;
            continue;
        }
        let w = term::char_width(c);
        if used + w > width {
            rows.push(String::new());
            used = 0;
        }
        rows.last_mut().unwrap().push(c);
        used += w;
    }
    if cursor >= buffer.len() {
        at = (rows.len() - 1, used);
    }
    (rows, at.0, at.1)
}

fn draw_locked() {
    let mut s = state();
    if !s.active || !term::at_line_start() {
        return;
    }
    s.frame = (s.frame + 1) % FRAMES.len();
    let elapsed = s.started.map(|t| t.elapsed()).unwrap_or_default();
    let mut detail = vec![crate::util::duration(elapsed)];
    if s.tokens > 0 {
        detail.push(format!("↓ {} tokens", crate::util::tokens(s.tokens)));
    }
    detail.push("esc to interrupt".into());
    let columns = term::columns();
    let verb = s.task.clone().unwrap_or_else(|| s.verb.clone());
    let line = term::truncate(&format!("{} {}… ({})", FRAMES[s.frame], verb, detail.join(" · ")), columns - 1);
    let (glyph_and_verb, rest) = line.split_once(" (").map(|(a, b)| (a.to_string(), format!(" ({b}"))).unwrap_or((line.clone(), String::new()));
    let mut lines = vec![format!("{}{}", term::accent(&glyph_and_verb), term::dim(&rest))];
    if let Some(note) = &s.note {
        lines.push(term::dim(&term::truncate(&format!("  ⎿  {note}"), columns - 1)));
    }
    if SHOW_TODOS.load(Ordering::Relaxed) && s.todos.iter().any(|(_, status)| status != "completed") {
        for (content, status) in s.todos.iter().take(10) {
            let line = term::truncate(&format!("     {} {content}", if status == "completed" { "☒" } else { "☐" }), columns - 1);
            lines.push(match status.as_str() {
                "completed" => term::dim(&line),
                "in_progress" => term::bold(&line),
                _ => line,
            });
        }
    }
    // The input box: what the user types meanwhile, and the mode.
    let mut cursor = None;
    if let Some(live) = &s.live {
        lines.push(String::new());
        for queued in live.queued.iter().rev().take(3).rev() {
            lines.push(term::dim(&term::truncate(&format!("> {} (queued)", queued.replace('\n', " ")), columns - 1)));
        }
        let text_width = columns.saturating_sub(7).max(10);
        let (rows, row, col) = wrap_input(&live.buffer, live.cursor, text_width);
        lines.push(term::dim(&format!("╭{}╮", "─".repeat(columns.saturating_sub(2)))));
        let top = lines.len();
        for (index, text) in rows.iter().enumerate() {
            let prefix = if index == 0 { "> " } else { "  " };
            let pad = text_width.saturating_sub(term::width(text));
            lines.push(format!("{} {prefix}{text}{} {}", term::dim("│"), " ".repeat(pad + 1), term::dim("│")));
        }
        lines.push(term::dim(&format!("╰{}╯", "─".repeat(columns.saturating_sub(2)))));
        let footer = match live.mode.banner() {
            Some(banner) => {
                let text = format!("  {banner} (shift+tab to cycle)");
                match live.mode {
                    Mode::Plan => term::cyan(&text),
                    Mode::Bypass => term::red(&text),
                    _ => term::magenta(&text),
                }
            }
            None => term::dim(if live.queued.is_empty() { "  enter to queue a message · shift+tab to change mode" } else { "  queued messages are sent when Claude is done" }),
        };
        lines.push(footer);
        cursor = Some((top + row, 4 + col));
    }
    let mut text = lines.join("\r\n");
    let last = lines.len() - 1;
    let cursor_row = match cursor {
        Some((row, col)) => {
            if last > row {
                text.push_str(&format!("\x1b[{}A", last - row));
            }
            text.push_str(&format!("\r\x1b[{col}C"));
            row
        }
        None => last,
    };
    // Replace the previous drawing in one write, to keep the flicker down.
    let erase = match (s.drawn, s.cursor_row) {
        (0, _) => String::new(),
        (_, 0) => "\r\x1b[J".to_string(),
        (_, up) => format!("\r\x1b[{up}A\x1b[J"),
    };
    s.drawn = lines.len();
    s.cursor_row = cursor_row;
    drop(s);
    term::raw(&format!("{erase}{text}"));
}
