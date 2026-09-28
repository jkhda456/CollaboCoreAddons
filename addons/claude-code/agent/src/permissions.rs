//! Permission modes and rules, as in Claude Code.
//!
//! Modes: default (ask before commands, edits, fetches and MCP tools), acceptEdits (file edits in
//! the working directories go ahead), plan (look, don't touch: only read-only tools, until the
//! plan is approved), bypassPermissions (everything goes ahead; the sandbox is the boundary).
//!
//! Rules, in settings `permissions.{allow,ask,deny}` (deny wins, then ask, then allow):
//!   Bash                   every command            Bash(npm test)   exactly that command
//!   Bash(npm run:*)        commands starting so     Read / Edit      every file
//!   Read(./src/**)         relative to the working directory; /x relative to the project
//!   Edit(//etc/**)         absolute; ~/x from home
//!   WebFetch(domain:docs.rs)                        mcp__server / mcp__server__tool
//! A Read rule covers Glob, Grep and LS too; an Edit rule covers Write, MultiEdit and
//! NotebookEdit. A compound command (&&, ||, ;, |) is allowed only if each part is.
use std::path::{Component, Path, PathBuf};

use serde_json::Value;

#[derive(Clone, Copy, PartialEq, Debug)]
pub enum Mode {
    Default,
    AcceptEdits,
    Plan,
    Bypass,
}

impl Mode {
    pub fn parse(text: &str) -> Result<Mode, String> {
        match text {
            "default" | "ask" => Ok(Mode::Default),
            "acceptEdits" | "accept-edits" => Ok(Mode::AcceptEdits),
            "plan" => Ok(Mode::Plan),
            "bypassPermissions" | "bypass" | "allow" | "dontAsk" => Ok(Mode::Bypass),
            other => Err(format!("unknown permission mode \"{other}\" (default, acceptEdits, plan, bypassPermissions)")),
        }
    }

    pub fn name(self) -> &'static str {
        match self {
            Mode::Default => "default",
            Mode::AcceptEdits => "acceptEdits",
            Mode::Plan => "plan",
            Mode::Bypass => "bypassPermissions",
        }
    }

    /// Shift+Tab: default → accept edits → plan → (bypass, when it was allowed at start) → default.
    pub fn next(self, bypass_available: bool) -> Mode {
        match self {
            Mode::Default => Mode::AcceptEdits,
            Mode::AcceptEdits => Mode::Plan,
            Mode::Plan if bypass_available => Mode::Bypass,
            Mode::Plan | Mode::Bypass => Mode::Default,
        }
    }

    /// The line under the prompt.
    pub fn banner(self) -> Option<&'static str> {
        match self {
            Mode::Default => None,
            Mode::AcceptEdits => Some("⏵⏵ accept edits on"),
            Mode::Plan => Some("⏸ plan mode on"),
            Mode::Bypass => Some("⏵⏵ bypass permissions on"),
        }
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct Rule {
    pub tool: String,
    pub specifier: Option<String>,
    pub text: String,
}

impl Rule {
    pub fn parse(text: &str) -> Option<Rule> {
        let text = text.trim();
        if text.is_empty() {
            return None;
        }
        match text.split_once('(') {
            Some((tool, rest)) if rest.ends_with(')') => {
                let specifier = rest[..rest.len() - 1].trim();
                Some(Rule {
                    tool: tool.trim().to_string(),
                    specifier: (!specifier.is_empty() && specifier != "*").then(|| specifier.to_string()),
                    text: text.to_string(),
                })
            }
            _ => Some(Rule { tool: text.to_string(), specifier: None, text: text.to_string() }),
        }
    }
}

#[derive(Debug, PartialEq)]
pub enum Decision {
    Allow,
    Ask,
    Deny(String),
}

pub const READ_ONLY_TOOLS: [&str; 11] =
    ["Read", "Glob", "Grep", "LS", "TodoWrite", "BashOutput", "KillShell", "Task", "WebSearch", "ExitPlanMode", "Skill"];
pub const EDIT_TOOLS: [&str; 4] = ["Edit", "Write", "MultiEdit", "NotebookEdit"];

pub fn is_read_only(tool: &str) -> bool {
    READ_ONLY_TOOLS.contains(&tool)
}

pub fn is_edit(tool: &str) -> bool {
    EDIT_TOOLS.contains(&tool)
}

#[derive(Clone, Default)]
pub struct Rules {
    pub allow: Vec<Rule>,
    pub ask: Vec<Rule>,
    pub deny: Vec<Rule>,
    /// Where file tools may work without asking: the working directory and additionalDirectories.
    pub directories: Vec<PathBuf>,
    /// What "/x" in a rule is relative to.
    pub project_root: PathBuf,
    pub home: PathBuf,
}

/// Lexically normalized absolute path: `.` and `..` resolved without touching the disk.
pub fn normalize(path: &Path, cwd: &Path) -> PathBuf {
    let joined = if path.is_absolute() { path.to_path_buf() } else { cwd.join(path) };
    let mut out = PathBuf::from("/");
    for part in joined.components() {
        match part {
            Component::ParentDir => {
                out.pop();
            }
            Component::Normal(name) => out.push(name),
            _ => {}
        }
    }
    out
}

/// The parts of a shell command line: split at &&, ||, ;, | and newlines outside quotes.
pub fn split_command(command: &str) -> Vec<String> {
    let mut parts = Vec::new();
    let mut current = String::new();
    let (mut single, mut double, mut escaped) = (false, false, false);
    let chars: Vec<char> = command.chars().collect();
    let mut i = 0;
    while i < chars.len() {
        let c = chars[i];
        if escaped {
            current.push(c);
            escaped = false;
        } else if c == '\\' && !single {
            current.push(c);
            escaped = true;
        } else if c == '\'' && !double {
            single = !single;
            current.push(c);
        } else if c == '"' && !single {
            double = !double;
            current.push(c);
        } else if !single && !double && (c == ';' || c == '\n' || c == '|' || (c == '&' && chars.get(i + 1) == Some(&'&'))) {
            if (c == '|' || c == '&') && chars.get(i + 1) == Some(&c) {
                i += 1;
            }
            if !current.trim().is_empty() {
                parts.push(current.trim().to_string());
            }
            current.clear();
        } else {
            current.push(c);
        }
        i += 1;
    }
    if !current.trim().is_empty() {
        parts.push(current.trim().to_string());
    }
    parts
}

/// Commands that only look: run without asking (each part of a compound command).
pub fn is_safe_command(command: &str) -> bool {
    if command.contains('>') || command.contains("$(") || command.contains('`') || command.contains("<(") {
        return false;
    }
    let parts = split_command(command);
    !parts.is_empty()
        && parts.iter().all(|part| {
            let words: Vec<&str> = part.split_whitespace().collect();
            let Some(first) = words.first() else { return false };
            match *first {
                "ls" | "pwd" | "echo" | "cat" | "head" | "tail" | "wc" | "which" | "whoami" | "date" | "uname" | "file"
                | "stat" | "du" | "df" | "tree" | "grep" | "egrep" | "fgrep" | "rg" | "sort" | "uniq" | "cut" | "tr"
                | "basename" | "dirname" | "realpath" | "readlink" | "true" | "false" | "id" | "hostname" | "nproc"
                | "free" | "uptime" | "printenv" | "type" | "diff" | "cmp" | "md5sum" | "sha256sum" | "less" | "more" => true,
                "find" => !words.iter().any(|w| matches!(*w, "-exec" | "-execdir" | "-delete" | "-ok" | "-fprint")),
                "git" => matches!(
                    words.get(1).copied(),
                    Some("status" | "diff" | "log" | "show" | "branch" | "rev-parse" | "ls-files" | "blame" | "remote" | "describe" | "tag" | "shortlog")
                ) && !words.iter().any(|w| matches!(*w, "-d" | "-D" | "--delete" | "add" | "rm" | "set-url")),
                "sed" => !words.iter().any(|w| w.starts_with("-i")),
                _ => false,
            }
        })
}

fn glob_matches(pattern: &str, path: &Path) -> bool {
    let text = path.to_string_lossy();
    let options = glob::MatchOptions { case_sensitive: true, require_literal_separator: true, require_literal_leading_dot: false };
    if let Ok(compiled) = glob::Pattern::new(pattern) {
        if compiled.matches_with(&text, options) {
            return true;
        }
    }
    // A directory (no wildcard) covers everything under it; "dir/**" matches "dir" itself too.
    let base = pattern.trim_end_matches("/**").trim_end_matches('/');
    !base.contains(['*', '?', '[']) && (text == base || text.starts_with(&format!("{base}/")))
}

impl Rules {
    pub fn from_settings(settings: &crate::settings::Settings, cwd: &Path, extra_dirs: &[PathBuf]) -> Rules {
        let parse = |key: &str| settings.strings(key).iter().filter_map(|text| Rule::parse(text)).collect::<Vec<_>>();
        let mut directories = vec![cwd.to_path_buf()];
        for dir in settings.strings("/permissions/additionalDirectories") {
            directories.push(normalize(Path::new(&expand_home(&dir)), cwd));
        }
        directories.extend(extra_dirs.iter().map(|dir| normalize(dir, cwd)));
        Rules {
            allow: parse("/permissions/allow"),
            ask: parse("/permissions/ask"),
            deny: parse("/permissions/deny"),
            directories,
            project_root: settings.project_root.clone(),
            home: crate::settings::home(),
        }
    }

    /// The file a pattern means: //abs, ~/home, /project-relative, else cwd-relative.
    fn resolve_pattern(&self, pattern: &str, cwd: &Path) -> String {
        if let Some(absolute) = pattern.strip_prefix("//") {
            format!("/{absolute}")
        } else if let Some(home) = pattern.strip_prefix("~/") {
            format!("{}/{home}", self.home.display())
        } else if let Some(project) = pattern.strip_prefix('/') {
            format!("{}/{project}", self.project_root.display())
        } else {
            let relative = pattern.strip_prefix("./").unwrap_or(pattern);
            format!("{}/{relative}", cwd.display())
        }
    }

    fn file_of(tool: &str, input: &Value, cwd: &Path) -> Option<PathBuf> {
        let key = match tool {
            "NotebookEdit" => "notebook_path",
            "Glob" | "Grep" | "LS" => "path",
            _ => "file_path",
        };
        let path = match (input.get(key).and_then(Value::as_str), tool) {
            (Some(path), _) => path,
            (None, "Glob" | "Grep" | "LS") => ".",
            (None, _) => return None,
        };
        Some(normalize(Path::new(path), cwd))
    }

    fn matches(&self, rule: &Rule, tool: &str, input: &Value, cwd: &Path, all_parts: bool) -> bool {
        // mcp__server covers the server's tools.
        if rule.tool.starts_with("mcp__") && tool.starts_with("mcp__") {
            let server_rule = rule.tool.trim_end_matches("__*");
            return tool == rule.tool || tool.starts_with(&format!("{server_rule}__"));
        }
        let family = match rule.tool.as_str() {
            "Read" => tool == "Read" || matches!(tool, "Glob" | "Grep" | "LS"),
            "Edit" => is_edit(tool),
            name => name == tool,
        };
        if !family {
            return false;
        }
        let Some(specifier) = &rule.specifier else { return true };
        match tool {
            "Bash" => {
                let command = input.get("command").and_then(Value::as_str).unwrap_or("");
                let matches_part = |part: &String| match specifier.strip_suffix(":*") {
                    Some(prefix) => part == prefix || part.starts_with(&format!("{prefix} ")),
                    None => part == specifier,
                };
                if command.trim() == specifier {
                    return true;
                }
                let parts = split_command(command);
                match all_parts {
                    true => !parts.is_empty() && parts.iter().all(matches_part),
                    false => parts.iter().any(matches_part),
                }
            }
            "WebFetch" => {
                let url = input.get("url").and_then(Value::as_str).unwrap_or("");
                let host = url.split("://").nth(1).unwrap_or("").split(['/', ':', '?']).next().unwrap_or("").to_lowercase();
                match specifier.strip_prefix("domain:") {
                    Some(domain) => match domain.strip_prefix("*.") {
                        Some(suffix) => host.ends_with(&format!(".{suffix}")),
                        None => host == domain.to_lowercase(),
                    },
                    None => url == specifier,
                }
            }
            _ => match Self::file_of(tool, input, cwd) {
                Some(path) => glob_matches(&self.resolve_pattern(specifier, cwd), &path),
                None => false,
            },
        }
    }

    fn find<'a>(&self, rules: &'a [Rule], tool: &str, input: &Value, cwd: &Path, all_parts: bool) -> Option<&'a Rule> {
        rules.iter().find(|rule| self.matches(rule, tool, input, cwd, all_parts))
    }

    fn inside_directories(&self, path: &Path) -> bool {
        self.directories.iter().any(|dir| path.starts_with(dir))
    }

    pub fn check(&self, mode: Mode, tool: &str, input: &Value, cwd: &Path) -> Decision {
        if let Some(rule) = self.find(&self.deny, tool, input, cwd, false) {
            return Decision::Deny(format!("Permission to use {tool} has been denied by the rule {}.", rule.text));
        }
        let command = input.get("command").and_then(Value::as_str).unwrap_or("");
        if mode == Mode::Plan && !is_read_only(tool) && !(tool == "Bash" && is_safe_command(command)) {
            return Decision::Deny(
                "Plan mode is on: do not make changes or run commands that change anything yet. Research with read-only tools, then present your plan with ExitPlanMode.".into(),
            );
        }
        if mode == Mode::Bypass {
            return Decision::Allow;
        }
        if self.find(&self.ask, tool, input, cwd, false).is_some() {
            return Decision::Ask;
        }
        if self.find(&self.allow, tool, input, cwd, true).is_some() {
            return Decision::Allow;
        }
        let file = Self::file_of(tool, input, cwd);
        if is_read_only(tool) || matches!(tool, "Read" | "Glob" | "Grep" | "LS") {
            return match file {
                Some(path) if !self.inside_directories(&path) => Decision::Ask,
                _ => Decision::Allow,
            };
        }
        if is_edit(tool) && mode == Mode::AcceptEdits && file.as_deref().is_some_and(|path| self.inside_directories(path)) {
            return Decision::Allow;
        }
        if tool == "Bash" && is_safe_command(command) {
            return Decision::Allow;
        }
        Decision::Ask
    }

    /// The rule "don't ask again" adds for this call.
    pub fn suggestion(tool: &str, input: &Value) -> String {
        match tool {
            "Bash" => {
                let command = input.get("command").and_then(Value::as_str).unwrap_or("").trim();
                let first = split_command(command).into_iter().next().unwrap_or_default();
                let words: Vec<&str> = first.split_whitespace().collect();
                match words.as_slice() {
                    [] => "Bash".into(),
                    [one] => format!("Bash({one}:*)"),
                    [one, two, ..] if !two.starts_with('-') && !two.contains(['/', '.', '"', '\'']) => format!("Bash({one} {two}:*)"),
                    [one, ..] => format!("Bash({one}:*)"),
                }
            }
            "WebFetch" => {
                let url = input.get("url").and_then(Value::as_str).unwrap_or("");
                let host = url.split("://").nth(1).unwrap_or("").split(['/', ':', '?']).next().unwrap_or("");
                format!("WebFetch(domain:{host})")
            }
            other => other.to_string(),
        }
    }
}

/// Does one rule (as settings write it) match this call? For hooks' "if".
pub fn rule_matches(rule: &str, tool: &str, input: &Value, cwd: &Path) -> bool {
    let Some(rule) = Rule::parse(rule) else { return false };
    let rules = Rules { directories: vec![cwd.to_path_buf()], project_root: cwd.to_path_buf(), home: crate::settings::home(), ..Default::default() };
    rules.matches(&rule, tool, input, cwd, false)
}

pub fn expand_home(path: &str) -> String {
    match path.strip_prefix("~/") {
        Some(rest) => format!("{}/{rest}", crate::settings::home().display()),
        None => path.to_string(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn rules(allow: &[&str], ask: &[&str], deny: &[&str]) -> Rules {
        Rules {
            allow: allow.iter().filter_map(|r| Rule::parse(r)).collect(),
            ask: ask.iter().filter_map(|r| Rule::parse(r)).collect(),
            deny: deny.iter().filter_map(|r| Rule::parse(r)).collect(),
            directories: vec![PathBuf::from("/work")],
            project_root: PathBuf::from("/work"),
            home: PathBuf::from("/root"),
        }
    }

    fn bash(command: &str) -> Value {
        json!({"command": command})
    }

    #[test]
    fn splitting() {
        assert_eq!(split_command("a && b || c; d | e"), ["a", "b", "c", "d", "e"]);
        assert_eq!(split_command("echo 'a && b' && ls"), ["echo 'a && b'", "ls"]);
        assert_eq!(split_command("sleep 1 & echo x"), ["sleep 1 & echo x"]);
    }

    #[test]
    fn bash_rules() {
        let cwd = Path::new("/work");
        let r = rules(&["Bash(npm run:*)", "Bash(make test)"], &[], &["Bash(rm:*)"]);
        assert_eq!(r.check(Mode::Default, "Bash", &bash("npm run build"), cwd), Decision::Allow);
        assert_eq!(r.check(Mode::Default, "Bash", &bash("npm run"), cwd), Decision::Allow);
        assert_eq!(r.check(Mode::Default, "Bash", &bash("npm install"), cwd), Decision::Ask);
        assert_eq!(r.check(Mode::Default, "Bash", &bash("make test"), cwd), Decision::Allow);
        assert_eq!(r.check(Mode::Default, "Bash", &bash("make test && curl x"), cwd), Decision::Ask);
        assert_eq!(r.check(Mode::Default, "Bash", &bash("npm run a && npm run b"), cwd), Decision::Allow);
        assert!(matches!(r.check(Mode::Default, "Bash", &bash("ls && rm -rf /"), cwd), Decision::Deny(_)));
        assert!(matches!(r.check(Mode::Bypass, "Bash", &bash("rm x"), cwd), Decision::Deny(_)));
        assert_eq!(r.check(Mode::Bypass, "Bash", &bash("curl x"), cwd), Decision::Allow);
        assert_eq!(r.check(Mode::Default, "Bash", &bash("ls -la | grep x"), cwd), Decision::Allow);
        assert_eq!(r.check(Mode::Default, "Bash", &bash("ls > out"), cwd), Decision::Ask);
        assert_eq!(r.check(Mode::Default, "Bash", &bash("git status"), cwd), Decision::Allow);
        assert_eq!(r.check(Mode::Default, "Bash", &bash("git push"), cwd), Decision::Ask);
    }

    #[test]
    fn file_rules_and_modes() {
        let cwd = Path::new("/work");
        let r = rules(&["Edit(./docs/**)"], &[], &["Read(//etc/shadow)", "Edit(/secrets/**)"]);
        let file = |path: &str| json!({"file_path": path});
        assert_eq!(r.check(Mode::Default, "Read", &file("src/a.rs"), cwd), Decision::Allow);
        assert_eq!(r.check(Mode::Default, "Read", &file("/opt/x"), cwd), Decision::Ask);
        assert!(matches!(r.check(Mode::Default, "Read", &file("/etc/shadow"), cwd), Decision::Deny(_)));
        assert_eq!(r.check(Mode::Default, "Edit", &file("src/a.rs"), cwd), Decision::Ask);
        assert_eq!(r.check(Mode::Default, "Write", &file("docs/x/y.md"), cwd), Decision::Allow);
        assert_eq!(r.check(Mode::AcceptEdits, "Edit", &file("src/a.rs"), cwd), Decision::Allow);
        assert_eq!(r.check(Mode::AcceptEdits, "Edit", &file("/tmp/a"), cwd), Decision::Ask);
        assert!(matches!(r.check(Mode::AcceptEdits, "Edit", &file("secrets/k"), cwd), Decision::Deny(_)));
        assert!(matches!(r.check(Mode::Plan, "Edit", &file("src/a.rs"), cwd), Decision::Deny(_)));
        assert_eq!(r.check(Mode::Plan, "Read", &file("src/a.rs"), cwd), Decision::Allow);
        assert_eq!(r.check(Mode::Plan, "Bash", &bash("ls"), cwd), Decision::Allow);
        assert!(matches!(r.check(Mode::Plan, "Bash", &bash("touch x"), cwd), Decision::Deny(_)));
        assert_eq!(r.check(Mode::Default, "Read", &file("../work/../work/a"), cwd), Decision::Allow);
    }

    #[test]
    fn web_and_mcp_rules() {
        let cwd = Path::new("/work");
        let r = rules(&["WebFetch(domain:docs.rs)", "mcp__github"], &["mcp__github__delete_repo"], &[]);
        assert_eq!(r.check(Mode::Default, "WebFetch", &json!({"url": "https://docs.rs/x"}), cwd), Decision::Allow);
        assert_eq!(r.check(Mode::Default, "WebFetch", &json!({"url": "https://evil.rs/x"}), cwd), Decision::Ask);
        assert_eq!(r.check(Mode::Default, "mcp__github__list_issues", &json!({}), cwd), Decision::Allow);
        assert_eq!(r.check(Mode::Default, "mcp__github__delete_repo", &json!({}), cwd), Decision::Ask);
        assert_eq!(r.check(Mode::Default, "mcp__other__x", &json!({}), cwd), Decision::Ask);
    }

    #[test]
    fn suggestions() {
        assert_eq!(Rules::suggestion("Bash", &bash("npm run test -- --watch")), "Bash(npm run:*)");
        assert_eq!(Rules::suggestion("Bash", &bash("pytest -x tests/")), "Bash(pytest:*)");
        assert_eq!(Rules::suggestion("WebFetch", &json!({"url": "https://a.b.com/x"})), "WebFetch(domain:a.b.com)");
    }

    #[test]
    fn mode_cycle() {
        assert_eq!(Mode::Default.next(false), Mode::AcceptEdits);
        assert_eq!(Mode::AcceptEdits.next(false), Mode::Plan);
        assert_eq!(Mode::Plan.next(false), Mode::Default);
        assert_eq!(Mode::Plan.next(true), Mode::Bypass);
    }
}
