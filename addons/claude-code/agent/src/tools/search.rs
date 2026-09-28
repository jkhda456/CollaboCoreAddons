//! Glob and Grep.
use std::fs;
use std::path::{Path, PathBuf};
use std::time::SystemTime;

use serde_json::Value;

use super::{fail, ok, shorten, Context, Outcome, MAX_OUTPUT};

const SKIPPED_DIRS: [&str; 2] = [".git", "node_modules"];

fn modified(path: &Path) -> SystemTime {
    fs::metadata(path).and_then(|meta| meta.modified()).unwrap_or(SystemTime::UNIX_EPOCH)
}

fn skipped(path: &Path) -> bool {
    path.components().any(|part| part.as_os_str().to_str().is_some_and(|name| SKIPPED_DIRS.contains(&name)))
}

/// {a,b} alternatives, which the glob crate does not know: one pattern per combination.
fn expand_braces(pattern: &str) -> Vec<String> {
    let Some(open) = pattern.find('{') else { return vec![pattern.to_string()] };
    let Some(close) = pattern[open..].find('}').map(|i| open + i) else { return vec![pattern.to_string()] };
    let (head, body, tail) = (&pattern[..open], &pattern[open + 1..close], &pattern[close + 1..]);
    body.split(',').flat_map(|choice| expand_braces(&format!("{head}{choice}{tail}"))).collect()
}

pub fn glob_tool(input: &Value, context: &mut Context) -> Outcome {
    let Some(pattern) = input.get("pattern").and_then(Value::as_str) else { return fail("InputValidationError: pattern is required") };
    let base = context.resolve(input.get("path").and_then(Value::as_str).unwrap_or("."));
    let mut found: Vec<(SystemTime, PathBuf)> = Vec::new();
    for pattern in expand_braces(pattern) {
        let full = match Path::new(&pattern).is_absolute() {
            true => pattern.clone(),
            false => format!("{}/{pattern}", base.display().to_string().trim_end_matches('/')),
        };
        let paths = match glob::glob(&full) {
            Ok(paths) => paths,
            Err(error) => return fail(format!("bad pattern: {error}")),
        };
        for path in paths.flatten() {
            if path.is_file() && !skipped(&path) && !found.iter().any(|(_, seen)| *seen == path) {
                found.push((modified(&path), path));
            }
        }
    }
    if found.is_empty() {
        return ok("No files found");
    }
    found.sort_by(|a, b| b.0.cmp(&a.0));
    let total = found.len();
    let mut out: Vec<String> = found.into_iter().take(100).map(|(_, path)| path.display().to_string()).collect();
    if total > 100 {
        out.push("(Results are truncated. Consider using a more specific path or pattern.)".into());
    }
    ok(out.join("\n"))
}

fn walk(path: &Path, files: &mut Vec<PathBuf>, limit: usize) {
    if files.len() >= limit {
        return;
    }
    let Ok(meta) = fs::symlink_metadata(path) else { return };
    if meta.is_file() {
        files.push(path.to_path_buf());
        return;
    }
    if !meta.is_dir() {
        return;
    }
    let Ok(entries) = fs::read_dir(path) else { return };
    let mut entries: Vec<PathBuf> = entries.flatten().map(|entry| entry.path()).collect();
    entries.sort();
    for entry in entries {
        if entry.file_name().and_then(|name| name.to_str()).is_some_and(|name| SKIPPED_DIRS.contains(&name)) {
            continue;
        }
        walk(&entry, files, limit);
    }
}

/// ripgrep's names for common file types.
fn type_extensions(kind: &str) -> Option<&'static [&'static str]> {
    Some(match kind {
        "js" => &["js", "jsx", "mjs", "cjs"],
        "ts" => &["ts", "tsx", "mts", "cts"],
        "py" => &["py", "pyi"],
        "rust" | "rs" => &["rs"],
        "go" => &["go"],
        "java" => &["java"],
        "kotlin" => &["kt", "kts"],
        "c" => &["c", "h"],
        "cpp" => &["cpp", "cc", "cxx", "hpp", "hh", "hxx", "h"],
        "cs" => &["cs"],
        "rb" | "ruby" => &["rb"],
        "php" => &["php"],
        "swift" => &["swift"],
        "sh" => &["sh", "bash"],
        "md" | "markdown" => &["md", "markdown"],
        "json" => &["json"],
        "yaml" => &["yaml", "yml"],
        "toml" => &["toml"],
        "html" => &["html", "htm"],
        "css" => &["css", "scss", "less"],
        "sql" => &["sql"],
        "dart" => &["dart"],
        _ => return None,
    })
}

pub fn grep(input: &Value, context: &mut Context) -> Outcome {
    let Some(pattern) = input.get("pattern").and_then(Value::as_str) else { return fail("InputValidationError: pattern is required") };
    let multiline = input.get("multiline").and_then(Value::as_bool).unwrap_or(false);
    let regex = match regex::RegexBuilder::new(pattern)
        .case_insensitive(input.get("-i").and_then(Value::as_bool).unwrap_or(false))
        .multi_line(true)
        .dot_matches_new_line(multiline)
        .build()
    {
        Ok(regex) => regex,
        Err(error) => return fail(format!("bad regular expression: {error}")),
    };
    let base = context.resolve(input.get("path").and_then(Value::as_str).unwrap_or("."));
    if !base.exists() {
        return fail(format!("Path does not exist: {}", base.display()));
    }
    let filters: Vec<glob::Pattern> = input
        .get("glob")
        .and_then(Value::as_str)
        .map(|glob| expand_braces(glob).iter().filter_map(|p| glob::Pattern::new(p).ok()).collect())
        .unwrap_or_default();
    let kinds = match input.get("type").and_then(Value::as_str) {
        Some(kind) => match type_extensions(kind) {
            Some(kinds) => Some(kinds),
            None => return fail(format!("unknown file type \"{kind}\"")),
        },
        None => None,
    };
    let mode = input.get("output_mode").and_then(Value::as_str).unwrap_or("files_with_matches");
    let limit = input.get("head_limit").and_then(Value::as_u64).map_or(usize::MAX, |n| n.max(1) as usize);
    let number = input.get("-n").and_then(Value::as_bool).unwrap_or(true);
    let context_lines = input.get("-C").and_then(Value::as_u64).unwrap_or(0) as usize;
    let before = input.get("-B").and_then(Value::as_u64).map_or(context_lines, |n| n as usize);
    let after = input.get("-A").and_then(Value::as_u64).map_or(context_lines, |n| n as usize);

    let mut files = Vec::new();
    walk(&base, &mut files, 200_000);
    // Most recently changed first for the file list, as Claude Code does.
    if mode == "files_with_matches" {
        files.sort_by_key(|path| std::cmp::Reverse(modified(path)));
    }
    let mut results: Vec<String> = Vec::new();
    let mut truncated = false;
    'files: for file in files {
        let relative = file.strip_prefix(&base).unwrap_or(&file);
        let name = file.file_name().map(|name| name.to_string_lossy().into_owned()).unwrap_or_default();
        if !filters.is_empty() && !filters.iter().any(|f| f.matches(&name) || f.matches_path(relative)) {
            continue;
        }
        if let Some(kinds) = kinds {
            let extension = file.extension().and_then(|e| e.to_str()).unwrap_or("");
            if !kinds.contains(&extension) {
                continue;
            }
        }
        let Ok(meta) = fs::metadata(&file) else { continue };
        if meta.len() > 20 * 1024 * 1024 {
            continue;
        }
        let Ok(bytes) = fs::read(&file) else { continue };
        if bytes[..bytes.len().min(8192)].contains(&0) {
            continue;
        }
        let text = String::from_utf8_lossy(&bytes);
        let shown = file.display();
        match mode {
            "content" => {
                let lines: Vec<&str> = text.lines().collect();
                // Line index of each byte offset's line, for multiline matches.
                let mut matched = vec![false; lines.len()];
                if multiline {
                    let starts: Vec<usize> = std::iter::once(0).chain(text.match_indices('\n').map(|(i, _)| i + 1)).collect();
                    for found in regex.find_iter(&text) {
                        let first = starts.partition_point(|&s| s <= found.start()).saturating_sub(1);
                        let last = starts.partition_point(|&s| s < found.end().max(found.start() + 1)).saturating_sub(1);
                        for flag in matched.iter_mut().take(last.min(lines.len().saturating_sub(1)) + 1).skip(first) {
                            *flag = true;
                        }
                    }
                } else {
                    for (i, line) in lines.iter().enumerate() {
                        matched[i] = regex.is_match(line);
                    }
                }
                let mut shown_until = 0usize;
                for i in 0..lines.len() {
                    if !matched[i] {
                        continue;
                    }
                    let from = i.saturating_sub(before).max(shown_until);
                    let to = (i + after + 1).min(lines.len());
                    if from > shown_until && shown_until > 0 && (before > 0 || after > 0) {
                        results.push("--".into());
                    }
                    for (j, line) in lines.iter().enumerate().take(to).skip(from) {
                        if results.len() >= limit {
                            truncated = true;
                            break 'files;
                        }
                        let separator = if matched[j] { ':' } else { '-' };
                        let line: String = line.chars().take(500).collect();
                        results.push(match number {
                            true => format!("{shown}{separator}{}{separator}{line}", j + 1),
                            false => format!("{shown}{separator}{line}"),
                        });
                    }
                    shown_until = to;
                }
            }
            "count" => {
                let count = match multiline {
                    true => regex.find_iter(&text).count(),
                    false => text.lines().filter(|line| regex.is_match(line)).count(),
                };
                if count > 0 {
                    results.push(format!("{shown}:{count}"));
                }
            }
            _ => {
                if regex.is_match(&text) {
                    results.push(shown.to_string());
                }
            }
        }
        if results.len() >= limit {
            truncated = true;
            break;
        }
    }
    if results.is_empty() {
        return ok("No matches found");
    }
    let mut out = match mode {
        "files_with_matches" => format!("Found {} file{}\n{}", results.len(), if results.len() == 1 { "" } else { "s" }, results.join("\n")),
        _ => results.join("\n"),
    };
    if truncated {
        out.push_str(&format!("\n[stopped at {} results; narrow the search or raise head_limit]", results.len()));
    }
    ok(shorten(&out, MAX_OUTPUT))
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn glob_and_grep() {
        let dir = std::env::temp_dir().join(format!("claude-search-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(dir.join("src/deep")).unwrap();
        fs::create_dir_all(dir.join(".git")).unwrap();
        fs::write(dir.join("src/a.rs"), "fn main() {}\n// TODO one\nlet x = 1;\n").unwrap();
        fs::write(dir.join("src/deep/b.ts"), "todo two\n").unwrap();
        fs::write(dir.join("notes.md"), "TODO three\n").unwrap();
        fs::write(dir.join(".git/c.rs"), "TODO hidden\n").unwrap();
        let mut c = Context::new(dir.clone());
        let o = glob_tool(&json!({"pattern": "**/*.{rs,ts}"}), &mut c);
        let mut lines: Vec<&str> = o.text.lines().collect();
        lines.sort();
        assert_eq!(lines, [dir.join("src/a.rs").to_str().unwrap(), dir.join("src/deep/b.ts").to_str().unwrap()]);
        let o = grep(&json!({"pattern": "todo", "-i": true, "type": "rust", "output_mode": "content", "-A": 1}), &mut c);
        let a = dir.join("src/a.rs");
        assert_eq!(o.text, format!("{0}:2:// TODO one\n{0}-3-let x = 1;", a.display()));
        let o = grep(&json!({"pattern": "TODO"}), &mut c);
        assert!(o.text.starts_with("Found 2 files"), "{}", o.text);
        let o = grep(&json!({"pattern": "main.*\\n.*TODO", "multiline": true, "output_mode": "content"}), &mut c);
        assert_eq!(o.text.lines().count(), 2, "{}", o.text);
        let o = grep(&json!({"pattern": "O", "output_mode": "count", "glob": "*.md"}), &mut c);
        assert_eq!(o.text, format!("{}:1", dir.join("notes.md").display()));
    }
}
