//! What a project or a person adds in Markdown files, as in Claude Code:
//!
//!   sub-agents      .claude/agents/*.md, ~/.claude/agents/*.md
//!                   front matter: name, description, tools (comma list; all when absent), model
//!                   (sonnet, opus, haiku, inherit or a model id); the body is its system prompt
//!   slash commands  .claude/commands/**/*.md, ~/.claude/commands/**/*.md → /name (a subfolder
//!                   makes /folder:name); front matter: description, argument-hint,
//!                   allowed-tools, model. The body is the prompt: $ARGUMENTS, $1…$9, !`command`
//!                   (replaced by its output), @file (the file is attached)
//!   output styles   .claude/output-styles/*.md, ~/.claude/output-styles/*.md; front matter:
//!                   name, description; the body replaces the default style's instructions
//! Project files win over the person's with the same name.
use std::path::{Path, PathBuf};

use crate::util::{front_matter, split_list};

#[derive(Clone, Debug)]
pub struct Agent {
    pub name: String,
    pub description: String,
    /// None: every tool but Task.
    pub tools: Option<Vec<String>>,
    pub model: Option<String>,
    pub prompt: String,
    pub source: String,
}

#[derive(Clone, Debug)]
pub struct CustomCommand {
    pub name: String,
    pub description: String,
    pub argument_hint: String,
    pub allowed_tools: Vec<String>,
    pub model: Option<String>,
    pub body: String,
}

/// A skill: .claude/skills/<name>/SKILL.md (or ~/.claude/skills/…), with its folder of
/// supporting files. The model loads it with the Skill tool; the user with /<name>.
#[derive(Clone, Debug)]
pub struct Skill {
    pub name: String,
    pub description: String,
    pub dir: std::path::PathBuf,
    pub body: String,
    pub allowed_tools: Vec<String>,
    pub source: String,
}

#[derive(Clone, Debug)]
pub struct OutputStyle {
    pub name: String,
    pub description: String,
    pub prompt: String,
}

fn markdown_files(dir: &Path, recursive: bool) -> Vec<(PathBuf, String)> {
    let mut found = Vec::new();
    let Ok(entries) = std::fs::read_dir(dir) else { return found };
    let mut entries: Vec<PathBuf> = entries.flatten().map(|e| e.path()).collect();
    entries.sort();
    for path in entries {
        if path.is_dir() && recursive {
            found.extend(markdown_files(&path, true));
        } else if path.extension().is_some_and(|e| e == "md") {
            if let Ok(text) = std::fs::read_to_string(&path) {
                found.push((path, text));
            }
        }
    }
    found
}

fn sources(project_root: &Path, sub: &str) -> Vec<(PathBuf, &'static str)> {
    vec![(crate::settings::user_dir().join(sub), "user"), (project_root.join(".claude").join(sub), "project")]
}

const EXPLORE_PROMPT: &str = "You are a file search specialist. You find files and code quickly and report what you found, precisely.\n\n\
Search with Glob, Grep and Read; use Bash only for read-only commands (ls, git log, git diff, find, cat). Never create, change or \
delete files. Go as deep as the task asks: a quick look for simple questions, several naming conventions and locations for thorough \
ones. Report absolute paths and line numbers, and quote the code that matters. Your final message is the report.";

const GENERAL_PROMPT: &str = "You are an agent for Claude Code, working on the user's codebase. Given the task, use the tools \
available to complete it fully: search, read, run and change what the task needs, and nothing more. Do not create files unless the \
task needs them. When you are done, reply with a concise report of what you found or did: it goes to the agent that gave you the \
task, which relays it. Include file paths (absolute) and relevant snippets.";

pub fn builtin_agents() -> Vec<Agent> {
    vec![
        Agent {
            name: "general-purpose".into(),
            description: "General-purpose agent for researching complex questions, searching for code, and executing multi-step tasks. When you are searching for a keyword or file and are not confident that you will find the right match in the first few tries use this agent to perform the search for you.".into(),
            tools: None,
            model: None,
            prompt: GENERAL_PROMPT.into(),
            source: "built-in".into(),
        },
        Agent {
            name: "Explore".into(),
            description: "Fast agent specialized for exploring codebases: finding files by patterns, searching code for keywords, or answering questions about the codebase. Say how thorough it should be: \"quick\", \"medium\" or \"very thorough\".".into(),
            tools: Some(["Glob", "Grep", "Read", "LS", "Bash", "WebFetch"].iter().map(|s| s.to_string()).collect()),
            model: None,
            prompt: EXPLORE_PROMPT.into(),
            source: "built-in".into(),
        },
    ]
}

/// The folders a kind of file comes from: the enabled plugins', then the person's, then the
/// project's (later ones win on a name).
fn all_sources(project_root: &Path, sub: &str) -> Vec<(PathBuf, String)> {
    let mut all: Vec<(PathBuf, String)> = crate::plugins::loaded().into_iter().map(|p| (p.root.join(sub), format!("plugin:{}", p.name))).collect();
    all.extend(sources(project_root, sub).into_iter().map(|(dir, source)| (dir, source.to_string())));
    all
}

pub fn agents(project_root: &Path) -> Vec<Agent> {
    let mut agents = builtin_agents();
    for (dir, source) in all_sources(project_root, "agents") {
        for (path, text) in markdown_files(&dir, false) {
            let (fields, body) = front_matter(&text);
            let name = fields.get("name").cloned().unwrap_or_else(|| path.file_stem().unwrap_or_default().to_string_lossy().into_owned());
            let agent = Agent {
                description: fields.get("description").cloned().unwrap_or_default(),
                tools: fields.get("tools").map(|list| split_list(list)).filter(|list| !list.is_empty()),
                model: fields.get("model").cloned().filter(|m| !m.is_empty() && m != "inherit"),
                prompt: body.trim().to_string(),
                source: source.clone(),
                name: name.clone(),
            };
            agents.retain(|existing| existing.name != name);
            agents.push(agent);
        }
    }
    agents
}

pub fn commands(project_root: &Path) -> Vec<CustomCommand> {
    let mut commands: Vec<CustomCommand> = Vec::new();
    for (dir, source) in all_sources(project_root, "commands") {
        for (path, text) in markdown_files(&dir, true) {
            let relative = path.strip_prefix(&dir).unwrap_or(&path).with_extension("");
            let mut name = relative.components().map(|c| c.as_os_str().to_string_lossy().into_owned()).collect::<Vec<_>>().join(":");
            // A plugin's commands are /plugin:command.
            if let Some(plugin) = source.strip_prefix("plugin:") {
                name = format!("{plugin}:{name}");
            }
            let (fields, body) = front_matter(&text);
            let description = fields
                .get("description")
                .cloned()
                .unwrap_or_else(|| body.lines().find(|l| !l.trim().is_empty()).unwrap_or("").trim_start_matches('#').trim().chars().take(80).collect());
            commands.retain(|existing| existing.name != name);
            commands.push(CustomCommand {
                name,
                description: format!("{description} ({source})"),
                argument_hint: fields.get("argument-hint").cloned().unwrap_or_default(),
                allowed_tools: fields.get("allowed-tools").map(|list| split_list(list)).unwrap_or_default(),
                model: fields.get("model").cloned(),
                body,
            });
        }
    }
    commands
}

/// The command's prompt with its arguments in place and its !`commands` run.
pub fn expand_command(command: &CustomCommand, arguments: &str, cwd: &Path) -> String {
    let words: Vec<&str> = arguments.split_whitespace().collect();
    let mut text = command.body.replace("$ARGUMENTS", arguments);
    for n in (1..=9).rev() {
        text = text.replace(&format!("${n}"), words.get(n - 1).copied().unwrap_or(""));
    }
    let bang = regex::Regex::new(r"!`([^`]+)`").unwrap();
    bang.replace_all(&text, |caps: &regex::Captures| {
        let output = std::process::Command::new("/bin/sh")
            .arg("-c")
            .arg(format!("exec 2>&1\n{}", &caps[1]))
            .current_dir(cwd)
            .stdin(std::process::Stdio::null())
            .output();
        match output {
            Ok(output) => String::from_utf8_lossy(&output.stdout).trim_end().to_string(),
            Err(error) => format!("(could not run {}: {error})", &caps[1]),
        }
    })
    .into_owned()
}

pub fn skills(project_root: &Path) -> Vec<Skill> {
    let mut skills: Vec<Skill> = Vec::new();
    for (dir, source) in all_sources(project_root, "skills") {
        let Ok(entries) = std::fs::read_dir(&dir) else { continue };
        let mut folders: Vec<PathBuf> = entries.flatten().map(|e| e.path()).filter(|p| p.join("SKILL.md").is_file()).collect();
        folders.sort();
        for folder in folders {
            let Ok(text) = std::fs::read_to_string(folder.join("SKILL.md")) else { continue };
            let (fields, body) = front_matter(&text);
            let name = fields.get("name").cloned().unwrap_or_else(|| folder.file_name().unwrap_or_default().to_string_lossy().into_owned());
            skills.retain(|s| s.name != name);
            skills.push(Skill {
                description: fields.get("description").cloned().unwrap_or_default(),
                allowed_tools: fields.get("allowed-tools").map(|list| split_list(list)).unwrap_or_default(),
                body: body.trim().to_string(),
                dir: folder,
                source: source.clone(),
                name,
            });
        }
    }
    skills
}

pub fn builtin_styles() -> Vec<OutputStyle> {
    vec![
        OutputStyle { name: "default".into(), description: "Claude completes coding tasks efficiently and provides concise responses".into(), prompt: String::new() },
        OutputStyle {
            name: "Explanatory".into(),
            description: "Claude explains its implementation choices and codebase patterns".into(),
            prompt: "# Output style: Explanatory\nAlong with doing the task, teach: before and after writing code, give brief educational insights about the implementation choices and the codebase's patterns, in a block like:\n\"★ Insight ─────────────────────────────────────\n[2-3 key points]\n─────────────────────────────────────────────────\"\nKeep insights specific to this codebase and this code, not general programming advice.".into(),
        },
        OutputStyle {
            name: "Learning".into(),
            description: "Claude pauses and asks you to write small pieces of code for hands-on practice".into(),
            prompt: "# Output style: Learning\nHelp the user learn by doing. Do the scaffolding yourself, but for meaningful pieces (5-10 lines of logic with real design choices) leave a TODO(human) in the code and ask the user to write it: explain the context, what the piece must do, and the trade-offs to weigh. Share brief insights (★ Insight blocks) about the codebase as you go. Continue after the user has written their part.".into(),
        },
    ]
}

pub fn styles(project_root: &Path) -> Vec<OutputStyle> {
    let mut styles = builtin_styles();
    for (dir, _) in sources(project_root, "output-styles") {
        for (path, text) in markdown_files(&dir, false) {
            let (fields, body) = front_matter(&text);
            let name = fields.get("name").cloned().unwrap_or_else(|| path.file_stem().unwrap_or_default().to_string_lossy().into_owned());
            styles.retain(|s| !s.name.eq_ignore_ascii_case(&name));
            styles.push(OutputStyle { description: fields.get("description").cloned().unwrap_or_default(), prompt: body.trim().to_string(), name });
        }
    }
    styles
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn project_extensions() {
        let root = std::env::temp_dir().join(format!("claude-ext-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        std::fs::create_dir_all(root.join(".claude/commands/frontend")).unwrap();
        std::fs::create_dir_all(root.join(".claude/agents")).unwrap();
        std::fs::write(root.join(".claude/commands/frontend/component.md"), "---\ndescription: Make a component\nargument-hint: <name>\n---\nCreate component $1 ($ARGUMENTS) in !`echo src`.\n").unwrap();
        std::fs::write(root.join(".claude/commands/fix.md"), "# Fix the issue\nFix issue $ARGUMENTS.\n").unwrap();
        std::fs::write(root.join(".claude/agents/reviewer.md"), "---\nname: code-reviewer\ndescription: Reviews code\ntools: Read, Grep\nmodel: haiku\n---\nYou review code.\n").unwrap();
        let commands = commands(&root);
        let component = commands.iter().find(|c| c.name == "frontend:component").unwrap();
        assert_eq!(component.argument_hint, "<name>");
        assert_eq!(expand_command(component, "Button big", &root), "Create component Button (Button big) in src.\n");
        assert_eq!(commands.iter().find(|c| c.name == "fix").unwrap().description, "Fix the issue (project)");
        let agents = agents(&root);
        let reviewer = agents.iter().find(|a| a.name == "code-reviewer").unwrap();
        assert_eq!(reviewer.tools.as_deref(), Some(&["Read".to_string(), "Grep".to_string()][..]));
        assert_eq!(reviewer.model.as_deref(), Some("haiku"));
        assert!(agents.iter().any(|a| a.name == "general-purpose"));
    }
}
