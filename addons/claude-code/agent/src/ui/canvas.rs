//! Lines kept on screen under the cursor and redrawn in place: the spinner's panel, the input
//! box, dialogs. A new frame is written over the old one, only the lines that changed, and is
//! never cleared first: the terminal shows what comes as it comes (Windows Terminal at once), and
//! the guest's terminal hands one write on a line at a time, so a cleared frame would be seen gone.
//! Each frame goes as one synchronized update (DEC mode 2026) with the cursor hidden while it moves.

pub struct Canvas {
    /// The lines on screen now, top to bottom (none when it is not drawn).
    pub shown: Vec<String>,
    /// The terminal's width when they were drawn.
    columns: usize,
    /// Where the cursor was left: rows from the top of the drawing, and the column.
    cursor_row: usize,
    cursor_col: usize,
}

impl Canvas {
    pub const fn new() -> Canvas {
        Canvas { shown: Vec::new(), columns: 0, cursor_row: 0, cursor_col: 0 }
    }

    pub fn is_drawn(&self) -> bool {
        !self.shown.is_empty()
    }

    /// Back to the start of the drawing's first line.
    pub fn to_top(&self) -> String {
        match self.cursor_row {
            0 => "\r".to_string(),
            up => format!("\r\x1b[{up}A"),
        }
    }

    /// What clears the drawing from the screen (nothing when it is not there).
    pub fn erase(&mut self) -> String {
        let text = match self.is_drawn() {
            true => self.to_top() + "\x1b[J",
            false => String::new(),
        };
        self.forget();
        text
    }

    /// Something else wrote over the drawing (or cleared the screen): the next frame starts afresh
    /// where the cursor is.
    pub fn forget(&mut self) {
        self.shown.clear();
        self.cursor_row = 0;
        self.cursor_col = 0;
    }

    /// The next frame, and the cursor's place in it (row, column): afresh the first time or when
    /// the width changed (what is there may have wrapped), else only the lines that changed
    /// (`every`: all of them). Empty when nothing changed.
    pub fn draw(&mut self, lines: Vec<String>, cursor: (usize, usize), columns: usize, every: bool) -> String {
        match self.is_drawn() && self.columns == columns {
            true => self.update(lines, cursor, columns, every),
            false => {
                let mut text = self.erase();
                text.push_str(&self.fresh(lines, cursor, columns));
                text
            }
        }
    }

    /// The frame drawn from the start of the cursor's row down, over whatever is there; below its
    /// last line (which never fills its row) everything is cleared.
    pub fn fresh(&mut self, lines: Vec<String>, cursor: (usize, usize), columns: usize) -> String {
        let mut out = String::new();
        for (index, line) in lines.iter().enumerate() {
            if index > 0 {
                out.push_str("\r\n");
            }
            put_line(&mut out, line, columns);
        }
        out.push_str("\x1b[J");
        place(&mut out, lines.len().saturating_sub(1), cursor);
        self.shown = lines;
        self.columns = columns;
        (self.cursor_row, self.cursor_col) = cursor;
        out
    }

    /// The frame brought from what is on screen to `lines`: only the lines that changed (`every`:
    /// all of them) are written, over the old ones; nothing is cleared first. Empty when nothing
    /// changed.
    pub fn update(&mut self, lines: Vec<String>, cursor: (usize, usize), columns: usize, every: bool) -> String {
        if !every && lines == self.shown && cursor == (self.cursor_row, self.cursor_col) {
            return String::new();
        }
        let mut out = String::new();
        // Rows of the drawing on screen (moved over with the cursor), and the row the cursor is on.
        let mut rows = self.shown.len();
        let mut at = self.cursor_row;
        for (index, line) in lines.iter().enumerate() {
            if !every && self.shown.get(index) == Some(line) {
                continue;
            }
            go_to(&mut out, &mut at, index, rows);
            put_line(&mut out, line, columns);
            rows = rows.max(index + 1);
        }
        // Shorter: what is left of the old drawing goes.
        if self.shown.len() > lines.len() {
            go_to(&mut out, &mut at, lines.len(), rows);
            out.push_str("\r\x1b[J");
        }
        place(&mut out, at, cursor);
        self.shown = lines;
        self.columns = columns;
        (self.cursor_row, self.cursor_col) = cursor;
        out
    }
}

/// One update of the screen: shown whole by a terminal that knows synchronized output (DEC mode
/// 2026; one that does not ignores it), with the cursor hidden while it moves.
pub fn synchronized(text: &str) -> String {
    format!("\x1b[?2026h\x1b[?25l{text}\x1b[?25h\x1b[?2026l")
}

/// A line written over whatever the row held: the rest of the row cleared, unless the line fills
/// it (a terminal waiting to wrap would clear the last column).
fn put_line(out: &mut String, line: &str, columns: usize) {
    out.push('\r');
    out.push_str(line);
    if crate::term::width(line) < columns {
        out.push_str("\x1b[K");
    }
}

/// Moves the cursor from row `*at` to `row` of the drawing: down over the `rows` it has on screen,
/// with new lines past them (which scroll the screen at its bottom).
fn go_to(out: &mut String, at: &mut usize, row: usize, rows: usize) {
    if row < *at {
        out.push_str(&format!("\x1b[{}A", *at - row));
    } else if row > *at {
        let over = row.min(rows.saturating_sub(1)).saturating_sub(*at);
        if over > 0 {
            out.push_str(&format!("\x1b[{over}B"));
        }
        for _ in *at + over..row {
            out.push_str("\r\n");
        }
    }
    *at = row;
}

/// From row `at` of the drawing to the cursor's place in it.
fn place(out: &mut String, at: usize, (row, col): (usize, usize)) {
    if at > row {
        out.push_str(&format!("\x1b[{}A", at - row));
    } else if row > at {
        out.push_str(&format!("\x1b[{}B", row - at));
    }
    out.push('\r');
    if col > 0 {
        out.push_str(&format!("\x1b[{col}C"));
    }
}

#[cfg(test)]
pub mod tests {
    use super::*;

    /// Just enough of a VT terminal for these frames: printing with autowrap, CR, LF (scrolling at
    /// the bottom), and CSI A B C K J.
    pub struct Screen {
        rows: Vec<Vec<char>>,
        pub row: usize,
        pub col: usize,
        width: usize,
    }

    impl Screen {
        pub fn new(width: usize, height: usize) -> Screen {
            Screen { rows: vec![vec![' '; width]; height], row: 0, col: 0, width }
        }

        pub fn feed(&mut self, text: &str) {
            let mut chars = text.chars().peekable();
            while let Some(c) = chars.next() {
                match c {
                    '\r' => self.col = 0,
                    '\n' => self.down(),
                    '\x1b' => {
                        chars.next(); // [
                        let mut params = String::new();
                        let mut last = ' ';
                        for c in chars.by_ref() {
                            if ('@'..='~').contains(&c) {
                                last = c;
                                break;
                            }
                            params.push(c);
                        }
                        if params.starts_with('?') {
                            continue;
                        }
                        let n = params.parse::<usize>().unwrap_or(1).max(1);
                        match last {
                            'A' => self.row = self.row.saturating_sub(n),
                            'B' => self.row = (self.row + n).min(self.rows.len() - 1),
                            'C' => self.col = (self.col + n).min(self.width - 1),
                            'K' => (self.col.min(self.width - 1)..self.width).for_each(|c| self.rows[self.row][c] = ' '),
                            'J' => {
                                (self.col.min(self.width - 1)..self.width).for_each(|c| self.rows[self.row][c] = ' ');
                                for row in self.row + 1..self.rows.len() {
                                    self.rows[row] = vec![' '; self.width];
                                }
                            }
                            _ => {}
                        }
                    }
                    c => {
                        if self.col >= self.width {
                            self.col = 0;
                            self.down();
                        }
                        self.rows[self.row][self.col] = c;
                        self.col += 1;
                    }
                }
            }
        }

        fn down(&mut self) {
            if self.row + 1 == self.rows.len() {
                self.rows.remove(0);
                self.rows.push(vec![' '; self.width]);
            } else {
                self.row += 1;
            }
        }

        pub fn text(&self) -> Vec<String> {
            let mut rows: Vec<String> = self.rows.iter().map(|r| r.iter().collect::<String>().trim_end().to_string()).collect();
            while rows.last().is_some_and(|r| r.is_empty()) {
                rows.pop();
            }
            rows
        }
    }

    fn lines(text: &[&str]) -> Vec<String> {
        text.iter().map(|s| s.to_string()).collect()
    }

    #[test]
    fn frames_drawn_in_place() {
        let width = 30;
        let mut screen = Screen::new(width, 9);
        screen.feed("above\r\n");
        let mut canvas = Canvas::new();
        let a = lines(&["✻ Doing… (1s)", "", "╭──╮", "│ > hi │", "╰──╯", "footer"]);
        screen.feed(&canvas.draw(a.clone(), (3, 6), width, false));
        let mut want = vec!["above".to_string()];
        want.extend(a.iter().cloned());
        assert_eq!(screen.text(), want.iter().map(|l| l.trim_end().to_string()).collect::<Vec<_>>());
        assert_eq!((screen.row, screen.col), (4, 6));

        // A tick: only the first line changes, and nothing else is written.
        let b = lines(&["✶ Doing… (2s)", "", "╭──╮", "│ > hi │", "╰──╯", "footer"]);
        let tick = canvas.draw(b.clone(), (3, 6), width, false);
        assert!(!tick.contains("footer") && !tick.contains('\n') && !tick.contains("\x1b[J"), "{tick:?}");
        screen.feed(&tick);
        assert_eq!(screen.text()[1..], b.iter().map(|l| l.trim_end().to_string()).collect::<Vec<_>>()[..]);
        assert_eq!((screen.row, screen.col), (4, 6));

        // Nothing changed: nothing written.
        assert_eq!(canvas.draw(b.clone(), (3, 6), width, false), "");

        // Longer, at the bottom of the screen: it scrolls ("above" goes), the rows stay in step.
        let c = lines(&["✶ Doing… (3s)", "  ⎿  note", "     ☐ one", "     ☐ two", "", "╭──╮", "│ > hi │", "╰──╯", "footer"]);
        screen.feed(&canvas.draw(c.clone(), (6, 6), width, false));
        assert_eq!(screen.text(), c.iter().map(|l| l.trim_end().to_string()).collect::<Vec<_>>());
        assert_eq!((screen.row, screen.col), (6, 6));

        // Shorter: what is left of the old drawing goes.
        let d = lines(&["✳ Doing… (4s)", "footer"]);
        screen.feed(&canvas.draw(d.clone(), (1, 6), width, false));
        let shown = screen.text();
        assert_eq!(shown[shown.len() - 2..], ["✳ Doing… (4s)", "footer"]);

        // Every line again (`every`) leaves the same screen.
        let before = screen.text();
        screen.feed(&canvas.draw(d.clone(), (1, 6), width, true));
        assert_eq!(screen.text(), before);

        // Erased: gone, the cursor where the drawing began.
        screen.feed(&canvas.erase());
        assert!(screen.text().is_empty() && (screen.row, screen.col) == (0, 0), "{:?}", screen.text());
        assert!(!canvas.is_drawn() && canvas.erase().is_empty());
    }

    #[test]
    fn full_width_lines_keep_their_last_column() {
        let width = 10;
        let mut screen = Screen::new(width, 4);
        let mut canvas = Canvas::new();
        let box_top = format!("╭{}╮", "─".repeat(width - 2));
        screen.feed(&canvas.draw(vec![box_top.clone(), "x".into()], (1, 1), width, false));
        assert_eq!(screen.text(), [box_top.clone(), "x".into()]);
        screen.feed(&canvas.draw(vec![box_top.clone(), "y".into()], (1, 1), width, true));
        assert_eq!(screen.text(), [box_top, "y".into()]);
    }

    #[test]
    fn a_new_width_draws_afresh() {
        let mut canvas = Canvas::new();
        canvas.draw(lines(&["a", "b"]), (1, 0), 20, false);
        let text = canvas.draw(lines(&["a", "b"]), (1, 0), 30, false);
        assert!(text.starts_with("\r\x1b[1A\x1b[J") && text.contains('a') && text.contains('b'), "{text:?}");
    }
}
