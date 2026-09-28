//! Read, Write, Edit, MultiEdit, NotebookEdit and LS.
use std::fs;
use std::path::{Path, PathBuf};
use std::time::SystemTime;

use serde_json::{json, Value};

use super::{fail, ok, Context, Display, Outcome};

const READ_LINES: usize = 2000;
const READ_LINE_CHARS: usize = 2000;
const MAX_TEXT_BYTES: u64 = 256 * 1024;
const MAX_IMAGE_BYTES: u64 = 5 * 1024 * 1024;

fn modified(path: &Path) -> Option<SystemTime> {
    fs::metadata(path).and_then(|meta| meta.modified()).ok()
}

fn looks_binary(bytes: &[u8]) -> bool {
    bytes[..bytes.len().min(8192)].contains(&0)
}

fn string<'a>(input: &'a Value, key: &str) -> Result<&'a str, Outcome> {
    input.get(key).and_then(Value::as_str).ok_or_else(|| fail(format!("InputValidationError: {key} is required (a string)")))
}

/// The session remembers what it has seen of a file.
fn remember(context: &mut Context, path: &Path) {
    if let Some(time) = modified(path) {
        context.read_files.insert(path.to_path_buf(), time);
    }
}

/// An existing file may change only after it was read, and only if it has not changed since.
fn check_read(context: &Context, path: &Path) -> Result<(), Outcome> {
    if !path.exists() {
        return Ok(());
    }
    match context.read_files.get(path) {
        None => Err(fail("File has not been read yet. Read it first before writing to it.")),
        Some(seen) if modified(path).is_some_and(|now| now > *seen) => Err(fail(
            "File has been modified since read, either by the user or by a linter. Read it again before attempting to write it.",
        )),
        Some(_) => Ok(()),
    }
}

fn image_type(path: &Path) -> Option<&'static str> {
    match path.extension()?.to_str()?.to_ascii_lowercase().as_str() {
        "png" => Some("image/png"),
        "jpg" | "jpeg" => Some("image/jpeg"),
        "gif" => Some("image/gif"),
        "webp" => Some("image/webp"),
        _ => None,
    }
}

fn numbered<'a>(lines: impl Iterator<Item = (usize, &'a str)>) -> String {
    let mut out = String::new();
    for (index, line) in lines {
        let line: String = match line.chars().count() > READ_LINE_CHARS {
            true => line.chars().take(READ_LINE_CHARS).collect::<String>() + "…",
            false => line.to_string(),
        };
        out.push_str(&format!("{:>6}\t{line}\n", index + 1));
    }
    out
}

fn cell_source(cell: &Value) -> String {
    match cell.get("source") {
        Some(Value::Array(parts)) => parts.iter().filter_map(Value::as_str).collect(),
        Some(Value::String(text)) => text.clone(),
        _ => String::new(),
    }
}

fn notebook_text(notebook: &Value) -> String {
    let mut out = String::new();
    for (index, cell) in notebook.get("cells").and_then(Value::as_array).into_iter().flatten().enumerate() {
        let id = cell.get("id").and_then(Value::as_str).map(str::to_string).unwrap_or_else(|| format!("cell-{index}"));
        let kind = cell.get("cell_type").and_then(Value::as_str).unwrap_or("code");
        out.push_str(&format!("<cell id=\"{id}\" type=\"{kind}\">\n{}\n", cell_source(cell)));
        for output in cell.get("outputs").and_then(Value::as_array).into_iter().flatten() {
            let text = match output.get("text") {
                Some(Value::Array(parts)) => parts.iter().filter_map(Value::as_str).collect::<String>(),
                Some(Value::String(text)) => text.clone(),
                _ => output.pointer("/data/text~1plain").map(|plain| match plain {
                    Value::Array(parts) => parts.iter().filter_map(Value::as_str).collect(),
                    other => other.as_str().unwrap_or("").to_string(),
                }).unwrap_or_default(),
            };
            if !text.is_empty() {
                out.push_str(&format!("<output>\n{}\n</output>\n", super::shorten(text.trim_end(), 5000)));
            }
        }
        out.push_str("</cell>\n");
    }
    out
}

pub fn read(input: &Value, context: &mut Context) -> Outcome {
    let path = match string(input, "file_path") {
        Ok(path) => context.resolve(path),
        Err(error) => return error,
    };
    let meta = match fs::metadata(&path) {
        Ok(meta) => meta,
        Err(_) => {
            let hint = path.parent().and_then(|dir| fs::read_dir(dir).ok()).and_then(|entries| {
                let name = path.file_name()?.to_string_lossy().to_lowercase();
                entries.flatten().map(|e| e.file_name().to_string_lossy().into_owned()).find(|other| other.to_lowercase() == name)
            });
            return fail(match hint {
                Some(other) => format!("File does not exist: {}. Did you mean {other}?", path.display()),
                None => format!("File does not exist: {}", path.display()),
            });
        }
    };
    if meta.is_dir() {
        return fail(format!("{} is a directory; use LS or Glob", path.display()));
    }
    if let Some(media_type) = image_type(&path) {
        if meta.len() > MAX_IMAGE_BYTES {
            return fail(format!("{} is {} bytes; images over 5 MB cannot be read", path.display(), meta.len()));
        }
        let bytes = match fs::read(&path) {
            Ok(bytes) => bytes,
            Err(error) => return fail(format!("{}: {error}", path.display())),
        };
        remember(context, &path);
        return Outcome {
            text: format!("[image {} ({} bytes)]", path.display(), bytes.len()),
            content: Some(json!([{"type": "image", "source": {"type": "base64", "media_type": media_type, "data": crate::util::base64(&bytes)}}])),
            ..Default::default()
        };
    }
    let offset = input.get("offset").and_then(Value::as_u64).unwrap_or(1).max(1) as usize;
    let limit = input.get("limit").and_then(Value::as_u64).map(|n| n.max(1) as usize);
    if meta.len() > MAX_TEXT_BYTES && input.get("offset").is_none() && limit.is_none() {
        return fail(format!(
            "{} is {} KB, more than can be read at once (256 KB). Read it in parts with offset and limit, or search it with Grep.",
            path.display(),
            meta.len() / 1024
        ));
    }
    let bytes = match fs::read(&path) {
        Ok(bytes) => bytes,
        Err(error) => return fail(format!("{}: {error}", path.display())),
    };
    if looks_binary(&bytes) {
        return fail(format!("{} is a binary file ({} bytes)", path.display(), bytes.len()));
    }
    remember(context, &path);
    if path.extension().and_then(|e| e.to_str()) == Some("ipynb") {
        if let Ok(notebook) = serde_json::from_slice::<Value>(&bytes) {
            return ok(notebook_text(&notebook));
        }
    }
    let text = String::from_utf8_lossy(&bytes);
    if text.is_empty() {
        return ok("<system-reminder>Warning: the file exists but its contents are empty.</system-reminder>");
    }
    let limit = limit.unwrap_or(READ_LINES);
    let total = text.lines().count();
    let mut out = numbered(text.lines().enumerate().skip(offset - 1).take(limit));
    if out.is_empty() {
        return fail(format!("<system-reminder>Warning: the file has {total} lines; offset {offset} is past the end.</system-reminder>"));
    }
    let last = (offset - 1 + limit).min(total);
    if last < total {
        out.push_str(&format!("[lines {offset}-{last} of {total}; read on with offset {}]\n", last + 1));
    }
    ok(out)
}

fn write_file(path: &Path, content: &str) -> Result<(), String> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent).map_err(|error| format!("{}: {error}", parent.display()))?;
    }
    fs::write(path, content).map_err(|error| format!("{}: {error}", path.display()))
}

pub fn write(input: &Value, context: &mut Context) -> Outcome {
    let (path, content) = match (string(input, "file_path"), string(input, "content")) {
        (Ok(path), Ok(content)) => (context.resolve(path), content),
        (Err(error), _) | (_, Err(error)) => return error,
    };
    if let Err(refused) = check_read(context, &path) {
        return refused;
    }
    let before = fs::read_to_string(&path).ok();
    if let Err(message) = write_file(&path, content) {
        return fail(message);
    }
    remember(context, &path);
    Outcome {
        text: match &before {
            Some(_) => format!("The file {} has been updated.", path.display()),
            None => format!("File created successfully at: {}", path.display()),
        },
        display: Some(Display::Diff { path: path.clone(), before: before.unwrap_or_default(), after: content.to_string() }),
        ..Default::default()
    }
}

/// One replacement on `text`: the new text, or why not.
fn replace(text: &str, old: &str, new: &str, replace_all: bool) -> Result<(String, usize), String> {
    if old == new {
        return Err("No changes to make: old_string and new_string are exactly the same.".into());
    }
    if old.is_empty() {
        return Err("old_string is empty: name the text to replace (an empty old_string only creates a new file).".into());
    }
    let count = text.matches(old).count();
    if count == 0 {
        return Err("String to replace not found in file. It must match exactly, whitespace and indentation included; Read the file again.".into());
    }
    if count > 1 && !replace_all {
        return Err(format!(
            "Found {count} matches of the string to replace, but replace_all is false. To replace all occurrences, set replace_all to true. To replace only one, add more surrounding context to make it unique."
        ));
    }
    Ok(match replace_all {
        true => (text.replace(old, new), count),
        false => (text.replacen(old, new, 1), 1),
    })
}

fn apply_edits(input: &Value, context: &mut Context, path: PathBuf, edits: &[Value]) -> Outcome {
    let exists = path.exists();
    // An empty old_string on a file that does not exist creates it.
    if !exists {
        if let [only] = edits {
            if only.get("old_string").and_then(Value::as_str) == Some("") {
                let new = only.get("new_string").and_then(Value::as_str).unwrap_or("");
                return write(&json!({"file_path": path.to_string_lossy(), "content": new}), context);
            }
        }
        return fail(format!("File does not exist: {}", path.display()));
    }
    if let Err(refused) = check_read(context, &path) {
        return refused;
    }
    let before = match fs::read_to_string(&path) {
        Ok(text) => text,
        Err(error) => return fail(format!("{}: {error}", path.display())),
    };
    let mut text = before.clone();
    let mut replaced = 0;
    for (index, edit) in edits.iter().enumerate() {
        let (Some(old), Some(new)) = (edit.get("old_string").and_then(Value::as_str), edit.get("new_string").and_then(Value::as_str)) else {
            return fail(format!("InputValidationError: edit {} needs old_string and new_string", index + 1));
        };
        let all = edit.get("replace_all").and_then(Value::as_bool).unwrap_or(false);
        match replace(&text, old, new, all) {
            Ok((next, count)) => {
                text = next;
                replaced += count;
            }
            Err(message) if edits.len() > 1 => return fail(format!("Edit {} of {}: {message} (no edit was made)", index + 1, edits.len())),
            Err(message) => return fail(message),
        }
    }
    if let Err(message) = write_file(&path, &text) {
        return fail(message);
    }
    remember(context, &path);
    let _ = input;
    // The model gets the changed region, numbered, to check it.
    let first_changed = before.lines().zip(text.lines()).take_while(|(a, b)| a == b).count();
    let changed_lines = text.lines().count().saturating_sub(before.lines().count()).max(1) + 4;
    let excerpt = text
        .lines()
        .enumerate()
        .skip(first_changed.saturating_sub(4))
        .take(changed_lines + 8)
        .map(|(index, line)| format!("{:>6}\t{line}", index + 1))
        .collect::<Vec<_>>()
        .join("\n");
    Outcome {
        text: format!(
            "The file {} has been updated ({replaced} replacement{}). Here's a snippet of the edited file:\n{excerpt}",
            path.display(),
            if replaced == 1 { "" } else { "s" }
        ),
        display: Some(Display::Diff { path, before, after: text }),
        ..Default::default()
    }
}

pub fn edit(input: &Value, context: &mut Context) -> Outcome {
    let path = match string(input, "file_path") {
        Ok(path) => context.resolve(path),
        Err(error) => return error,
    };
    for key in ["old_string", "new_string"] {
        if let Err(error) = string(input, key) {
            return error;
        }
    }
    let edit = json!({"old_string": input["old_string"], "new_string": input["new_string"], "replace_all": input.get("replace_all").cloned().unwrap_or(json!(false))});
    apply_edits(input, context, path, &[edit])
}

pub fn multi_edit(input: &Value, context: &mut Context) -> Outcome {
    let path = match string(input, "file_path") {
        Ok(path) => context.resolve(path),
        Err(error) => return error,
    };
    let Some(edits) = input.get("edits").and_then(Value::as_array).filter(|edits| !edits.is_empty()) else {
        return fail("InputValidationError: edits must be a non-empty array");
    };
    let edits = edits.clone();
    apply_edits(input, context, path, &edits)
}

pub fn notebook_edit(input: &Value, context: &mut Context) -> Outcome {
    let path = match string(input, "notebook_path") {
        Ok(path) => context.resolve(path),
        Err(error) => return error,
    };
    let source = input.get("new_source").and_then(Value::as_str).unwrap_or("");
    let mode = input.get("edit_mode").and_then(Value::as_str).unwrap_or("replace");
    if let Err(refused) = check_read(context, &path) {
        return refused;
    }
    let before = match fs::read_to_string(&path) {
        Ok(text) => text,
        Err(error) => return fail(format!("{}: {error}", path.display())),
    };
    let mut notebook: Value = match serde_json::from_str(&before) {
        Ok(notebook) => notebook,
        Err(error) => return fail(format!("{} is not a notebook: {error}", path.display())),
    };
    let Some(cells) = notebook.get_mut("cells").and_then(Value::as_array_mut) else { return fail("the notebook has no cells array") };
    let index = match input.get("cell_id").and_then(Value::as_str) {
        None => None,
        Some(id) => match cells.iter().position(|cell| cell.get("id").and_then(Value::as_str) == Some(id)) {
            Some(index) => Some(index),
            None => match id.strip_prefix("cell-").and_then(|n| n.parse::<usize>().ok()).filter(|n| *n < cells.len()) {
                Some(index) => Some(index),
                None => return fail(format!("no cell {id} in the notebook")),
            },
        },
    };
    // Sources are stored as lines, each keeping its newline.
    let lines: Vec<Value> = source.split_inclusive('\n').map(|line| json!(line)).collect();
    let message = match mode {
        "insert" => {
            let kind = input.get("cell_type").and_then(Value::as_str).unwrap_or("code");
            let mut cell = json!({"cell_type": kind, "id": crate::util::random_hex(4), "metadata": {}, "source": lines});
            if kind == "code" {
                cell["outputs"] = json!([]);
                cell["execution_count"] = Value::Null;
            }
            let at = index.map_or(0, |i| i + 1);
            cells.insert(at, cell);
            format!("Inserted a {kind} cell at position {at}")
        }
        "delete" => {
            let Some(index) = index else { return fail("delete needs cell_id") };
            cells.remove(index);
            format!("Deleted cell {index}")
        }
        _ => {
            let Some(index) = index else { return fail("replace needs cell_id") };
            let cell = &mut cells[index];
            cell["source"] = json!(lines);
            if let Some(kind) = input.get("cell_type").and_then(Value::as_str) {
                cell["cell_type"] = json!(kind);
            }
            if cell.get("cell_type").and_then(Value::as_str) == Some("code") {
                cell["outputs"] = json!([]);
                cell["execution_count"] = Value::Null;
            }
            format!("Updated cell {index}")
        }
    };
    let after = serde_json::to_string_pretty(&notebook).expect("notebook serializes") + "\n";
    if let Err(message) = write_file(&path, &after) {
        return fail(message);
    }
    remember(context, &path);
    ok(format!("{message} in {}", path.display()))
}

pub fn ls(input: &Value, context: &mut Context) -> Outcome {
    let path = context.resolve(input.get("path").and_then(Value::as_str).unwrap_or("."));
    let ignore: Vec<glob::Pattern> = input
        .get("ignore")
        .and_then(Value::as_array)
        .into_iter()
        .flatten()
        .filter_map(Value::as_str)
        .filter_map(|pattern| glob::Pattern::new(pattern).ok())
        .collect();
    let entries = match fs::read_dir(&path) {
        Ok(entries) => entries,
        Err(error) => return fail(format!("{}: {error}", path.display())),
    };
    let mut rows: Vec<(bool, String, u64)> = entries
        .flatten()
        .filter_map(|entry| {
            let name = entry.file_name().to_string_lossy().into_owned();
            if ignore.iter().any(|pattern| pattern.matches(&name)) {
                return None;
            }
            let meta = entry.metadata().ok()?;
            Some((meta.is_dir(), name, meta.len()))
        })
        .collect();
    rows.sort_by(|a, b| b.0.cmp(&a.0).then(a.1.cmp(&b.1)));
    let total = rows.len();
    let mut out = format!("- {}/\n", path.display().to_string().trim_end_matches('/'));
    for (dir, name, size) in rows.iter().take(1000) {
        out.push_str(&match dir {
            true => format!("  - {name}/\n"),
            false => format!("  - {name} ({size} bytes)\n"),
        });
    }
    if total > 1000 {
        out.push_str(&format!("  … and {} more\n", total - 1000));
    }
    ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn scratch(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("claude-files-{name}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        dir
    }

    #[test]
    fn read_before_edit() {
        let dir = scratch("rbe");
        fs::write(dir.join("a.txt"), "one\ntwo\ntwo\n").unwrap();
        let mut c = Context::new(dir.clone());
        let o = edit(&json!({"file_path": "a.txt", "old_string": "one", "new_string": "1"}), &mut c);
        assert!(o.is_error && o.text.contains("has not been read"), "{}", o.text);
        assert!(!read(&json!({"file_path": "a.txt"}), &mut c).is_error);
        let o = edit(&json!({"file_path": "a.txt", "old_string": "two", "new_string": "2"}), &mut c);
        assert!(o.is_error && o.text.contains("Found 2 matches"));
        let o = edit(&json!({"file_path": "a.txt", "old_string": "one", "new_string": "1"}), &mut c);
        assert!(!o.is_error, "{}", o.text);
        assert!(matches!(o.display, Some(Display::Diff { .. })));
        let o = multi_edit(&json!({"file_path": "a.txt", "edits": [
            {"old_string": "two", "new_string": "2", "replace_all": true}, {"old_string": "1\n2", "new_string": "one\ntwo"}]}), &mut c);
        assert!(!o.is_error, "{}", o.text);
        assert_eq!(fs::read_to_string(dir.join("a.txt")).unwrap(), "one\ntwo\n2\n");
        let o = multi_edit(&json!({"file_path": "a.txt", "edits": [{"old_string": "one", "new_string": "x"}, {"old_string": "zzz", "new_string": "y"}]}), &mut c);
        assert!(o.is_error && o.text.starts_with("Edit 2 of 2"));
        assert_eq!(fs::read_to_string(dir.join("a.txt")).unwrap(), "one\ntwo\n2\n");
        // Changed behind the session's back: read again first.
        std::thread::sleep(std::time::Duration::from_millis(1100));
        fs::write(dir.join("a.txt"), "changed\n").unwrap();
        let o = write(&json!({"file_path": "a.txt", "content": "new"}), &mut c);
        assert!(o.is_error && o.text.contains("modified since read"));
        let o = write(&json!({"file_path": "new/b.txt", "content": "fresh"}), &mut c);
        assert!(!o.is_error && o.text.starts_with("File created"));
        let o = edit(&json!({"file_path": "c.txt", "old_string": "", "new_string": "made"}), &mut c);
        assert!(!o.is_error);
        assert_eq!(fs::read_to_string(dir.join("c.txt")).unwrap(), "made");
    }

    #[test]
    fn images_notebooks_listing() {
        let dir = scratch("misc");
        fs::write(dir.join("p.png"), [0x89, b'P', b'N', b'G', 0, 1]).unwrap();
        let mut c = Context::new(dir.clone());
        let o = read(&json!({"file_path": "p.png"}), &mut c);
        assert_eq!(o.content.unwrap()[0]["source"]["media_type"], "image/png");
        let nb = json!({"cells": [{"cell_type": "code", "id": "abc", "source": ["print(1)\n"], "outputs": [{"text": ["1\n"]}]}], "metadata": {}, "nbformat": 4, "nbformat_minor": 5});
        fs::write(dir.join("n.ipynb"), nb.to_string()).unwrap();
        let o = read(&json!({"file_path": "n.ipynb"}), &mut c);
        assert!(o.text.contains("<cell id=\"abc\" type=\"code\">\nprint(1)") && o.text.contains("<output>\n1\n</output>"), "{}", o.text);
        let o = notebook_edit(&json!({"notebook_path": "n.ipynb", "cell_id": "abc", "new_source": "x = 2\nx", "edit_mode": "insert", "cell_type": "markdown"}), &mut c);
        assert!(!o.is_error, "{}", o.text);
        let o = notebook_edit(&json!({"notebook_path": "n.ipynb", "cell_id": "cell-0", "new_source": "print(3)\n"}), &mut c);
        assert!(!o.is_error, "{}", o.text);
        let saved: Value = serde_json::from_str(&fs::read_to_string(dir.join("n.ipynb")).unwrap()).unwrap();
        assert_eq!(saved["cells"][0]["source"], json!(["print(3)\n"]));
        assert_eq!(saved["cells"][1]["source"], json!(["x = 2\n", "x"]));
        fs::create_dir(dir.join("sub")).unwrap();
        let o = ls(&json!({"ignore": ["*.png"]}), &mut c);
        assert!(o.text.contains("  - sub/\n") && o.text.contains("n.ipynb") && !o.text.contains("p.png"), "{}", o.text);
    }
}
