//! Hooks: shell commands the settings attach to events, as in Claude Code.
//!
//!   "hooks": {"PreToolUse": [{"matcher": "Bash|Edit", "hooks": [{"type": "command", "command": "…", "timeout": 60}]}]}
//!
//! Events: PreToolUse, PostToolUse (matched on the tool name), UserPromptSubmit, Stop,
//! SubagentStop, SessionStart (matched on startup|resume|clear|compact), SessionEnd,
//! Notification, PreCompact (manual|auto). A hook gets the event as JSON on stdin (session_id,
//! transcript_path, cwd, hook_event_name, and the event's own fields) and CLAUDE_PROJECT_DIR in
//! its environment.
//!
//! What it says back:
//!   exit 0        fine; for UserPromptSubmit and SessionStart, stdout is added as context
//!   exit 2        blocks: PreToolUse refuses the call, PostToolUse and Stop hand stderr to the
//!                 model (Stop: keep going), UserPromptSubmit drops the prompt
//!   other exits   an error shown to the user; nothing is blocked
//!   JSON stdout   {"continue": false, "stopReason"}, {"decision": "block", "reason"},
//!                 {"systemMessage"}, {"hookSpecificOutput": {"permissionDecision": "allow" |
//!                 "deny" | "ask", "permissionDecisionReason", "additionalContext", "updatedInput"}}
use std::fs;
use std::path::PathBuf;
use std::process::Command;
use std::time::{Duration, Instant};

use serde_json::{json, Value};

#[derive(Default, Debug)]
pub struct Outcome {
    /// Exit 2 or decision "block": why.
    pub blocked: Option<String>,
    /// PreToolUse's say on the call: ("allow" | "deny" | "ask", reason).
    pub permission: Option<(String, String)>,
    pub context: Vec<String>,
    /// continue: false — end the turn, with this reason.
    pub stop: Option<String>,
    /// For the user: systemMessage, and hooks that failed.
    pub messages: Vec<String>,
    pub updated_input: Option<Value>,
    pub ran: usize,
}

#[derive(Clone)]
pub struct Hooks {
    pub config: Value,
    pub session_id: String,
    pub transcript_path: String,
    pub project_dir: PathBuf,
}

fn matches(matcher: Option<&str>, subject: Option<&str>) -> bool {
    let matcher = matcher.unwrap_or("").trim();
    if matcher.is_empty() || matcher == "*" {
        return true;
    }
    let Some(subject) = subject else { return true };
    match regex::Regex::new(&format!("^(?:{matcher})$")) {
        Ok(regex) => regex.is_match(subject),
        Err(_) => matcher == subject,
    }
}

impl Hooks {
    /// The hooks that apply: (command, timeout, the message for an asyncRewake hook). `call`
    /// (tool, input) is checked against a hook's "if" rule, e.g. "Bash(git commit:*)".
    fn entries(&self, event: &str, subject: Option<&str>, call: Option<(&str, &Value)>, cwd: &std::path::Path) -> Vec<(String, Duration, Option<String>)> {
        let mut commands = Vec::new();
        for group in self.config.get(event).and_then(Value::as_array).into_iter().flatten() {
            if !matches(group.get("matcher").and_then(Value::as_str), subject) {
                continue;
            }
            for hook in group.get("hooks").and_then(Value::as_array).into_iter().flatten() {
                if hook.get("type").and_then(Value::as_str).unwrap_or("command") != "command" {
                    continue;
                }
                if let (Some(rule), Some((tool, input))) = (hook.get("if").and_then(Value::as_str), call) {
                    if !crate::permissions::rule_matches(rule, tool, input, cwd) {
                        continue;
                    }
                }
                if let Some(command) = hook.get("command").and_then(Value::as_str) {
                    let timeout = hook.get("timeout").and_then(Value::as_f64).unwrap_or(60.0).max(1.0);
                    // asyncRewake: its findings come back to Claude as a message (here, at once).
                    let rewake = (hook.get("asyncRewake").and_then(Value::as_bool) == Some(true))
                        .then(|| hook.get("rewakeMessage").and_then(Value::as_str).unwrap_or("A background hook reported:").to_string());
                    commands.push((command.to_string(), Duration::from_secs_f64(timeout), rewake));
                }
            }
        }
        commands
    }

    pub fn run(&self, event: &str, subject: Option<&str>, cwd: &std::path::Path, fields: Value) -> Outcome {
        let mut outcome = Outcome::default();
        let call = match (fields.get("tool_name").and_then(Value::as_str), fields.get("tool_input")) {
            (Some(tool), Some(input)) => Some((tool.to_string(), input.clone())),
            _ => None,
        };
        let commands = self.entries(event, subject, call.as_ref().map(|(t, i)| (t.as_str(), i)), cwd);
        if commands.is_empty() {
            return outcome;
        }
        let mut input = json!({
            "session_id": self.session_id,
            "transcript_path": self.transcript_path,
            "cwd": cwd.display().to_string(),
            "hook_event_name": event,
        });
        if let (Some(input), Value::Object(fields)) = (input.as_object_mut(), fields) {
            input.extend(fields);
        }
        let stdin = serde_json::to_vec(&input).expect("hook input serializes");
        for (command, timeout, rewake) in commands {
            outcome.ran += 1;
            let (code, stdout, stderr) = match execute(&command, &stdin, cwd, &self.project_dir, timeout) {
                Ok(result) => result,
                Err(error) => {
                    outcome.messages.push(format!("{event} hook \"{command}\" failed: {error}"));
                    continue;
                }
            };
            if let Some(message) = rewake {
                let report = if code == Some(2) { stderr.trim() } else { stdout.trim() };
                if code.is_some_and(|c| c == 0 || c == 2) && !report.is_empty() {
                    outcome.blocked = Some(format!("{message}\n{report}"));
                }
                continue;
            }
            match code {
                Some(0) => self.read_output(event, &stdout, &mut outcome),
                Some(2) => {
                    let reason = stderr.trim().to_string();
                    outcome.blocked = Some(if reason.is_empty() { format!("blocked by the {event} hook") } else { reason });
                }
                Some(code) => outcome.messages.push(format!("{event} hook \"{command}\" exited with {code}: {}", stderr.trim())),
                None => outcome.messages.push(format!("{event} hook \"{command}\" timed out after {} s", timeout.as_secs())),
            }
        }
        outcome
    }

    fn read_output(&self, event: &str, stdout: &str, outcome: &mut Outcome) {
        let trimmed = stdout.trim();
        let parsed = trimmed.starts_with('{').then(|| serde_json::from_str::<Value>(trimmed).ok()).flatten();
        let Some(output) = parsed else {
            if matches!(event, "UserPromptSubmit" | "SessionStart") && !trimmed.is_empty() {
                outcome.context.push(trimmed.to_string());
            }
            return;
        };
        if output.get("continue").and_then(Value::as_bool) == Some(false) {
            outcome.stop = Some(output.get("stopReason").and_then(Value::as_str).unwrap_or("stopped by a hook").to_string());
        }
        if let Some(message) = output.get("systemMessage").and_then(Value::as_str) {
            outcome.messages.push(message.to_string());
        }
        let reason = output.get("reason").and_then(Value::as_str).unwrap_or("").to_string();
        match output.get("decision").and_then(Value::as_str) {
            Some("block") => outcome.blocked = Some(if reason.is_empty() { format!("blocked by the {event} hook") } else { reason.clone() }),
            // The old spelling of permissionDecision.
            Some("approve") if event == "PreToolUse" => outcome.permission = Some(("allow".into(), reason.clone())),
            _ => {}
        }
        if let Some(specific) = output.get("hookSpecificOutput") {
            if let Some(decision) = specific.get("permissionDecision").and_then(Value::as_str) {
                let why = specific.get("permissionDecisionReason").and_then(Value::as_str).unwrap_or("").to_string();
                outcome.permission = Some((decision.to_string(), why));
            }
            if let Some(context) = specific.get("additionalContext").and_then(Value::as_str) {
                outcome.context.push(context.to_string());
            }
            if let Some(updated) = specific.get("updatedInput").filter(|value| value.is_object()) {
                outcome.updated_input = Some(updated.clone());
            }
        }
    }
}

/// Runs one hook: stdin from a file, stdout and stderr to files (no pipes to drain), killed at
/// the timeout. (exit code or None on timeout, stdout, stderr)
fn execute(command: &str, stdin: &[u8], cwd: &std::path::Path, project: &std::path::Path, timeout: Duration) -> std::io::Result<(Option<i32>, String, String)> {
    use std::os::unix::process::CommandExt;
    let base = std::env::temp_dir().join(format!(".claude-hook-{}-{}", std::process::id(), crate::util::random_hex(4)));
    let (input, output, errors) = (base.with_extension("in"), base.with_extension("out"), base.with_extension("err"));
    fs::write(&input, stdin)?;
    let cleanup = || {
        let _ = fs::remove_file(&input);
        let _ = fs::remove_file(&output);
        let _ = fs::remove_file(&errors);
    };
    let spawned = Command::new("/bin/sh")
        .arg("-c")
        .arg(command)
        .current_dir(if cwd.is_dir() { cwd } else { std::path::Path::new("/") })
        .env("CLAUDE_PROJECT_DIR", project)
        .stdin(fs::File::open(&input)?)
        .stdout(fs::File::create(&output)?)
        .stderr(fs::File::create(&errors)?)
        .process_group(0)
        .spawn();
    let mut child = match spawned {
        Ok(child) => child,
        Err(error) => {
            cleanup();
            return Err(error);
        }
    };
    let started = Instant::now();
    let code = loop {
        match child.try_wait()? {
            Some(status) => break Some(status.code().unwrap_or(1)),
            None if started.elapsed() >= timeout => {
                // SAFETY: kill(2) on the hook's process group.
                unsafe { libc::kill(-(child.id() as libc::pid_t), libc::SIGKILL) };
                let _ = child.wait();
                break None;
            }
            None => std::thread::sleep(Duration::from_millis(10)),
        }
    };
    let stdout = fs::read_to_string(&output).unwrap_or_default();
    let stderr = fs::read_to_string(&errors).unwrap_or_default();
    cleanup();
    Ok((code, stdout, stderr))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn hooks(config: Value) -> Hooks {
        Hooks { config, session_id: "s1".into(), transcript_path: "/tmp/t.jsonl".into(), project_dir: std::env::temp_dir() }
    }

    #[test]
    fn exit_codes_and_json() {
        let h = hooks(json!({
            "PreToolUse": [
                {"matcher": "Bash", "hooks": [{"type": "command", "command": "grep -q 'rm -rf' && { echo 'no rm -rf' >&2; exit 2; } || exit 0"}]},
                {"matcher": "Edit|Write", "hooks": [{"type": "command", "command": "echo '{\"hookSpecificOutput\": {\"permissionDecision\": \"allow\", \"permissionDecisionReason\": \"ok\"}}'"}]}
            ],
            "UserPromptSubmit": [{"hooks": [{"type": "command", "command": "cat >/dev/null; echo context-from-hook"}]}],
            "Stop": [{"hooks": [{"type": "command", "command": "echo '{\"decision\": \"block\", \"reason\": \"run the tests\"}'"}]}],
            "Notification": [{"hooks": [{"type": "command", "command": "exit 3"}]}],
            "SessionStart": [{"matcher": "startup", "hooks": [{"type": "command", "command": "sed -n 's/.*\"hook_event_name\":\"\\([A-Za-z]*\\)\".*/\\1/p'"}]}]
        }));
        let cwd = std::env::temp_dir();
        let o = h.run("PreToolUse", Some("Bash"), &cwd, json!({"tool_name": "Bash", "tool_input": {"command": "rm -rf /"}}));
        assert_eq!(o.blocked.as_deref(), Some("no rm -rf"));
        let o = h.run("PreToolUse", Some("Bash"), &cwd, json!({"tool_name": "Bash", "tool_input": {"command": "ls"}}));
        assert!(o.blocked.is_none() && o.ran == 1);
        let o = h.run("PreToolUse", Some("Write"), &cwd, json!({}));
        assert_eq!(o.permission, Some(("allow".into(), "ok".into())));
        assert_eq!(h.run("PreToolUse", Some("Read"), &cwd, json!({})).ran, 0);
        assert_eq!(h.run("UserPromptSubmit", None, &cwd, json!({"prompt": "hi"})).context, ["context-from-hook"]);
        assert_eq!(h.run("Stop", None, &cwd, json!({})).blocked.as_deref(), Some("run the tests"));
        assert!(h.run("Notification", None, &cwd, json!({})).messages[0].contains("exited with 3"));
        assert_eq!(h.run("SessionStart", Some("startup"), &cwd, json!({"source": "startup"})).context, ["SessionStart"]);
        assert_eq!(h.run("SessionStart", Some("resume"), &cwd, json!({})).ran, 0);
    }
}
