//! The input box: a line editor drawn as Claude Code draws it.
//!
//!   ╭──────────────────────────────────────╮
//!   │ > what the user types, wrapped        │
//!   ╰──────────────────────────────────────╯
//!     ? for shortcuts            ✻ thinking on
//!
//! Keys: ←→ ↑↓ (history on the first/last line), Home/End, Ctrl-A/E/B/F/K/U/W/L, Alt-B/F,
//! Enter (a line ending in \ continues), Alt-Enter / Ctrl-J (new line), Tab (complete, or
//! thinking on/off), Shift-Tab (permission mode), Esc Esc (clear), Ctrl-C (clear; on an
//! empty line, leave twice), Ctrl-D (leave on an empty line), pasted text as is. "/" lists
//! commands, "@" completes paths, "!" runs a command, "#" adds to memory.
use std::collections::VecDeque;
use std::path::{Path, PathBuf};

use crate::term;

#[derive(Debug, PartialEq, Clone)]
pub enum Key {
    Char(char),
    Enter,
    NewLine,
    Backspace,
    Delete,
    Left,
    Right,
    Up,
    Down,
    Home,
    End,
    WordLeft,
    WordRight,
    DeleteWord,
    KillEnd,
    KillStart,
    Tab,
    ShiftTab,
    Esc,
    CtrlC,
    CtrlD,
    CtrlL,
    /// Ctrl-B: a cursor move in the input box; while a command runs, "run it in the background".
    CtrlB,
    /// Ctrl-O: verbose output on/off.
    CtrlO,
    /// Ctrl-R: search the history.
    CtrlR,
    /// Ctrl-T: the todo list on/off.
    CtrlT,
    Paste(String),
    Unknown,
}

/// Decodes keys from stdin (raw mode).
#[derive(Default)]
pub struct Keys {
    pending: VecDeque<u8>,
}

impl Keys {
    fn byte(&mut self, wait_ms: Option<i32>) -> Option<u8> {
        if let Some(byte) = self.pending.pop_front() {
            return Some(byte);
        }
        if let Some(ms) = wait_ms {
            if !term::stdin_ready(ms) {
                return None;
            }
        }
        let mut buffer = [0u8; 4096];
        loop {
            match term::read_stdin(&mut buffer) {
                Some(read) => {
                    self.pending.extend(&buffer[..read]);
                    return self.pending.pop_front();
                }
                None if std::io::Error::last_os_error().kind() == std::io::ErrorKind::Interrupted => {
                    if crate::interrupt::take() {
                        return Some(3);
                    }
                    continue;
                }
                None => return None,
            }
        }
    }

    /// Bytes already read and not yet decoded (a burst of typing, a paste).
    pub fn has_pending(&self) -> bool {
        !self.pending.is_empty()
    }

    /// Hands the undecoded bytes over (typed right after Enter: they belong to whoever reads
    /// the keys next).
    pub fn take_pending(&mut self) -> Vec<u8> {
        self.pending.drain(..).collect()
    }

    pub fn give(&mut self, bytes: Vec<u8>) {
        self.pending.extend(bytes);
    }

    /// The next key; None at end of input.
    pub fn read(&mut self) -> Option<Key> {
        let byte = self.byte(None)?;
        Some(match byte {
            b'\r' => Key::Enter,
            b'\n' => Key::NewLine,
            0x7f | 0x08 => Key::Backspace,
            b'\t' => Key::Tab,
            1 => Key::Home,
            2 => Key::CtrlB,
            3 => Key::CtrlC,
            4 => Key::CtrlD,
            5 => Key::End,
            6 => Key::Right,
            11 => Key::KillEnd,
            12 => Key::CtrlL,
            14 => Key::Down,
            15 => Key::CtrlO,
            16 => Key::Up,
            18 => Key::CtrlR,
            20 => Key::CtrlT,
            21 => Key::KillStart,
            23 => Key::DeleteWord,
            0x1b => self.escape(),
            b if b < 0x20 => Key::Unknown,
            b if b < 0x80 => Key::Char(b as char),
            b => {
                let length = if b >= 0xf0 { 4 } else if b >= 0xe0 { 3 } else { 2 };
                let mut bytes = vec![b];
                for _ in 1..length {
                    match self.byte(Some(50)) {
                        Some(next) => bytes.push(next),
                        None => break,
                    }
                }
                String::from_utf8(bytes).ok().and_then(|s| s.chars().next()).map_or(Key::Unknown, Key::Char)
            }
        })
    }

    fn escape(&mut self) -> Key {
        let Some(next) = self.byte(Some(30)) else { return Key::Esc };
        match next {
            b'[' | b'O' => {
                let mut sequence = Vec::new();
                while let Some(byte) = self.byte(Some(30)) {
                    sequence.push(byte);
                    if (0x40..=0x7e).contains(&byte) {
                        break;
                    }
                }
                let text = String::from_utf8_lossy(&sequence).into_owned();
                match text.as_str() {
                    "A" => Key::Up,
                    "B" => Key::Down,
                    "C" => Key::Right,
                    "D" => Key::Left,
                    "H" | "1~" | "7~" => Key::Home,
                    "F" | "4~" | "8~" => Key::End,
                    "3~" => Key::Delete,
                    "Z" => Key::ShiftTab,
                    "1;5C" | "1;3C" => Key::WordRight,
                    "1;5D" | "1;3D" => Key::WordLeft,
                    "13;2u" | "13;3u" => Key::NewLine,
                    "200~" => self.paste(),
                    _ => Key::Unknown,
                }
            }
            b'\r' | b'\n' => Key::NewLine,
            b'b' => Key::WordLeft,
            b'f' => Key::WordRight,
            0x7f => Key::DeleteWord,
            0x1b => {
                // Esc Esc
                self.pending.push_front(0x1b);
                Key::Esc
            }
            other => {
                self.pending.push_front(other);
                Key::Esc
            }
        }
    }

    fn paste(&mut self) -> Key {
        let mut bytes = Vec::new();
        while let Some(byte) = self.byte(Some(500)) {
            bytes.push(byte);
            if bytes.ends_with(b"\x1b[201~") {
                bytes.truncate(bytes.len() - 6);
                break;
            }
        }
        Key::Paste(String::from_utf8_lossy(&bytes).replace("\r\n", "\n").replace('\r', "\n"))
    }
}

pub enum Read {
    Line(String),
    /// Esc Esc on an empty line: go back to an earlier message (/rewind).
    Rewind,
    /// Ctrl-C on an empty line.
    Cancel,
    Eof,
}

/// What the editor asks of its host while it runs.
pub trait Host {
    /// Slash commands (name, description) for the menu.
    fn commands(&self) -> Vec<(String, String)>;
    /// Shift-Tab: the next permission mode; the new footer text.
    fn cycle_mode(&mut self);
    /// Tab on an empty line: thinking on/off.
    fn toggle_thinking(&mut self);
    /// (left, right) of the footer.
    fn footer(&mut self) -> (String, String);
    fn cwd(&self) -> PathBuf;
    /// Vim keys in the input box (settings editorMode "vim", or /vim).
    fn vim(&self) -> bool {
        false
    }
}

pub struct Editor {
    pub history: Vec<String>,
    history_path: Option<PathBuf>,
    pub keys: Keys,
    buffer: Vec<char>,
    cursor: usize,
    browsing: Option<usize>,
    draft: Vec<char>,
    /// Rows from the top of the drawn region to the cursor's row.
    cursor_row: usize,
    selected: usize,
    show_help: bool,
    last_esc: bool,
    /// Ctrl-R: the search text and the history entry it found.
    search: Option<(String, Option<usize>)>,
    /// Vim: in normal mode (else insert), an operator waiting for its motion (d, c, y), the
    /// last deleted or yanked text, and what u undoes.
    vim_normal: bool,
    vim_pending: Option<char>,
    yank: Vec<char>,
    undo: Vec<(Vec<char>, usize)>,
}

const HELP: [(&str, &str); 8] = [
    ("! for bash mode", "double tap esc to clear input"),
    ("/ for commands", "shift + tab to cycle modes"),
    ("@ for file paths", "tab to toggle thinking"),
    ("# to memorize", "\\⏎ or alt+⏎ for newline"),
    ("", "ctrl + c twice to exit"),
    ("", "↑↓ for history"),
    ("", ""),
    ("", ""),
];

fn word_boundary_left(chars: &[char], mut at: usize) -> usize {
    while at > 0 && chars[at - 1].is_whitespace() {
        at -= 1;
    }
    while at > 0 && !chars[at - 1].is_whitespace() {
        at -= 1;
    }
    at
}

fn word_boundary_right(chars: &[char], mut at: usize) -> usize {
    while at < chars.len() && chars[at].is_whitespace() {
        at += 1;
    }
    while at < chars.len() && !chars[at].is_whitespace() {
        at += 1;
    }
    at
}

/// Path completions for a partial path after @.
pub fn complete_path(cwd: &Path, partial: &str) -> Vec<String> {
    let (dir_part, prefix) = match partial.rfind('/') {
        Some(i) => (&partial[..=i], &partial[i + 1..]),
        None => ("", partial),
    };
    let dir = match dir_part {
        "" => cwd.to_path_buf(),
        d if d.starts_with('/') => PathBuf::from(d),
        d => cwd.join(d),
    };
    let mut found: Vec<String> = std::fs::read_dir(dir)
        .into_iter()
        .flatten()
        .flatten()
        .filter_map(|entry| {
            let name = entry.file_name().to_string_lossy().into_owned();
            if !name.to_lowercase().starts_with(&prefix.to_lowercase()) || (name.starts_with('.') && !prefix.starts_with('.')) {
                return None;
            }
            if matches!(name.as_str(), "node_modules" | ".git") {
                return None;
            }
            let dir = entry.file_type().is_ok_and(|t| t.is_dir());
            Some(format!("{dir_part}{name}{}", if dir { "/" } else { "" }))
        })
        .collect();
    found.sort();
    found.truncate(8);
    found
}

impl Editor {
    pub fn new(history_path: Option<PathBuf>) -> Editor {
        let history = history_path
            .as_ref()
            .and_then(|path| std::fs::read_to_string(path).ok())
            .map(|text| text.lines().filter_map(|line| serde_json::from_str::<String>(line).ok()).collect())
            .unwrap_or_default();
        Editor { history, history_path, keys: Keys::default(), buffer: Vec::new(), cursor: 0, browsing: None, draft: Vec::new(), cursor_row: 0, selected: 0, show_help: false, last_esc: false, search: None, vim_normal: false, vim_pending: None, yank: Vec::new(), undo: Vec::new() }
    }

    fn remember(&mut self, line: &str) {
        if line.trim().is_empty() || self.history.last().is_some_and(|last| last == line) {
            return;
        }
        self.history.push(line.to_string());
        if let Some(path) = &self.history_path {
            if let Some(dir) = path.parent() {
                let _ = std::fs::create_dir_all(dir);
            }
            use std::io::Write;
            if let Ok(mut file) = std::fs::OpenOptions::new().create(true).append(true).open(path) {
                let _ = writeln!(file, "{}", serde_json::to_string(line).unwrap_or_default());
            }
        }
    }

    fn text(&self) -> String {
        self.buffer.iter().collect()
    }

    /// The menu under the box: slash commands or path completions.
    fn suggestions(&self, host: &dyn Host) -> Vec<(String, String)> {
        let text = self.text();
        if let Some(command) = text.strip_prefix('/') {
            if command.contains(char::is_whitespace) || self.cursor != self.buffer.len() {
                return Vec::new();
            }
            let command = command.to_lowercase();
            let bare = |name: &str| name.trim_start_matches('/').to_lowercase();
            let mut matches: Vec<(String, String)> = host.commands().into_iter().filter(|(name, _)| bare(name).starts_with(&command)).collect();
            if matches.is_empty() {
                matches = host.commands().into_iter().filter(|(name, _)| bare(name).contains(&command)).collect();
            }
            matches.truncate(8);
            return matches;
        }
        // @path under the cursor.
        let before: String = self.buffer[..self.cursor].iter().collect();
        if let Some(at) = before.rfind('@') {
            let partial = &before[at + 1..];
            if (at == 0 || before[..at].ends_with(char::is_whitespace)) && !partial.contains(char::is_whitespace) {
                return complete_path(&host.cwd(), partial).into_iter().map(|path| (format!("@{path}"), String::new())).collect();
            }
        }
        Vec::new()
    }

    fn accept_suggestion(&mut self, choice: &str) {
        if choice.starts_with('/') {
            self.buffer = format!("{choice} ").chars().collect();
            self.cursor = self.buffer.len();
            return;
        }
        let before: String = self.buffer[..self.cursor].iter().collect();
        if let Some(at) = before.rfind('@') {
            let start = before[..at].chars().count();
            let mut replacement: Vec<char> = choice.chars().collect();
            if !choice.ends_with('/') {
                replacement.push(' ');
            }
            let tail: Vec<char> = self.buffer[self.cursor..].to_vec();
            self.buffer.truncate(start);
            self.buffer.extend(&replacement);
            self.cursor = self.buffer.len();
            self.buffer.extend(tail);
        }
    }

    /// Visual lines of the input (wrapped to `width`) and where the cursor is in them.
    fn layout(&self, width: usize) -> (Vec<String>, usize, usize) {
        let mut rows = vec![String::new()];
        let (mut used, mut cursor) = (0usize, (0usize, 0usize));
        for (index, &c) in self.buffer.iter().enumerate() {
            if index == self.cursor {
                cursor = (rows.len() - 1, used);
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
        if self.cursor == self.buffer.len() {
            if used >= width {
                rows.push(String::new());
                used = 0;
            }
            cursor = (rows.len() - 1, used);
        }
        (rows, cursor.0, cursor.1)
    }

    fn render(&mut self, host: &mut dyn Host, suggestions: &[(String, String)]) {
        let columns = term::columns().max(20);
        let inner = columns - 4;
        let text_width = inner - 3;
        let (mode_char, border): (&str, fn(&str) -> String) = match self.buffer.first() {
            Some('!') => ("!", term::magenta),
            Some('#') => ("#", term::blue),
            _ => (">", term::dim),
        };
        let (rows, cursor_row, cursor_col) = self.layout(text_width);
        let mut lines = vec![border(&format!("╭{}╮", "─".repeat(columns - 2)))];
        for (index, row) in rows.iter().enumerate() {
            let prefix = if index == 0 { format!("{mode_char} ") } else { "  ".to_string() };
            let pad = text_width.saturating_sub(term::width(row));
            lines.push(format!("{} {}{}{} {}", border("│"), if index == 0 { match mode_char { ">" => prefix.clone(), _ => border(&prefix) } } else { prefix.clone() }, row, " ".repeat(pad + 1), border("│")));
        }
        lines.push(border(&format!("╰{}╯", "─".repeat(columns - 2))));
        if !suggestions.is_empty() {
            let name_width = suggestions.iter().map(|(name, _)| term::width(name)).max().unwrap_or(0).min(columns / 2);
            for (index, (name, description)) in suggestions.iter().enumerate() {
                let line = term::truncate(&format!("  {:<name_width$}  {description}", name), columns - 1);
                lines.push(if index == self.selected { term::accent(&line) } else { term::dim(&line) });
            }
        } else if let Some((query, found)) = &self.search {
            let state = if found.is_none() && !query.is_empty() { "failing " } else { "" };
            lines.push(term::accent(&format!("  ({state}reverse-i-search)`{query}': enter to use · ctrl+r older · esc cancel")));
        } else if self.show_help {
            for (left, right) in HELP.iter().filter(|(l, r)| !l.is_empty() || !r.is_empty()) {
                lines.push(term::dim(&format!("  {left:<30}{right}")));
            }
        } else {
            let (left, right) = host.footer();
            let left = match (host.vim(), self.vim_normal) {
                (true, true) => format!("{}  {left}", term::bold("-- NORMAL --")),
                (true, false) => format!("{}  {left}", term::dim("-- INSERT --")),
                _ => left,
            };
            let left = match self.buffer.first() {
                Some('!') => term::magenta("  ! for bash mode"),
                Some('#') => term::blue("  # to memorize"),
                _ => format!("  {left}"),
            };
            let gap = columns.saturating_sub(term::width(&left) + term::width(&right) + 2);
            lines.push(format!("{left}{}{right}", " ".repeat(gap)));
        }
        // Back to the top of the last drawing, clear, draw, and put the cursor in the box.
        let mut out = String::from("\r");
        if self.cursor_row > 0 {
            out.push_str(&format!("\x1b[{}A", self.cursor_row));
        }
        out.push_str("\x1b[J");
        out.push_str(&lines.join("\r\n"));
        let target_row = 1 + cursor_row;
        let up = lines.len() - 1 - target_row;
        if up > 0 {
            out.push_str(&format!("\x1b[{up}A"));
        }
        out.push_str(&format!("\r\x1b[{}C", 4 + cursor_col));
        let _guard = term::lock();
        term::raw(&out);
        self.cursor_row = target_row;
    }

    /// Removes the drawing, leaving the cursor where it began.
    fn clear(&mut self) {
        let mut out = String::from("\r");
        if self.cursor_row > 0 {
            out.push_str(&format!("\x1b[{}A", self.cursor_row));
        }
        out.push_str("\x1b[J");
        let _guard = term::lock();
        term::raw(&out);
        self.cursor_row = 0;
    }

    fn history_step(&mut self, back: bool) {
        if self.history.is_empty() {
            return;
        }
        let next = match (self.browsing, back) {
            (None, true) => {
                self.draft = self.buffer.clone();
                Some(self.history.len() - 1)
            }
            (None, false) => return,
            (Some(0), true) => Some(0),
            (Some(i), true) => Some(i - 1),
            (Some(i), false) if i + 1 < self.history.len() => Some(i + 1),
            (Some(_), false) => None,
        };
        self.browsing = next;
        self.buffer = match next {
            Some(i) => self.history[i].chars().collect(),
            None => std::mem::take(&mut self.draft),
        };
        self.cursor = self.buffer.len();
    }

    fn insert(&mut self, text: &str) {
        for c in text.chars() {
            self.buffer.insert(self.cursor, c);
            self.cursor += 1;
        }
    }

    /// Reads one input. `prefill` starts the line (typed ahead while Claude worked).
    pub fn read(&mut self, host: &mut dyn Host, prefill: &str) -> Read {
        let _mode = term::Mode::raw();
        term::raw("\x1b[?2004h");
        self.buffer = prefill.chars().collect();
        self.cursor = self.buffer.len();
        self.browsing = None;
        self.cursor_row = 0;
        self.selected = 0;
        self.show_help = false;
        self.search = None;
        self.vim_normal = false;
        self.vim_pending = None;
        self.undo.clear();
        let result = loop {
            let suggestions = self.suggestions(host);
            if self.selected >= suggestions.len() {
                self.selected = 0;
            }
            self.render(host, &suggestions);
            let Some(key) = self.keys.read() else { break Read::Eof };
            if host.vim() {
                if self.vim_normal {
                    match self.vim_key(&key) {
                        Some(true) => {
                            let line = self.text();
                            break Read::Line(line);
                        }
                        Some(false) => continue,
                        None => {}
                    }
                } else if key == Key::Esc {
                    self.vim_normal = true;
                    self.cursor = self.cursor.saturating_sub(1).max(self.line_start(self.cursor));
                    continue;
                }
            }
            // Ctrl-R: search the history as the user types.
            if let Some((query, found)) = self.search.as_mut() {
                let find = |history: &[String], query: &str, before: usize| (0..before).rev().find(|&i| history[i].contains(query));
                match key {
                    Key::Char(c) => {
                        query.push(c);
                        *found = find(&self.history, query, found.map_or(self.history.len(), |i| i + 1));
                    }
                    Key::Backspace => {
                        query.pop();
                        *found = find(&self.history, query, self.history.len());
                    }
                    Key::CtrlR => {
                        if let Some(older) = find(&self.history, query, found.unwrap_or(self.history.len())) {
                            *found = Some(older);
                        }
                    }
                    Key::Esc | Key::CtrlC => {
                        self.buffer = std::mem::take(&mut self.draft);
                        self.cursor = self.buffer.len();
                        self.search = None;
                        continue;
                    }
                    _ => {
                        // Anything else takes the match and goes on editing (Enter just takes it).
                        self.search = None;
                        continue;
                    }
                }
                if let Some((_, Some(index))) = &self.search {
                    self.buffer = self.history[*index].chars().collect();
                    self.cursor = self.buffer.len();
                }
                continue;
            }
            if key != Key::Esc {
                self.last_esc = false;
            }
            if key != Key::Char('?') {
                self.show_help = false;
            }
            match key {
                Key::Enter => {
                    if let Some((choice, _)) = suggestions.get(self.selected) {
                        let exact = self.text().trim_end() == choice.as_str();
                        if choice.starts_with('/') && !exact {
                            self.buffer = choice.chars().collect();
                            self.cursor = self.buffer.len();
                        } else if choice.starts_with('@') {
                            self.accept_suggestion(&choice.clone());
                            continue;
                        }
                    }
                    if self.cursor > 0 && self.buffer[self.cursor - 1] == '\\' && self.cursor == self.buffer.len() {
                        self.buffer[self.cursor - 1] = '\n';
                        continue;
                    }
                    let line = self.text();
                    break Read::Line(line);
                }
                Key::NewLine => self.insert("\n"),
                Key::Char('?') if self.buffer.is_empty() => self.show_help = !self.show_help,
                Key::Char(c) => self.insert(&c.to_string()),
                Key::Paste(text) => self.insert(&text),
                Key::Backspace => {
                    if self.cursor > 0 {
                        self.cursor -= 1;
                        self.buffer.remove(self.cursor);
                    }
                }
                Key::Delete => {
                    if self.cursor < self.buffer.len() {
                        self.buffer.remove(self.cursor);
                    }
                }
                Key::Left | Key::CtrlB => self.cursor = self.cursor.saturating_sub(1),
                Key::Right => self.cursor = (self.cursor + 1).min(self.buffer.len()),
                Key::Home => {
                    while self.cursor > 0 && self.buffer[self.cursor - 1] != '\n' {
                        self.cursor -= 1;
                    }
                }
                Key::End => {
                    while self.cursor < self.buffer.len() && self.buffer[self.cursor] != '\n' {
                        self.cursor += 1;
                    }
                }
                Key::WordLeft => self.cursor = word_boundary_left(&self.buffer, self.cursor),
                Key::WordRight => self.cursor = word_boundary_right(&self.buffer, self.cursor),
                Key::DeleteWord => {
                    let start = word_boundary_left(&self.buffer, self.cursor);
                    self.buffer.drain(start..self.cursor);
                    self.cursor = start;
                }
                Key::KillEnd => {
                    let end = (self.cursor..self.buffer.len()).find(|&i| self.buffer[i] == '\n').unwrap_or(self.buffer.len());
                    self.buffer.drain(self.cursor..end);
                }
                Key::KillStart => {
                    let start = (0..self.cursor).rev().find(|&i| self.buffer[i] == '\n').map_or(0, |i| i + 1);
                    self.buffer.drain(start..self.cursor);
                    self.cursor = start;
                }
                Key::Up if !suggestions.is_empty() => self.selected = (self.selected + suggestions.len() - 1) % suggestions.len(),
                Key::Down if !suggestions.is_empty() => self.selected = (self.selected + 1) % suggestions.len(),
                Key::Up => {
                    let (rows, row, col) = self.layout(term::columns().max(20) - 7);
                    let _ = rows;
                    if row == 0 {
                        self.history_step(true);
                    } else {
                        // Up a visual line, near the same column.
                        let target = (row - 1, col);
                        self.move_to(target);
                    }
                }
                Key::Down => {
                    let (rows, row, col) = self.layout(term::columns().max(20) - 7);
                    if row + 1 >= rows.len() {
                        self.history_step(false);
                    } else {
                        self.move_to((row + 1, col));
                    }
                }
                Key::Tab => match suggestions.get(self.selected) {
                    Some((choice, _)) => {
                        let choice = choice.clone();
                        self.accept_suggestion(&choice);
                    }
                    None if self.buffer.is_empty() => host.toggle_thinking(),
                    None => {}
                },
                Key::ShiftTab => host.cycle_mode(),
                Key::CtrlR => {
                    self.draft = self.buffer.clone();
                    self.search = Some((String::new(), None));
                }
                Key::CtrlO => {
                    crate::ui::terminal::toggle_verbose();
                }
                Key::CtrlT => {
                    crate::ui::spinner::toggle_todos();
                }
                Key::Esc => {
                    if self.last_esc && self.buffer.is_empty() {
                        break Read::Rewind;
                    } else if self.last_esc {
                        self.buffer.clear();
                        self.cursor = 0;
                        self.last_esc = false;
                    } else {
                        self.last_esc = true;
                    }
                }
                Key::CtrlC => {
                    if self.buffer.is_empty() {
                        break Read::Cancel;
                    }
                    self.buffer.clear();
                    self.cursor = 0;
                }
                Key::CtrlD => {
                    if self.buffer.is_empty() {
                        break Read::Eof;
                    }
                    if self.cursor < self.buffer.len() {
                        self.buffer.remove(self.cursor);
                    }
                }
                Key::CtrlL => {
                    term::raw("\x1b[2J\x1b[H");
                    self.cursor_row = 0;
                }
                Key::Unknown => {}
            }
        };
        self.clear();
        term::raw("\x1b[?2004l");
        term::set_line_start(true);
        if let Read::Line(line) = &result {
            self.remember(line);
        }
        result
    }

    fn line_start(&self, at: usize) -> usize {
        (0..at).rev().find(|&i| self.buffer[i] == '\n').map_or(0, |i| i + 1)
    }

    fn line_end(&self, at: usize) -> usize {
        (at..self.buffer.len()).find(|&i| self.buffer[i] == '\n').unwrap_or(self.buffer.len())
    }

    /// Vim's w: past this word and the blanks after it.
    fn next_word_start(&self, at: usize) -> usize {
        let mut i = at;
        while i < self.buffer.len() && !self.buffer[i].is_whitespace() {
            i += 1;
        }
        while i < self.buffer.len() && self.buffer[i].is_whitespace() && self.buffer[i] != '\n' {
            i += 1;
        }
        i
    }

    fn word_end(&self, at: usize) -> usize {
        let mut i = at + 1;
        while i < self.buffer.len() && self.buffer[i].is_whitespace() {
            i += 1;
        }
        while i + 1 < self.buffer.len() && !self.buffer[i + 1].is_whitespace() {
            i += 1;
        }
        i.min(self.buffer.len().saturating_sub(1))
    }

    fn save_undo(&mut self) {
        self.undo.push((self.buffer.clone(), self.cursor));
        if self.undo.len() > 100 {
            self.undo.remove(0);
        }
    }

    fn cut(&mut self, from: usize, to: usize) {
        let (from, to) = (from.min(to), from.max(to).min(self.buffer.len()));
        self.yank = self.buffer.drain(from..to).collect();
        self.cursor = from.min(self.buffer.len());
    }

    /// A key in vim's normal mode. Some(true): submit; Some(false): handled; None: not a
    /// normal-mode key (arrows and the like fall through to the usual handling).
    fn vim_key(&mut self, key: &Key) -> Option<bool> {
        let c = match key {
            Key::Char(c) => *c,
            Key::Enter => return Some(true),
            Key::Esc => {
                self.vim_pending = None;
                return None;
            }
            _ => return None,
        };
        if let Some(op) = self.vim_pending.take() {
            let (start, end) = match (op, c) {
                (o, m) if o == m => {
                    // dd, cc, yy: the whole line (with its newline, for dd).
                    let start = self.line_start(self.cursor);
                    let mut end = self.line_end(self.cursor);
                    if op == 'd' && end < self.buffer.len() {
                        end += 1;
                    }
                    (start, end)
                }
                (_, 'w') => (self.cursor, if op == 'c' { self.word_end(self.cursor) + 1 } else { self.next_word_start(self.cursor) }),
                (_, 'e') => (self.cursor, self.word_end(self.cursor) + 1),
                (_, 'b') => (word_boundary_left(&self.buffer, self.cursor), self.cursor),
                (_, '$') => (self.cursor, self.line_end(self.cursor)),
                (_, '0') => (self.line_start(self.cursor), self.cursor),
                _ => return Some(false),
            };
            match op {
                'y' => self.yank = self.buffer[start..end.min(self.buffer.len())].to_vec(),
                _ => {
                    self.save_undo();
                    self.cut(start, end);
                    if op == 'c' {
                        self.vim_normal = false;
                    }
                }
            }
            return Some(false);
        }
        let len = self.buffer.len();
        match c {
            'i' => self.vim_normal = false,
            'a' => {
                self.cursor = (self.cursor + 1).min(self.line_end(self.cursor));
                self.vim_normal = false;
            }
            'A' => {
                self.cursor = self.line_end(self.cursor);
                self.vim_normal = false;
            }
            'I' | '^' => {
                let start = self.line_start(self.cursor);
                self.cursor = (start..self.line_end(start)).find(|&i| !self.buffer[i].is_whitespace()).unwrap_or(start);
                if c == 'I' {
                    self.vim_normal = false;
                }
            }
            'o' | 'O' => {
                self.save_undo();
                let at = if c == 'o' { self.line_end(self.cursor) } else { self.line_start(self.cursor) };
                self.buffer.insert(at, '\n');
                self.cursor = if c == 'o' { at + 1 } else { at };
                self.vim_normal = false;
            }
            'h' => self.cursor = self.cursor.saturating_sub(1).max(self.line_start(self.cursor)),
            'l' => self.cursor = (self.cursor + 1).min(self.line_end(self.cursor).saturating_sub(1).max(self.line_start(self.cursor))),
            '0' => self.cursor = self.line_start(self.cursor),
            '$' => self.cursor = self.line_end(self.cursor).saturating_sub(1).max(self.line_start(self.cursor)),
            'w' => self.cursor = self.next_word_start(self.cursor).min(len.saturating_sub(1)),
            'b' => self.cursor = word_boundary_left(&self.buffer, self.cursor),
            'e' => self.cursor = self.word_end(self.cursor),
            'x' if self.cursor < len => {
                self.save_undo();
                let at = self.cursor;
                self.cut(at, at + 1);
            }
            'X' if self.cursor > self.line_start(self.cursor) => {
                self.save_undo();
                let at = self.cursor;
                self.cut(at - 1, at);
            }
            'D' | 'C' => {
                self.save_undo();
                let (at, end) = (self.cursor, self.line_end(self.cursor));
                self.cut(at, end);
                if c == 'C' {
                    self.vim_normal = false;
                }
            }
            'd' | 'c' | 'y' => self.vim_pending = Some(c),
            'p' | 'P' => {
                self.save_undo();
                let at = if c == 'p' && self.cursor < len { self.cursor + 1 } else { self.cursor };
                for (i, ch) in self.yank.clone().into_iter().enumerate() {
                    self.buffer.insert(at + i, ch);
                }
                self.cursor = at + self.yank.len().saturating_sub(1);
            }
            'u' => {
                if let Some((buffer, cursor)) = self.undo.pop() {
                    self.buffer = buffer;
                    self.cursor = cursor.min(self.buffer.len());
                }
            }
            'j' => self.vim_move(false),
            'k' => {
                self.vim_move(true);
            }
            _ => {}
        }
        Some(false)
    }

    /// j / k: a line down or up, or through the history at the ends.
    fn vim_move(&mut self, up: bool) {
        let start = self.line_start(self.cursor);
        let column = self.cursor - start;
        if up {
            if start == 0 {
                self.history_step(true);
                return;
            }
            let previous = self.line_start(start - 1);
            self.cursor = (previous + column).min(start - 1);
        } else {
            let end = self.line_end(self.cursor);
            if end >= self.buffer.len() {
                self.history_step(false);
                return;
            }
            let next = end + 1;
            self.cursor = (next + column).min(self.line_end(next));
        }
    }

    fn move_to(&mut self, (row, col): (usize, usize)) {
        let width = term::columns().max(20) - 7;
        // Walk the layout to the character at (row, col).
        let (mut r, mut used) = (0usize, 0usize);
        for (index, &c) in self.buffer.iter().enumerate() {
            if r == row && used >= col {
                self.cursor = index;
                return;
            }
            if c == '\n' {
                if r == row {
                    self.cursor = index;
                    return;
                }
                r += 1;
                used = 0;
                continue;
            }
            let w = term::char_width(c);
            if used + w > width {
                if r == row {
                    self.cursor = index;
                    return;
                }
                r += 1;
                used = 0;
            }
            used += w;
        }
        self.cursor = self.buffer.len();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn keys(bytes: &[u8]) -> Vec<Key> {
        let mut k = Keys::default();
        k.pending.extend(bytes);
        let mut out = Vec::new();
        while !k.pending.is_empty() {
            out.push(k.read().unwrap());
        }
        out
    }

    #[test]
    fn decoding() {
        assert_eq!(keys(b"a\r"), [Key::Char('a'), Key::Enter]);
        assert_eq!(keys("한".as_bytes()), [Key::Char('한')]);
        assert_eq!(keys(b"\x1b[A\x1b[Z\x1b[3~\x1b\r\x1bb"), [Key::Up, Key::ShiftTab, Key::Delete, Key::NewLine, Key::WordLeft]);
        assert_eq!(keys(b"\x1b[200~line1\r\nline2\x1b[201~x"), [Key::Paste("line1\nline2".into()), Key::Char('x')]);
        assert_eq!(keys(b"\x03\x04\x17"), [Key::CtrlC, Key::CtrlD, Key::DeleteWord]);
    }

    #[test]
    fn layout_wraps_wide_characters() {
        let mut e = Editor::new(None);
        e.buffer = "가나다라\nab".chars().collect();
        e.cursor = 3;
        let (rows, row, col) = e.layout(5);
        assert_eq!(rows, ["가나", "다라", "ab"]);
        assert_eq!((row, col), (1, 2));
        e.cursor = e.buffer.len();
        assert_eq!(e.layout(5).1, 2);
    }

    #[test]
    fn words_and_paths() {
        let chars: Vec<char> = "foo bar  baz".chars().collect();
        assert_eq!(word_boundary_left(&chars, 12), 9);
        assert_eq!(word_boundary_right(&chars, 3), 7);
        let dir = std::env::temp_dir().join(format!("claude-complete-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(dir.join("src")).unwrap();
        std::fs::write(dir.join("src/main.rs"), "").unwrap();
        std::fs::write(dir.join("setup.py"), "").unwrap();
        assert_eq!(complete_path(&dir, "s"), ["setup.py", "src/"]);
        assert_eq!(complete_path(&dir, "src/m"), ["src/main.rs"]);
    }
}
