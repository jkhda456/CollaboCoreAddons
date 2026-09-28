//! Drawing the model's words and the tools' changes: Markdown as it streams in (a line at a time),
//! and edits as numbered diffs.
use crate::term;

/// Renders Markdown line by line as text arrives.
#[derive(Default)]
pub struct Markdown {
    pending: String,
    in_code: bool,
    /// The pending text continues a line already partly drawn (a long paragraph, wrapped as
    /// it streams): no block syntax at its start, and this indent.
    continuing: Option<usize>,
    /// Wrap width; 0 means the terminal's.
    pub width: usize,
}

/// Where a long line can be cut: the last space within `limit` columns, outside `code` and
/// with ** balanced before it.
fn wrap_point(text: &str, limit: usize) -> Option<usize> {
    let mut used = 0;
    let mut best = None;
    let mut ticks = 0;
    for (index, c) in text.char_indices() {
        used += term::char_width(c);
        if used > limit {
            break;
        }
        if c == '`' {
            ticks += 1;
        }
        if c == ' ' && ticks % 2 == 0 && text[..index].matches("**").count() % 2 == 0 {
            best = Some(index);
        }
    }
    best
}

impl Markdown {
    /// Takes streamed text; returns what can be drawn now (whole lines).
    pub fn push(&mut self, text: &str) -> String {
        self.pending.push_str(text);
        let mut out = String::new();
        while let Some(end) = self.pending.find('\n') {
            let line: String = self.pending.drain(..=end).collect();
            if let Some(rendered) = self.piece(line.trim_end_matches(['\n', '\r'])) {
                out.push_str(&rendered);
                out.push('\n');
            }
            self.continuing = None;
        }
        // A paragraph longer than the screen: draw what fills a line now.
        let width = if self.width > 0 { self.width } else { term::columns().saturating_sub(4).max(20) };
        while !self.in_code {
            let indent = self.continuing.unwrap_or(0);
            let limit = width.saturating_sub(indent);
            if term::width(&self.pending) <= limit {
                break;
            }
            let Some(cut) = wrap_point(&self.pending, limit) else { break };
            let head: String = self.pending.drain(..=cut).collect();
            let block_indent = match self.continuing {
                Some(indent) => indent,
                None => {
                    let trimmed = head.trim_start();
                    let lead = head.len() - trimmed.len();
                    lead + if ["- ", "* ", "+ ", "> "].iter().any(|b| trimmed.starts_with(b)) { 2 } else { 0 }
                }
            };
            if let Some(rendered) = self.piece(head.trim_end()) {
                out.push_str(&rendered);
                out.push('\n');
            }
            self.continuing = Some(block_indent);
        }
        out
    }

    fn piece(&mut self, text: &str) -> Option<String> {
        match self.continuing {
            Some(indent) => Some(format!("{}{}", " ".repeat(indent), inline(text.trim_start()))),
            None => self.line(text),
        }
    }

    /// The rest, at the end of the answer.
    pub fn finish(&mut self) -> String {
        let rest = std::mem::take(&mut self.pending);
        let out = match rest.is_empty() {
            true => String::new(),
            false => self.piece(&rest).map(|line| line + "\n").unwrap_or_default(),
        };
        self.in_code = false;
        self.continuing = None;
        out
    }

    fn line(&mut self, line: &str) -> Option<String> {
        let trimmed = line.trim_start();
        if trimmed.starts_with("```") || trimmed.starts_with("~~~") {
            self.in_code = !self.in_code;
            return None;
        }
        if self.in_code {
            return Some(format!("  {}", term::code(line)));
        }
        let indent = &line[..line.len() - trimmed.len()];
        if let Some(rest) = trimmed.strip_prefix("#") {
            let level = 1 + rest.chars().take_while(|c| *c == '#').count();
            let title = rest.trim_start_matches('#').trim();
            return Some(match level {
                1 => term::paint("1;4", &inline(title)),
                2 => term::bold(&inline(title)),
                _ => term::paint("1;3", &inline(title)),
            });
        }
        if trimmed.chars().all(|c| c == '-' || c == '*' || c == '_') && trimmed.len() >= 3 {
            return Some(term::dim(&"─".repeat(term::columns().min(80))));
        }
        if let Some(quote) = trimmed.strip_prefix('>') {
            return Some(format!("{indent}{} {}", term::dim("▌"), term::italic(&inline(quote.trim_start()))));
        }
        for bullet in ["- ", "* ", "+ "] {
            if let Some(item) = trimmed.strip_prefix(bullet) {
                let mark = if indent.len() >= 2 { "◦" } else { "•" };
                let item = match item.strip_prefix("[ ] ").map(|rest| ("☐", rest)).or_else(|| item.strip_prefix("[x] ").map(|rest| ("☒", rest))) {
                    Some((check, rest)) => format!("{check} {}", inline(rest)),
                    None => inline(item),
                };
                return Some(format!("{indent}{mark} {item}"));
            }
        }
        Some(format!("{indent}{}", inline(trimmed)))
    }
}

/// **bold**, *italic*, `code`, [text](url).
pub fn inline(text: &str) -> String {
    if !term::color() {
        return text.to_string();
    }
    let chars: Vec<char> = text.chars().collect();
    let mut out = String::new();
    let mut i = 0;
    let find = |from: usize, pattern: &[char]| -> Option<usize> {
        (from..chars.len().saturating_sub(pattern.len() - 1)).find(|&j| chars[j..j + pattern.len()] == *pattern)
    };
    while i < chars.len() {
        let c = chars[i];
        if c == '`' {
            if let Some(end) = find(i + 1, &['`']) {
                let code: String = chars[i + 1..end].iter().collect();
                out.push_str(&term::code(&code));
                i = end + 1;
                continue;
            }
        }
        if c == '*' && chars.get(i + 1) == Some(&'*') {
            if let Some(end) = find(i + 2, &['*', '*']) {
                let inner: String = chars[i + 2..end].iter().collect();
                out.push_str(&term::bold(&inline(&inner)));
                i = end + 2;
                continue;
            }
        }
        if (c == '*' || c == '_') && chars.get(i + 1).is_some_and(|next| !next.is_whitespace()) && (i == 0 || !chars[i - 1].is_alphanumeric()) {
            if let Some(end) = find(i + 1, &[c]) {
                if end > i + 1 && !chars[end - 1].is_whitespace() && chars.get(end + 1).is_none_or(|next| !next.is_alphanumeric()) {
                    let inner: String = chars[i + 1..end].iter().collect();
                    out.push_str(&term::italic(&inner));
                    i = end + 1;
                    continue;
                }
            }
        }
        if c == '[' {
            if let Some(close) = find(i + 1, &[']']) {
                if chars.get(close + 1) == Some(&'(') {
                    if let Some(end) = find(close + 2, &[')']) {
                        let label: String = chars[i + 1..close].iter().collect();
                        let url: String = chars[close + 2..end].iter().collect();
                        out.push_str(&term::paint("4", &label));
                        if label != url {
                            out.push_str(&term::dim(&format!(" ({url})")));
                        }
                        i = end + 1;
                        continue;
                    }
                }
            }
        }
        out.push(c);
        i += 1;
    }
    out
}

#[derive(Debug, PartialEq, Clone, Copy)]
pub enum Change {
    Same,
    Removed,
    Added,
}

/// Line diff by longest common subsequence (both sides cut to a size that stays quick).
pub fn diff_lines<'a>(old: &'a str, new: &'a str) -> Vec<(Change, &'a str)> {
    let a: Vec<&str> = old.lines().collect();
    let b: Vec<&str> = new.lines().collect();
    // Common head and tail first: an edit usually touches a small middle.
    let head = a.iter().zip(&b).take_while(|(x, y)| x == y).count();
    let tail = a[head..].iter().rev().zip(b[head..].iter().rev()).take_while(|(x, y)| x == y).count();
    let (ma, mb) = (&a[head..a.len() - tail], &b[head..b.len() - tail]);
    let mut out: Vec<(Change, &str)> = a[..head].iter().map(|line| (Change::Same, *line)).collect();
    if ma.len() * mb.len() > 4_000_000 {
        out.extend(ma.iter().map(|line| (Change::Removed, *line)));
        out.extend(mb.iter().map(|line| (Change::Added, *line)));
    } else {
        let (n, m) = (ma.len(), mb.len());
        let mut table = vec![0u32; (n + 1) * (m + 1)];
        for i in (0..n).rev() {
            for j in (0..m).rev() {
                table[i * (m + 1) + j] = if ma[i] == mb[j] {
                    table[(i + 1) * (m + 1) + j + 1] + 1
                } else {
                    table[(i + 1) * (m + 1) + j].max(table[i * (m + 1) + j + 1])
                };
            }
        }
        let (mut i, mut j) = (0, 0);
        while i < n || j < m {
            if i < n && j < m && ma[i] == mb[j] {
                out.push((Change::Same, ma[i]));
                i += 1;
                j += 1;
            } else if i < n && (j == m || table[(i + 1) * (m + 1) + j] >= table[i * (m + 1) + j + 1]) {
                out.push((Change::Removed, ma[i]));
                i += 1;
            } else {
                out.push((Change::Added, mb[j]));
                j += 1;
            }
        }
    }
    out.extend(a[a.len() - tail..].iter().map(|line| (Change::Same, *line)));
    out
}

/// "Updated path with 2 additions and 1 removal" and the changed lines with three lines of
/// context, numbered; at most `max_lines` lines.
pub fn diff(before: &str, after: &str, max_lines: usize) -> (String, Vec<String>) {
    let lines = diff_lines(before, after);
    let added = lines.iter().filter(|(change, _)| *change == Change::Added).count();
    let removed = lines.iter().filter(|(change, _)| *change == Change::Removed).count();
    let plural = |n: usize, word: &str| format!("{n} {word}{}", if n == 1 { "" } else { "s" });
    let summary = match (added, removed) {
        (0, 0) => "no changes".to_string(),
        (a, 0) => plural(a, "addition"),
        (0, r) => plural(r, "removal"),
        (a, r) => format!("{} and {}", plural(a, "addition"), plural(r, "removal")),
    };
    // Which lines to show: changes and their neighbours.
    let near: Vec<bool> = (0..lines.len())
        .map(|i| lines[i.saturating_sub(3)..(i + 4).min(lines.len())].iter().any(|(change, _)| *change != Change::Same))
        .collect();
    let (mut old_no, mut new_no) = (0usize, 0usize);
    let mut out = Vec::new();
    let mut skipped = false;
    let columns = term::columns().saturating_sub(14).max(20);
    for (i, (change, text)) in lines.iter().enumerate() {
        match change {
            Change::Same => {
                old_no += 1;
                new_no += 1;
            }
            Change::Removed => old_no += 1,
            Change::Added => new_no += 1,
        }
        if !near[i] {
            skipped = true;
            continue;
        }
        if skipped && !out.is_empty() {
            out.push(term::dim("   ..."));
        }
        skipped = false;
        let shown = term::truncate(&text.replace('\t', "    "), columns);
        out.push(match change {
            Change::Same => format!("{:>5}  {}", new_no, term::dim(&shown)),
            Change::Removed => term::diff_removed(&format!("{:>5} -{shown}", old_no)),
            Change::Added => term::diff_added(&format!("{:>5} +{shown}", new_no)),
        });
        if out.len() >= max_lines {
            out.push(term::dim("   … (more changes)"));
            break;
        }
    }
    (summary, out)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn markdown_lines() {
        let mut md = Markdown::default();
        assert_eq!(md.push("# Title\n- one\n  - two\npart"), "Title\n• one\n  ◦ two\n");
        assert_eq!(md.push("ial\n```rust\nfn x() {}\n```\n"), "partial\n  fn x() {}\n");
        assert_eq!(md.push("> quote\n---\ntail"), "▌ quote\n".to_string() + &"─".repeat(term::columns().min(80)) + "\n");
        assert_eq!(md.finish(), "tail\n");
    }

    #[test]
    fn long_lines_stream_wrapped() {
        let mut md = Markdown { width: 20, ..Default::default() };
        assert_eq!(md.push("- one two three four five six"), "• one two three\n");
        assert_eq!(md.push(" seven\nnext"), "  four five six seven\n");
        assert_eq!(md.finish(), "next\n");
        let mut md = Markdown { width: 12, ..Default::default() };
        // Not inside the code span: the cut comes before it, and after it only when it fits.
        assert_eq!(md.push("say `a b c d e f` ok"), "say\n");
        assert_eq!(md.finish(), "`a b c d e f` ok\n");
    }

    #[test]
    fn diffs() {
        let before = "a\nb\nc\nd\ne\nf\ng\nh\ni\nj\n";
        let after = "a\nb\nc\nd\nE\nf\ng\nh\ni\nj\nk\n";
        let lines = diff_lines(before, after);
        assert_eq!(lines.iter().filter(|(c, _)| *c == Change::Removed).map(|(_, l)| *l).collect::<Vec<_>>(), ["e"]);
        assert_eq!(lines.iter().filter(|(c, _)| *c == Change::Added).map(|(_, l)| *l).collect::<Vec<_>>(), ["E", "k"]);
        let (summary, shown) = diff(before, after, 50);
        assert_eq!(summary, "2 additions and 1 removal");
        assert!(shown.iter().any(|line| line.contains("5 -e")));
        assert!(shown.iter().any(|line| line.contains("11 +k")));
        assert!(!shown.iter().any(|line| line.ends_with(" a")));
    }
}
