//! The live panel under the conversation while Claude works: the spinner (a glyph that
//! breathes, a verb, the time, the tokens, "esc to interrupt", what is happening), messages
//! queued for Claude (read when the current step ends, or at once with send now), the input box
//! the user keeps typing in, and the permission mode (Shift+Tab changes it as Claude works).
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Mutex;
use std::time::{Duration, Instant};

use crate::permissions::Mode;
use crate::term;
use crate::ui::canvas::{synchronized, Canvas};
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
    /// The panel on screen.
    canvas: Canvas,
    verb: String,
    started: Option<Instant>,
    tokens: u64,
    note: Option<String>,
    /// The task in progress (TodoWrite's activeForm), in place of a random verb.
    task: Option<String>,
    frame: usize,
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

static STATE: Mutex<State> = Mutex::new(State {
    active: false,
    canvas: Canvas::new(),
    verb: String::new(),
    started: None,
    tokens: 0,
    note: None,
    task: None,
    frame: 0,
    live: None,
    todos: Vec::new(),
});
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
    crate::interrupt::take_send_now();
    state().live = Some(Live { buffer: Vec::new(), cursor: 0, queued: Vec::new(), mode, bypass, mode_changed: false });
}

/// The turn is over: the messages queued, what is still being typed, and the mode.
pub fn live_end() -> Option<(Vec<String>, String, Mode)> {
    let _guard = term::lock();
    erase_locked();
    state().live.take().map(|live| (live.queued, live.buffer.into_iter().collect(), live.mode))
}

/// The messages queued so far, taken for the model to read now (between two steps of a turn).
pub fn take_queued() -> Vec<String> {
    let queued = state().live.as_mut().map(|live| std::mem::take(&mut live.queued)).unwrap_or_default();
    if !queued.is_empty() {
        let _guard = term::lock();
        draw_locked();
    }
    queued
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
        Key::Enter | Key::SendNow => {
            let text: String = live.buffer.iter().collect();
            if !text.trim().is_empty() {
                live.queued.push(text.trim_end().to_string());
            }
            live.buffer.clear();
            live.cursor = 0;
            if key == Key::SendNow && !live.queued.is_empty() {
                crate::interrupt::send_now();
            }
        }
        // Up on the input's first line: the queued messages come back into it to be edited.
        Key::Up if !live.queued.is_empty() && !live.buffer[..live.cursor].contains(&'\n') => {
            let mut text: Vec<char> = live.queued.drain(..).collect::<Vec<_>>().join("\n").chars().collect();
            live.cursor = text.len();
            if !live.buffer.is_empty() {
                text.push('\n');
            }
            text.extend(live.buffer.drain(..));
            live.buffer = text;
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
    let text = state().canvas.erase();
    if !text.is_empty() {
        term::raw(&text);
    }
}

/// `text` written where the panel is, and the panel drawn again under it, in one write: over the
/// old panel rather than after clearing it, so that it is never seen gone (the terminal shows
/// what comes as it comes, and the guest's terminal hands the bytes on a line at a time).
/// The caller holds the terminal lock.
pub fn around_locked(text: &str) -> String {
    let mut s = state();
    if !s.canvas.is_drawn() {
        return text.to_string();
    }
    let columns = term::columns();
    // Each line over the old panel clears what is left of it.
    let mut out = s.canvas.to_top();
    out.push_str(&text.replace("\r\n", "\n").replace('\n', "\x1b[K\n"));
    s.canvas.forget();
    if s.active && text.ends_with('\n') {
        let (lines, cursor) = compose(&s, columns);
        out.push_str(&s.canvas.fresh(lines, cursor, columns));
    } else {
        out.push_str("\x1b[J");
    }
    synchronized(&out)
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
    let columns = term::columns();
    let (lines, cursor) = compose(&s, columns);
    // About once a second every line again, in case something else wrote over the panel.
    let every = s.frame == 0;
    let text = s.canvas.draw(lines, cursor, columns, every);
    drop(s);
    if !text.is_empty() {
        term::raw(&synchronized(&text));
    }
}

/// The panel's lines, and where the cursor goes in them (row, column).
fn compose(s: &State, columns: usize) -> (Vec<String>, (usize, usize)) {
    let elapsed = s.started.map(|t| t.elapsed()).unwrap_or_default();
    let mut detail = vec![crate::util::duration(elapsed)];
    if s.tokens > 0 {
        detail.push(format!("↓ {} tokens", crate::util::tokens(s.tokens)));
    }
    detail.push("esc to interrupt".into());
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
                let text = term::truncate(&format!("  {banner} (shift+tab to cycle)"), columns - 1);
                match live.mode {
                    Mode::Plan => term::cyan(&text),
                    Mode::Bypass => term::red(&text),
                    _ => term::magenta(&text),
                }
            }
            None => term::dim(&term::truncate(
                if live.queued.is_empty() { "  enter to queue a message · shift+tab to change mode" } else { "  queued: Claude reads them after this step · ctrl+x ctrl+s to send now · ↑ to edit" },
                columns - 1,
            )),
        };
        lines.push(footer);
        cursor = Some((top + row, 4 + col));
    }
    // Without the input box the cursor rests after the last line.
    let last = lines.len() - 1;
    let cursor = cursor.unwrap_or((last, term::width(&lines[last])));
    (lines, cursor)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn typed(text: &str) {
        text.chars().for_each(|c| live_key(Key::Char(c)));
    }

    #[test]
    fn queue() {
        live_begin(Mode::Default, false);
        typed("one");
        live_key(Key::Enter);
        typed("two");
        live_key(Key::Enter);
        // Up brings the queued messages back into the input box, before what is being typed.
        typed("three");
        live_key(Key::Home);
        live_key(Key::Up);
        assert!(take_queued().is_empty());
        live_key(Key::End);
        live_key(Key::Enter);
        typed("four");
        live_key(Key::Enter);
        assert_eq!(take_queued(), ["one\ntwo\nthree", "four"]);
        assert!(take_queued().is_empty());
        typed("draft");
        let (queued, draft, _) = live_end().unwrap();
        assert!(queued.is_empty());
        assert_eq!(draft, "draft");
    }
}
