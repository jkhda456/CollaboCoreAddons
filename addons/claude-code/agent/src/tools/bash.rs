//! Bash, and the shells it leaves running in the background (BashOutput, KillShell, /bashes).
//!
//! A command runs in /bin/sh in its own process group, its output (stdout and stderr together)
//! going to a file: nothing has to drain a pipe while it runs, and a background command's output
//! can be read in pieces. The working directory carries over between calls, as in a terminal.
use std::fs;
use std::io::{Read, Seek, SeekFrom};
use std::os::unix::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::time::{Duration, Instant};

use serde_json::Value;

use super::{fail, shorten, Context, Outcome, MAX_OUTPUT};

pub const DEFAULT_TIMEOUT_MS: u64 = 120_000;
pub const MAX_TIMEOUT_MS: u64 = 600_000;

pub struct Shell {
    pub id: String,
    pub command: String,
    child: Child,
    output: PathBuf,
    /// How far BashOutput has read.
    offset: u64,
    pub started: Instant,
    pub exit: Option<i32>,
    /// The model has been told it finished.
    pub reported: bool,
}

impl Shell {
    pub fn poll(&mut self) -> Option<i32> {
        if self.exit.is_none() {
            if let Ok(Some(status)) = self.child.try_wait() {
                self.exit = Some(status.code().unwrap_or(-1));
            }
        }
        self.exit
    }

    pub fn status(&mut self) -> String {
        match self.poll() {
            None => "running".into(),
            Some(code) => format!("exited with code {code}"),
        }
    }

    fn read_new(&mut self) -> String {
        let Ok(mut file) = fs::File::open(&self.output) else { return String::new() };
        if file.seek(SeekFrom::Start(self.offset)).is_err() {
            return String::new();
        }
        let mut bytes = Vec::new();
        let _ = file.read_to_end(&mut bytes);
        self.offset += bytes.len() as u64;
        String::from_utf8_lossy(&bytes).into_owned()
    }

    pub fn tail(&self, lines: usize) -> String {
        let text = fs::read(&self.output).map(|bytes| String::from_utf8_lossy(&bytes).into_owned()).unwrap_or_default();
        let all: Vec<&str> = text.lines().collect();
        all[all.len().saturating_sub(lines)..].join("\n")
    }

    pub fn kill(&mut self) {
        if self.poll().is_none() {
            // SAFETY: kill(2) on the shell's process group.
            unsafe { libc::kill(-(self.child.id() as libc::pid_t), libc::SIGKILL) };
            let _ = self.child.kill();
            let _ = self.child.wait();
            self.exit = Some(-9);
        }
    }
}

impl Drop for Shell {
    fn drop(&mut self) {
        self.kill();
        let _ = fs::remove_file(&self.output);
    }
}

#[derive(Default)]
pub struct Shells {
    pub list: Vec<Shell>,
    next: usize,
}

impl Shells {
    pub fn get(&mut self, id: &str) -> Option<&mut Shell> {
        self.list.iter_mut().find(|shell| shell.id == id)
    }

    /// Background shells that finished since the model last heard: (id, command, exit code).
    pub fn newly_finished(&mut self) -> Vec<(String, String, i32)> {
        let mut done = Vec::new();
        for shell in self.list.iter_mut() {
            if let (Some(code), false) = (shell.poll(), shell.reported) {
                shell.reported = true;
                done.push((shell.id.clone(), shell.command.clone(), code));
            }
        }
        done
    }

    pub fn running(&mut self) -> usize {
        self.list.iter_mut().map(|shell| shell.poll()).filter(Option::is_none).count()
    }
}

fn temp(name: &str) -> PathBuf {
    std::env::temp_dir().join(format!(".claude-{name}-{}-{}", std::process::id(), crate::util::random_hex(3)))
}

/// Starts `command` in /bin/sh, all output to `output`; `cwd_file` receives the directory it
/// ended in.
fn spawn(command: &str, cwd: &Path, output: &Path, cwd_file: Option<&Path>) -> std::io::Result<Child> {
    let file = fs::File::create(output)?;
    // stderr joins stdout in the shell: File::try_clone is unsupported on wasm32.
    let script = match cwd_file {
        Some(cwd_file) => format!("exec 2>&1\n{command}\n__claude_status=$?\npwd -P > '{}' 2>/dev/null\nexit $__claude_status\n", cwd_file.display()),
        None => format!("exec 2>&1\n{command}\n"),
    };
    Command::new("/bin/sh")
        .arg("-c")
        .arg(&script)
        .current_dir(if cwd.is_dir() { cwd } else { Path::new("/") })
        .stdin(Stdio::null())
        .stdout(file)
        .stderr(Stdio::null())
        .process_group(0)
        .spawn()
}

pub fn bash(input: &Value, context: &mut Context) -> Outcome {
    let Some(command) = input.get("command").and_then(Value::as_str) else { return fail("command is required (a string)") };
    if input.get("run_in_background").and_then(Value::as_bool) == Some(true) {
        let output = temp("bg");
        return match spawn(command, &context.cwd, &output, None) {
            Ok(child) => {
                context.shells.next += 1;
                let id = format!("bash_{}", context.shells.next);
                context.shells.list.push(Shell {
                    id: id.clone(),
                    command: command.to_string(),
                    child,
                    output,
                    offset: 0,
                    started: Instant::now(),
                    exit: None,
                    reported: false,
                });
                super::ok(format!("Command running in background with ID: {id}. Read its output with BashOutput; stop it with KillShell."))
            }
            Err(error) => fail(format!("cannot run /bin/sh: {error}")),
        };
    }

    let timeout = Duration::from_millis(input.get("timeout").and_then(Value::as_f64).map_or(DEFAULT_TIMEOUT_MS, |ms| ms.clamp(1000.0, MAX_TIMEOUT_MS as f64) as u64));
    let (output, cwd_file) = (temp("out"), temp("cwd"));
    let mut child = match spawn(command, &context.cwd, &output, Some(&cwd_file)) {
        Ok(child) => child,
        Err(error) => return fail(format!("cannot run /bin/sh: {error}")),
    };
    let started = Instant::now();
    let (mut timed_out, mut interrupted) = (false, false);
    crate::interrupt::take_background();
    let status = loop {
        match child.try_wait() {
            Ok(Some(status)) => break Some(status),
            Ok(None) if crate::interrupt::take_background() => {
                // Ctrl-B: the command goes on as a background shell; the model gets its id.
                let _ = fs::remove_file(&cwd_file);
                context.shells.next += 1;
                let id = format!("bash_{}", context.shells.next);
                let mut shell = Shell { id: id.clone(), command: command.to_string(), child, output, offset: 0, started, exit: None, reported: false };
                let so_far = shell.read_new();
                context.shells.list.push(shell);
                let so_far = if so_far.trim().is_empty() { "(no output yet)".to_string() } else { shorten(so_far.trim_end(), MAX_OUTPUT) };
                return super::ok(format!(
                    "The user moved this command to the background; it is still running as {id}. Read its output with BashOutput, stop it with KillShell.\nOutput so far:\n{so_far}"
                ));
            }
            Ok(None) => {
                interrupted = crate::interrupt::is_set();
                timed_out = started.elapsed() >= timeout;
                if interrupted || timed_out {
                    // SAFETY: kill(2) on the child's process group.
                    unsafe { libc::kill(-(child.id() as libc::pid_t), libc::SIGKILL) };
                    let _ = child.kill();
                    break child.wait().ok();
                }
                if let Some(progress) = &context.progress {
                    progress(&output);
                }
                std::thread::sleep(Duration::from_millis(20));
            }
            Err(_) => break None,
        }
    };
    let text = fs::read(&output).map(|bytes| String::from_utf8_lossy(&bytes).into_owned()).unwrap_or_default();
    let _ = fs::remove_file(&output);
    if let Ok(cwd) = fs::read_to_string(&cwd_file) {
        let cwd = cwd.trim();
        if !cwd.is_empty() && Path::new(cwd).is_dir() {
            context.cwd = PathBuf::from(cwd);
        }
    }
    let _ = fs::remove_file(&cwd_file);

    let mut text = shorten(text.trim_end(), MAX_OUTPUT);
    let code = status.and_then(|status| status.code());
    if interrupted {
        text.push_str("\n[interrupted by the user]");
    } else if timed_out {
        text.push_str(&format!("\n[timed out after {} s; the command was killed]", timeout.as_secs()));
    } else if code != Some(0) {
        text.push_str(&format!("\n[exit code {}]", code.map_or("none (killed by a signal)".into(), |code| code.to_string())));
    }
    if text.trim().is_empty() {
        text = "(no output)".into();
    }
    Outcome { text: text.trim_start_matches('\n').to_string(), is_error: timed_out || interrupted || code != Some(0), ..Default::default() }
}

pub fn bash_output(input: &Value, context: &mut Context) -> Outcome {
    let Some(id) = input.get("bash_id").or(input.get("shell_id")).and_then(Value::as_str) else { return fail("bash_id is required") };
    let filter = match input.get("filter").and_then(Value::as_str) {
        Some(pattern) => match regex::Regex::new(pattern) {
            Ok(regex) => Some(regex),
            Err(error) => return fail(format!("bad filter: {error}")),
        },
        None => None,
    };
    let Some(shell) = context.shells.get(id) else { return fail(format!("no background shell {id}")) };
    let status = shell.status();
    let mut new = shell.read_new();
    if let Some(filter) = filter {
        new = new.lines().filter(|line| filter.is_match(line)).collect::<Vec<_>>().join("\n");
    }
    if shell.exit.is_some() {
        shell.reported = true;
    }
    let body = match new.trim().is_empty() {
        true => "(no new output)".to_string(),
        false => shorten(new.trim_end(), MAX_OUTPUT),
    };
    super::ok(format!("<status>{status}</status>\n<output>\n{body}\n</output>"))
}

pub fn kill_shell(input: &Value, context: &mut Context) -> Outcome {
    let Some(id) = input.get("shell_id").or(input.get("bash_id")).and_then(Value::as_str) else { return fail("shell_id is required") };
    let Some(shell) = context.shells.get(id) else { return fail(format!("no background shell {id}")) };
    if shell.poll().is_some() {
        return super::ok(format!("{id} had already {}", shell.status()));
    }
    shell.kill();
    super::ok(format!("Killed {id} ({})", shell.command))
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn foreground_and_background() {
        let dir = std::env::temp_dir().join(format!("claude-bash-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(dir.join("sub")).unwrap();
        let mut context = Context::new(dir.clone());
        let o = bash(&json!({"command": "cd sub && echo hi"}), &mut context);
        assert_eq!((o.text.as_str(), o.is_error), ("hi", false));
        assert_eq!(context.cwd, dir.join("sub").canonicalize().unwrap());
        let o = bash(&json!({"command": "echo out; echo err >&2; exit 3"}), &mut context);
        assert_eq!((o.text.as_str(), o.is_error), ("out\nerr\n[exit code 3]", true));
        let o = bash(&json!({"command": "sleep 5", "timeout": 1000}), &mut context);
        assert!(o.is_error && o.text.contains("timed out"));

        let o = bash(&json!({"command": "echo one; sleep 0.3; echo two; exit 4", "run_in_background": true}), &mut context);
        assert!(o.text.contains("bash_1"), "{}", o.text);
        std::thread::sleep(Duration::from_millis(150));
        let first = bash_output(&json!({"bash_id": "bash_1"}), &mut context);
        assert!(first.text.contains("<status>running</status>") && first.text.contains("one") && !first.text.contains("two"), "{}", first.text);
        std::thread::sleep(Duration::from_millis(500));
        assert_eq!(context.shells.newly_finished(), [("bash_1".to_string(), "echo one; sleep 0.3; echo two; exit 4".to_string(), 4)]);
        let second = bash_output(&json!({"bash_id": "bash_1"}), &mut context);
        assert!(second.text.contains("exited with code 4") && second.text.contains("two") && !second.text.contains("one"), "{}", second.text);

        bash(&json!({"command": "sleep 30", "run_in_background": true}), &mut context);
        assert_eq!(context.shells.running(), 1);
        assert!(kill_shell(&json!({"shell_id": "bash_2"}), &mut context).text.starts_with("Killed bash_2"));
        assert_eq!(context.shells.running(), 0);
    }
}
