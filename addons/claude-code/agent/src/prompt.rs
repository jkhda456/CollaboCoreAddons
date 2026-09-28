//! The system prompt: how to work, what this machine is, and the instruction files.
//!
//! Instruction files (memory), in this order (later ones are more specific):
//!   ~/.claude/CLAUDE.md                 the person's own, for every project
//!   CLAUDE.md in each directory from / down to the working directory (AGENTS.md where a
//!   directory has no CLAUDE.md), then CLAUDE.local.md beside it
//! A line "@path" in one pulls in that file (relative to the file; ~/ for home), five deep at most.
use std::path::{Path, PathBuf};

use crate::permissions::Mode;

const INTRO: &str = "You are Claude Code, an interactive CLI coding agent that helps users with software engineering tasks. \
You run inside collaboCore: a sandboxed Linux machine (a WebAssembly Linux kernel with a busybox userland) that holds only the \
folders the user shared with it. Use the instructions below and the tools available to you to assist the user.

IMPORTANT: Assist with defensive security tasks only. Refuse to create, modify, or improve code that may be used maliciously.
IMPORTANT: Never generate or guess URLs for the user unless you are confident they help with programming.";

const STYLE: &str = "# Tone and style
- Be concise, direct and to the point. Your output is shown in a terminal with Markdown rendering: short answers, no preamble \
or postamble (\"Here is…\", \"I will now…\"), unless the user asks for detail.
- Answer the question asked. After working on a file, stop: do not summarize what you did unless asked.
- Use emojis only if the user asks for them.
- Refer to code as `file_path:line_number` so the user can find it.
- Output text to communicate with the user: never use Bash echo or code comments for that.
- If you cannot or will not help with something, say so briefly, without lecturing.";

const WORK: &str = "# Doing tasks
- Understand first: search and read the relevant code before changing it. Follow the codebase's conventions: its libraries, \
naming, formatting and comment density. Never assume a library is available; check that the project uses it.
- Plan work with several steps with TodoWrite, and keep the list current as you go (one task in_progress at a time; mark each \
completed as soon as it is done).
- After changing code, verify it: run the tests, the build, the linter or the type checker when the project has them (look in \
the README or the build files for the commands). If you cannot find them, ask the user, and suggest writing them to CLAUDE.md.
- Do what was asked; nothing more, nothing less. Prefer editing existing files to creating new ones. Do not create documentation \
files unless asked.
- Never commit changes unless the user asks you to. Never push unless asked.
- When something fails, read the error and fix its cause; do not paper over it or disable the check. If you are blocked, say so.
- Never introduce code that exposes or logs secrets and keys.";

const TOOLS: &str = "# Using tools
- Use the dedicated tools for files: Read (not cat/head/tail), Edit and MultiEdit (not sed/awk), Write (not echo >/cat <<EOF), \
Glob (not find/ls), Grep (not grep/rg). Reserve Bash for running programs and commands.
- Make independent tool calls in the same answer (several Reads, Globs, Greps or commands at once).
- For open-ended searches that may take many rounds, or work that would fill your context, use the Task tool with a sub-agent.
- Tool results and user messages may include <system-reminder> tags: information from the system, not from the user.
- When the user runs a command themselves (their message shows <bash-input>), its output is in <bash-stdout>/<bash-stderr>.
- A tool call the user refused is not to be retried as it was: ask what they want instead, or do something else.
- Hooks (commands the user configured) may block a call or give feedback: treat their messages as coming from the user.";

fn exists(path: &str) -> bool {
    Path::new(path).exists()
}

fn git_root(cwd: &Path) -> Option<PathBuf> {
    cwd.ancestors().find(|dir| dir.join(".git").exists()).map(Path::to_path_buf)
}

fn environment(cwd: &Path, model: &str, directories: &[PathBuf]) -> String {
    let mut programs = vec!["busybox (sh, ls, grep, sed, awk, find, tar, wget, vi, …)", "hfetch URL (HTTP(S) through the host)"];
    if exists("/usr/bin/python3") || exists("/usr/local/bin/python3") {
        programs.push("python3 (CPython 3.13, pip)");
    }
    for (path, name) in [("/usr/bin/git", "git"), ("/usr/bin/curl", "curl"), ("/usr/bin/ssh", "ssh")] {
        if exists(path) {
            programs.push(name);
        }
    }
    let extra = directories.iter().skip(1).map(|d| d.display().to_string()).collect::<Vec<_>>();
    format!(
        "<env>\nWorking directory: {}\n{}Is directory a git repo: {}\nPlatform: collaboCore sandbox (Linux, wasm32)\nPrograms: {}\n\
         Network: requests go through the host, which may allow only some hosts; the app's API keys are added by the host (there are none \
         in this machine, so do not look for them).\nToday's date: {}\n</env>\nYou are powered by the model {model}.",
        cwd.display(),
        if extra.is_empty() { String::new() } else { format!("Additional working directories: {}\n", extra.join(", ")) },
        git_root(cwd).map_or("No".to_string(), |root| format!("Yes ({})", root.display())),
        programs.join(", "),
        crate::util::today(),
    )
}

/// A memory file with its @imports expanded in place.
fn with_imports(path: &Path, text: &str, depth: usize) -> String {
    if depth >= 5 {
        return text.to_string();
    }
    let mut out = String::new();
    let mut in_code = false;
    for line in text.lines() {
        if line.trim_start().starts_with("```") {
            in_code = !in_code;
        }
        let trimmed = line.trim();
        if let (false, Some(target)) = (in_code, trimmed.strip_prefix('@').filter(|t| !t.is_empty() && !t.contains(' '))) {
            let target = crate::permissions::expand_home(target);
            let full = match Path::new(&target).is_absolute() {
                true => PathBuf::from(&target),
                false => path.parent().unwrap_or(Path::new("/")).join(&target),
            };
            if let Ok(imported) = std::fs::read_to_string(&full) {
                out.push_str(&with_imports(&full, &imported, depth + 1));
                continue;
            }
        }
        out.push_str(line);
        out.push('\n');
    }
    out
}

/// The instruction files that apply to `cwd`, as (path, contents with imports).
pub fn memory_files(cwd: &Path) -> Vec<(PathBuf, String)> {
    let mut files = Vec::new();
    let mut add = |path: PathBuf| {
        if let Ok(text) = std::fs::read_to_string(&path) {
            if !text.trim().is_empty() && !files.iter().any(|(seen, _): &(PathBuf, String)| *seen == path) {
                let text = with_imports(&path, &text, 0);
                files.push((path, text));
            }
        }
    };
    add(crate::settings::user_dir().join("CLAUDE.md"));
    let mut dirs: Vec<&Path> = cwd.ancestors().collect();
    dirs.reverse();
    for dir in dirs {
        let claude = dir.join("CLAUDE.md");
        match claude.is_file() {
            true => add(claude),
            false => add(dir.join("AGENTS.md")),
        }
        add(dir.join(".claude/CLAUDE.md"));
        add(dir.join("CLAUDE.local.md"));
    }
    files
}

pub struct Parts<'a> {
    pub cwd: &'a Path,
    pub model: &'a str,
    pub mode: Mode,
    pub directories: &'a [PathBuf],
    pub agents: &'a [crate::extensions::Agent],
    /// /goal: what the session is working toward.
    pub goal: Option<&'a str>,
    pub style: Option<&'a crate::extensions::OutputStyle>,
    pub mcp_instructions: &'a [(String, String)],
    pub replace: Option<&'a str>,
    pub append: Option<&'a str>,
    /// A sub-agent's own prompt (it replaces the main instructions).
    pub agent_prompt: Option<&'a str>,
}

pub fn system(parts: &Parts) -> String {
    let mut prompt = match (parts.replace, parts.agent_prompt) {
        (Some(replace), _) => replace.to_string(),
        (None, Some(agent)) => format!("{agent}\n\n{TOOLS}"),
        (None, None) => {
            let style = parts.style.filter(|s| !s.prompt.is_empty());
            let mut text = format!("{INTRO}\n\n");
            match style {
                // A custom style replaces the default ways of answering; the task rules stay.
                Some(style) => text.push_str(&format!("{}\n\n", style.prompt)),
                None => text.push_str(&format!("{STYLE}\n\n")),
            }
            text.push_str(&format!("{WORK}\n\n{TOOLS}"));
            text
        }
    };
    if parts.agent_prompt.is_none() && parts.agents.len() > 0 {
        prompt.push_str("\n\n# Sub-agents for the Task tool\n");
        for agent in parts.agents {
            let tools = agent.tools.as_ref().map_or("all tools".to_string(), |t| t.join(", "));
            prompt.push_str(&format!("- {}: {} (Tools: {tools})\n", agent.name, agent.description));
        }
    }
    prompt.push_str(&format!("\n\n{}", environment(parts.cwd, parts.model, parts.directories)));
    if let Some(goal) = parts.goal {
        prompt.push_str(&format!(
            "\n\n# Goal\nThe user set a goal for this session: {goal}\nWork toward it without stopping to ask; when you stop, a check decides whether it holds, and you are sent back to work if it does not."
        ));
    }
    match parts.mode {
        Mode::Plan => prompt.push_str(
            "\n\n<system-reminder>Plan mode is active. The user does not want you to make changes yet: do not edit files, run \
             commands that change anything, or make commits. Research with read-only tools, then present your plan with the \
             ExitPlanMode tool and wait for approval.</system-reminder>",
        ),
        Mode::AcceptEdits => prompt.push_str("\n\n(Permission mode: file edits are accepted without asking.)"),
        _ => {}
    }
    for (server, instructions) in parts.mcp_instructions {
        prompt.push_str(&format!("\n\n# MCP server {server}\n{instructions}"));
    }
    let files = memory_files(parts.cwd);
    if !files.is_empty() {
        prompt.push_str(
            "\n\n# claudeMd\nCodebase and user instructions are shown below. Be sure to adhere to these instructions. IMPORTANT: These \
             instructions OVERRIDE any default behavior and you MUST follow them exactly as written.",
        );
        for (path, text) in files {
            let kind = if path.starts_with(crate::settings::user_dir()) {
                "user's private global instructions for all projects"
            } else if path.ends_with("CLAUDE.local.md") {
                "user's private project instructions, not checked in"
            } else {
                "project instructions, checked into the codebase"
            };
            prompt.push_str(&format!("\n\nContents of {} ({kind}):\n\n{}", path.display(), text.trim_end()));
        }
    }
    if let Some(append) = parts.append {
        prompt.push_str(&format!("\n\n{append}"));
    }
    prompt
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn instruction_files_and_imports() {
        let root = std::env::temp_dir().join(format!("claude-prompt-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        std::fs::create_dir_all(root.join("app/sub")).unwrap();
        std::fs::create_dir_all(root.join("docs")).unwrap();
        std::fs::write(root.join("AGENTS.md"), "agents at root").unwrap();
        std::fs::write(root.join("app/CLAUDE.md"), "claude in app\n@../docs/style.md\n```\n@not-an-import\n```").unwrap();
        std::fs::write(root.join("docs/style.md"), "imported style").unwrap();
        std::fs::write(root.join("app/AGENTS.md"), "ignored: app has CLAUDE.md").unwrap();
        std::fs::write(root.join("app/sub/CLAUDE.local.md"), "local").unwrap();
        let found: Vec<String> = memory_files(&root.join("app/sub")).into_iter().map(|(_, text)| text).collect();
        assert!(found.ends_with(&[
            "agents at root\n".to_string(),
            "claude in app\nimported style\n```\n@not-an-import\n```\n".to_string(),
            "local\n".to_string()
        ]), "{found:?}");
    }
}
