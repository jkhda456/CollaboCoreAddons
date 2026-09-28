//! The terminal: raw mode, size, display width (Hangul and other wide characters take two
//! columns), colors, and one lock for everything that draws.
use std::io::{self, IsTerminal, Write};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Mutex, MutexGuard};

static COLOR: AtomicBool = AtomicBool::new(false);

/// Color themes: (name, description, accent, diff removed, diff added, code).
pub const THEMES: [(&str, &str, &str, &str, &str, &str); 5] = [
    ("dark", "Dark mode", "38;5;173", "38;5;0;48;5;224", "38;5;0;48;5;194", "38;5;110"),
    ("light", "Light mode", "38;5;166", "38;5;0;48;5;217", "38;5;0;48;5;157", "38;5;25"),
    ("dark-daltonized", "Dark mode (colorblind-friendly)", "38;5;179", "38;5;0;48;5;223", "38;5;0;48;5;153", "38;5;117"),
    ("light-daltonized", "Light mode (colorblind-friendly)", "38;5;130", "38;5;0;48;5;222", "38;5;0;48;5;117", "38;5;25"),
    ("ansi", "Terminal colors only (16-color terminals)", "33", "41", "42", "36"),
];
static THEME: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);

/// Picks a theme by name (unknown names leave the default).
pub fn set_theme(name: &str) {
    if let Some(index) = THEMES.iter().position(|(n, ..)| *n == name) {
        THEME.store(index, Ordering::Relaxed);
    }
}

pub fn theme() -> &'static str {
    THEMES[THEME.load(Ordering::Relaxed)].0
}

fn palette() -> &'static (&'static str, &'static str, &'static str, &'static str, &'static str, &'static str) {
    &THEMES[THEME.load(Ordering::Relaxed)]
}
static DRAW: Mutex<()> = Mutex::new(());

/// Decides once whether output is colored: a terminal, and NO_COLOR unset.
pub fn init_color(force_off: bool) {
    COLOR.store(!force_off && io::stdout().is_terminal() && std::env::var_os("NO_COLOR").is_none(), Ordering::Relaxed);
}

pub fn color() -> bool {
    COLOR.load(Ordering::Relaxed)
}

/// Held while writing to the terminal, so the spinner never draws in the middle of a line.
pub fn lock() -> MutexGuard<'static, ()> {
    DRAW.lock().unwrap_or_else(|poison| poison.into_inner())
}

pub fn paint(code: &str, text: &str) -> String {
    match color() {
        true => format!("\x1b[{code}m{text}\x1b[0m"),
        false => text.to_string(),
    }
}

pub fn dim(text: &str) -> String {
    paint("2", text)
}
pub fn bold(text: &str) -> String {
    paint("1", text)
}
pub fn italic(text: &str) -> String {
    paint("3", text)
}
pub fn red(text: &str) -> String {
    paint("31", text)
}
pub fn green(text: &str) -> String {
    paint("32", text)
}
pub fn yellow(text: &str) -> String {
    paint("33", text)
}
pub fn blue(text: &str) -> String {
    paint("34", text)
}
pub fn magenta(text: &str) -> String {
    paint("35", text)
}
pub fn cyan(text: &str) -> String {
    paint("36", text)
}
/// The accent: Claude's orange where 256 colors are there.
pub fn accent(text: &str) -> String {
    paint(palette().2, text)
}
pub fn diff_removed(text: &str) -> String {
    paint(palette().3, text)
}
pub fn diff_added(text: &str) -> String {
    paint(palette().4, text)
}
pub fn code(text: &str) -> String {
    paint(palette().5, text)
}

/// Columns a character takes: 0 for combining marks and controls, 2 for wide East Asian
/// characters and emoji, 1 otherwise.
pub fn char_width(c: char) -> usize {
    let code = c as u32;
    if code == 0 || code < 32 || (0x7f..0xa0).contains(&code) {
        return 0;
    }
    // Combining marks, zero-width joiners and variation selectors.
    if (0x300..=0x36f).contains(&code)
        || (0x1ab0..=0x1aff).contains(&code)
        || (0x20d0..=0x20ff).contains(&code)
        || (0xfe00..=0xfe0f).contains(&code)
        || (0xfe20..=0xfe2f).contains(&code)
        || code == 0x200b
        || code == 0x200c
        || code == 0x200d
        || (0x1160..=0x11ff).contains(&code)
    {
        return 0;
    }
    let wide = (0x1100..=0x115f).contains(&code)
        || (0x2e80..=0x303e).contains(&code)
        || (0x3041..=0x33ff).contains(&code)
        || (0x3400..=0x4dbf).contains(&code)
        || (0x4e00..=0x9fff).contains(&code)
        || (0xa000..=0xa4cf).contains(&code)
        || (0xa960..=0xa97f).contains(&code)
        || (0xac00..=0xd7a3).contains(&code)
        || (0xf900..=0xfaff).contains(&code)
        || (0xfe30..=0xfe4f).contains(&code)
        || (0xff00..=0xff60).contains(&code)
        || (0xffe0..=0xffe6).contains(&code)
        || (0x1f300..=0x1f64f).contains(&code)
        || (0x1f900..=0x1f9ff).contains(&code)
        || (0x20000..=0x3fffd).contains(&code);
    if wide {
        2
    } else {
        1
    }
}

/// Display width of text, ANSI escapes not counted.
pub fn width(text: &str) -> usize {
    let mut total = 0;
    let mut chars = text.chars().peekable();
    while let Some(c) = chars.next() {
        if c == '\x1b' {
            // CSI: ESC [ … final byte in @..~
            if chars.peek() == Some(&'[') {
                chars.next();
                for c in chars.by_ref() {
                    if ('@'..='~').contains(&c) {
                        break;
                    }
                }
            }
            continue;
        }
        total += char_width(c);
    }
    total
}

/// Text cut to at most `columns` display columns, with … when cut.
pub fn truncate(text: &str, columns: usize) -> String {
    if width(text) <= columns {
        return text.to_string();
    }
    let mut out = String::new();
    let mut used = 0;
    for c in text.chars() {
        let w = char_width(c);
        if used + w + 1 > columns {
            break;
        }
        out.push(c);
        used += w;
    }
    out.push('…');
    out
}

/// The terminal's size (columns, rows); 80x24 when it cannot be asked.
pub fn size() -> (usize, usize) {
    // SAFETY: TIOCGWINSZ fills the zeroed winsize.
    unsafe {
        let mut size: libc::winsize = std::mem::zeroed();
        for fd in [1, 0, 2] {
            if libc::ioctl(fd, libc::TIOCGWINSZ, &mut size) == 0 && size.ws_col > 0 {
                return (size.ws_col as usize, size.ws_row.max(1) as usize);
            }
        }
    }
    (80, 24)
}

pub fn columns() -> usize {
    size().0.max(20)
}

/// stdin's terminal settings, restored when dropped. `raw` for the line editor (no echo, no
/// line buffering, no signals: Ctrl-C arrives as a byte); `cbreak` while a turn runs (no echo,
/// no line buffering, but Ctrl-C still raises SIGINT).
pub struct Mode {
    saved: Option<libc::termios>,
}

impl Mode {
    fn set(change: impl FnOnce(&mut libc::termios)) -> Mode {
        // SAFETY: termios calls on fd 0 with a struct tcgetattr fills.
        unsafe {
            let mut termios: libc::termios = std::mem::zeroed();
            if libc::isatty(0) == 0 || libc::tcgetattr(0, &mut termios) != 0 {
                return Mode { saved: None };
            }
            let saved = termios;
            change(&mut termios);
            libc::tcsetattr(0, libc::TCSADRAIN, &termios);
            Mode { saved: Some(saved) }
        }
    }

    pub fn raw() -> Mode {
        Mode::set(|t| {
            t.c_iflag &= !(libc::IXON | libc::ICRNL | libc::INLCR | libc::IGNCR | libc::ISTRIP);
            t.c_lflag &= !(libc::ICANON | libc::ECHO | libc::ISIG | libc::IEXTEN);
            t.c_cc[libc::VMIN] = 1;
            t.c_cc[libc::VTIME] = 0;
        })
    }

    pub fn cbreak() -> Mode {
        Mode::set(|t| {
            t.c_iflag &= !(libc::IXON | libc::ICRNL);
            t.c_lflag &= !(libc::ICANON | libc::ECHO);
            t.c_lflag |= libc::ISIG;
            t.c_cc[libc::VMIN] = 1;
            t.c_cc[libc::VTIME] = 0;
        })
    }
}

impl Drop for Mode {
    fn drop(&mut self) {
        if let Some(saved) = self.saved {
            // SAFETY: the settings tcgetattr gave back.
            unsafe { libc::tcsetattr(0, libc::TCSADRAIN, &saved) };
        }
    }
}

pub fn stdin_is_tty() -> bool {
    // SAFETY: isatty on fd 0.
    unsafe { libc::isatty(0) == 1 }
}

/// Waits up to `millis` for input on stdin.
pub fn stdin_ready(millis: i32) -> bool {
    let mut poll = libc::pollfd { fd: 0, events: libc::POLLIN, revents: 0 };
    // SAFETY: one pollfd.
    unsafe { libc::poll(&mut poll, 1, millis) > 0 && poll.revents & libc::POLLIN != 0 }
}

/// One read(2) from stdin: what is there, at most `buffer.len()` bytes. None at EOF or error
/// (EINTR included, so an interrupt reaches the caller).
pub fn read_stdin(buffer: &mut [u8]) -> Option<usize> {
    // SAFETY: reads into the buffer, within its length.
    let read = unsafe { libc::read(0, buffer.as_mut_ptr() as *mut libc::c_void, buffer.len()) };
    (read > 0).then_some(read as usize)
}

static AT_LINE_START: AtomicBool = AtomicBool::new(true);

/// Whether the cursor is at the start of a line (the spinner draws only there).
pub fn at_line_start() -> bool {
    AT_LINE_START.load(Ordering::Relaxed)
}

/// Writes to the terminal; the spinner's line is cleared first, and comes back after.
pub fn out(text: &str) {
    if text.is_empty() {
        return;
    }
    let _guard = lock();
    crate::ui::spinner::erase_locked();
    let mut stdout = io::stdout().lock();
    let _ = stdout.write_all(text.as_bytes());
    let _ = stdout.flush();
    AT_LINE_START.store(text.ends_with('\n'), Ordering::Relaxed);
}

/// Raw bytes (cursor movement), under the lock, without touching the spinner.
pub fn raw(text: &str) {
    let mut stdout = io::stdout().lock();
    let _ = stdout.write_all(text.as_bytes());
    let _ = stdout.flush();
}

pub fn set_line_start(value: bool) {
    AT_LINE_START.store(value, Ordering::Relaxed);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn widths() {
        assert_eq!(width("abc"), 3);
        assert_eq!(width("한글"), 4);
        assert_eq!(width("\x1b[1mhi\x1b[0m"), 2);
        assert_eq!(width("e\u{301}"), 1);
        assert_eq!(truncate("안녕하세요", 7), "안녕하…");
    }

}
