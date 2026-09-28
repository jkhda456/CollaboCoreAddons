//! Transcripts: every message of a session, one JSON line each, in
//! ~/.claude/projects/<project path, / and . as ->/<session id>.jsonl — what --continue, --resume
//! and /resume pick up again. A compaction writes the new, short conversation as one entry;
//! loading starts from the last one.
use std::fs::{self, File, OpenOptions};
use std::io::Write;
use std::path::{Path, PathBuf};
use std::time::{Duration, SystemTime};

use serde_json::{json, Value};

pub struct Transcript {
    pub id: String,
    pub path: PathBuf,
    cwd: PathBuf,
    file: Option<File>,
}

pub struct Info {
    pub id: String,
    pub title: Option<String>,
    pub path: PathBuf,
    pub modified: SystemTime,
    pub first_prompt: String,
    pub messages: usize,
}

pub fn project_dir(cwd: &Path) -> PathBuf {
    let name: String = cwd.display().to_string().chars().map(|c| if c.is_ascii_alphanumeric() || c == '-' { c } else { '-' }).collect();
    crate::settings::user_dir().join("projects").join(name)
}

impl Transcript {
    pub fn new(cwd: &Path, id: Option<String>) -> Transcript {
        let id = id.unwrap_or_else(crate::util::uuid);
        let path = project_dir(cwd).join(format!("{id}.jsonl"));
        Transcript { id, path, cwd: cwd.to_path_buf(), file: None }
    }

    /// Continues the transcript at `path`.
    pub fn reopen(cwd: &Path, path: &Path) -> Transcript {
        let id = path.file_stem().map(|stem| stem.to_string_lossy().into_owned()).unwrap_or_else(crate::util::uuid);
        Transcript { id, path: path.to_path_buf(), cwd: cwd.to_path_buf(), file: None }
    }

    fn write(&mut self, mut entry: Value) {
        if self.file.is_none() {
            if let Some(dir) = self.path.parent() {
                let _ = fs::create_dir_all(dir);
            }
            self.file = OpenOptions::new().create(true).append(true).open(&self.path).ok();
        }
        if let Some(object) = entry.as_object_mut() {
            object.insert("sessionId".into(), json!(self.id));
            object.insert("timestamp".into(), json!(crate::util::timestamp()));
            object.insert("cwd".into(), json!(self.cwd.display().to_string()));
            object.insert("uuid".into(), json!(crate::util::uuid()));
        }
        if let Some(file) = &mut self.file {
            let _ = writeln!(file, "{entry}");
        }
    }

    pub fn message(&mut self, message: &Value) {
        let kind = message.get("role").and_then(Value::as_str).unwrap_or("user").to_string();
        self.write(json!({"type": kind, "message": message}));
    }

    /// /rename: the name /resume shows.
    pub fn title(&mut self, title: &str) {
        self.write(json!({"type": "title", "title": title}));
    }

    /// After a compaction or /clear: the conversation from here on is `messages`.
    pub fn reset(&mut self, messages: &[Value], reason: &str) {
        self.write(json!({"type": "reset", "reason": reason, "messages": messages}));
    }
}

/// The conversation a transcript holds.
pub fn load(path: &Path) -> Result<Vec<Value>, String> {
    let text = fs::read_to_string(path).map_err(|error| format!("{}: {error}", path.display()))?;
    let mut messages = Vec::new();
    for line in text.lines() {
        let Ok(entry) = serde_json::from_str::<Value>(line) else { continue };
        match entry.get("type").and_then(Value::as_str) {
            Some("user") | Some("assistant") => {
                if let Some(message) = entry.get("message") {
                    messages.push(message.clone());
                }
            }
            Some("reset") => messages = entry.get("messages").and_then(Value::as_array).cloned().unwrap_or_default(),
            _ => {}
        }
    }
    Ok(messages)
}

fn first_prompt(messages: &[Value]) -> String {
    for message in messages {
        if message.get("role").and_then(Value::as_str) != Some("user") {
            continue;
        }
        let text = match message.get("content") {
            Some(Value::String(text)) => text.clone(),
            Some(Value::Array(blocks)) => blocks.iter().filter(|b| b["type"] == "text").filter_map(|b| b["text"].as_str()).collect::<Vec<_>>().join(" "),
            _ => String::new(),
        };
        let text = text.trim();
        if !text.is_empty() && !text.starts_with('<') && !text.starts_with("This session is being continued") {
            return text.lines().next().unwrap_or("").to_string();
        }
    }
    String::new()
}

/// This project's sessions, newest first.
pub fn list(cwd: &Path) -> Vec<Info> {
    let mut sessions: Vec<Info> = fs::read_dir(project_dir(cwd))
        .into_iter()
        .flatten()
        .flatten()
        .filter(|entry| entry.path().extension().is_some_and(|e| e == "jsonl"))
        .filter_map(|entry| {
            let path = entry.path();
            let messages = load(&path).ok()?;
            if messages.is_empty() {
                return None;
            }
            let title = std::fs::read_to_string(&path)
                .ok()?
                .lines()
                .filter_map(|line| serde_json::from_str::<Value>(line).ok())
                .filter(|entry| entry["type"] == "title")
                .last()
                .and_then(|entry| entry["title"].as_str().map(str::to_string));
            Some(Info {
                title,
                id: path.file_stem()?.to_string_lossy().into_owned(),
                modified: entry.metadata().and_then(|m| m.modified()).unwrap_or(SystemTime::UNIX_EPOCH),
                first_prompt: first_prompt(&messages),
                messages: messages.len(),
                path,
            })
        })
        .collect();
    sessions.sort_by(|a, b| b.modified.cmp(&a.modified));
    sessions
}

/// Transcripts older than `days` go (settings cleanupPeriodDays, default 30).
pub fn cleanup(days: u64) {
    let cutoff = SystemTime::now().checked_sub(Duration::from_secs(days * 86_400)).unwrap_or(SystemTime::UNIX_EPOCH);
    let root = crate::settings::user_dir().join("projects");
    for project in fs::read_dir(root).into_iter().flatten().flatten() {
        for entry in fs::read_dir(project.path()).into_iter().flatten().flatten() {
            if entry.metadata().and_then(|m| m.modified()).is_ok_and(|time| time < cutoff) {
                let _ = fs::remove_file(entry.path());
            }
        }
    }
}

/// A session by id (or a unique id prefix) in this project.
pub fn find(cwd: &Path, id: &str) -> Option<PathBuf> {
    let matches: Vec<Info> = list(cwd).into_iter().filter(|info| info.id.starts_with(id)).collect();
    match matches.as_slice() {
        [one] => Some(one.path.clone()),
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn record_and_load() {
        let dir = std::env::temp_dir().join(format!("claude-session-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        let path = dir.join("s.jsonl");
        let mut t = Transcript { id: "s".into(), path: path.clone(), cwd: dir.clone(), file: None };
        t.message(&json!({"role": "user", "content": [{"type": "text", "text": "first question"}]}));
        t.message(&json!({"role": "assistant", "content": [{"type": "text", "text": "answer"}]}));
        assert_eq!(load(&path).unwrap().len(), 2);
        t.reset(&[json!({"role": "user", "content": "summary"})], "compact");
        t.message(&json!({"role": "user", "content": "next"}));
        let messages = load(&path).unwrap();
        assert_eq!(messages, [json!({"role": "user", "content": "summary"}), json!({"role": "user", "content": "next"})]);
        assert_eq!(first_prompt(&[json!({"role": "user", "content": [{"type": "text", "text": "hello\nthere"}]})]), "hello");
    }
}
