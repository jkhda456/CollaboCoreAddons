//! The tools the model can call, with Claude Code's names and arguments. Paths are the guest's;
//! relative ones are taken from the session's working directory, which `cd` in Bash changes.
//!
//! Task (sub-agents), ExitPlanMode and MCP tools need the session and are run by agent.rs;
//! WebSearch is the Messages API's server tool (Anthropic runs it).
pub mod bash;
pub mod files;
pub mod search;
pub mod todo;
pub mod web;

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::time::SystemTime;

use serde_json::{json, Value};

pub const MAX_OUTPUT: usize = 30_000;

/// What the UI can draw for a result beyond its text.
#[derive(Clone, Debug)]
pub enum Display {
    Diff { path: PathBuf, before: String, after: String },
    Todos(Vec<todo::Todo>),
    /// One line under the result (a sub-agent's totals).
    Note(String),
}

#[derive(Default)]
pub struct Outcome {
    pub text: String,
    pub is_error: bool,
    /// Richer content for the model than text (an image read from a file).
    pub content: Option<Value>,
    pub display: Option<Display>,
}

pub fn ok(text: impl Into<String>) -> Outcome {
    Outcome { text: text.into(), ..Default::default() }
}

pub fn fail(text: impl Into<String>) -> Outcome {
    Outcome { text: text.into(), is_error: true, ..Default::default() }
}

/// What the tools share across calls in one session.
pub struct Context {
    pub cwd: PathBuf,
    /// Files read (or written) in this session, with their modification time then: an existing
    /// file is edited only after it has been read, and not if it changed since.
    pub read_files: HashMap<PathBuf, SystemTime>,
    pub shells: bash::Shells,
    pub todos: Vec<todo::Todo>,
    /// Called while a foreground command runs, with its output file (the UI shows its tail).
    pub progress: Option<Box<dyn Fn(&Path)>>,
}

impl Context {
    pub fn new(cwd: PathBuf) -> Context {
        Context { cwd, read_files: HashMap::new(), shells: bash::Shells::default(), todos: Vec::new(), progress: None }
    }

    pub fn resolve(&self, path: &str) -> PathBuf {
        crate::permissions::normalize(Path::new(&crate::permissions::expand_home(path)), &self.cwd)
    }
}

/// Keeps the start and the end of long output.
pub fn shorten(text: &str, limit: usize) -> String {
    if text.len() <= limit {
        return text.to_string();
    }
    let mut head = limit / 2;
    while !text.is_char_boundary(head) {
        head -= 1;
    }
    let mut tail = text.len() - limit / 2;
    while !text.is_char_boundary(tail) {
        tail += 1;
    }
    let dropped = text[head..tail].lines().count();
    format!("{}\n\n… [{dropped} lines cut] …\n\n{}", &text[..head], &text[tail..])
}

fn schema(properties: Value, required: &[&str]) -> Value {
    json!({"type": "object", "properties": properties, "required": required, "additionalProperties": false})
}

/// Every built-in tool, in the Messages API's shape. `web_search`: include the server tool.
pub fn specs() -> Vec<Value> {
    vec![
        json!({"name": "Task", "description": "Launch a sub-agent to handle a complex, multi-step task on its own: it gets the prompt, works with its own tools and its own context, and returns one final report (which only you see; relay what matters). Use it for open-ended searches across a codebase, or work that would fill your context with file contents. Give it a complete, self-contained prompt: it does not see this conversation. Several Task calls in one answer run one after another. Agent types are listed in the system prompt (general-purpose if unsure).",
            "input_schema": schema(json!({
                "description": {"type": "string", "description": "A short (3-5 word) description of the task"},
                "prompt": {"type": "string", "description": "The task for the agent to perform"},
                "subagent_type": {"type": "string", "description": "The type of agent to use"}
            }), &["description", "prompt", "subagent_type"])}),
        json!({"name": "Bash", "description": "Run a command with /bin/sh (busybox ash) in the sandbox and return its output (stdout and stderr together). The working directory persists between calls: `cd` changes it for later commands; environment variables do not persist.\n\n- Use it for running programs, tests, builds, git and package managers, and for mv, cp, mkdir and similar.\n- Prefer Read, Edit, Write, Glob and Grep over cat, sed, echo >, find and grep for files.\n- Quote paths with spaces. Chain dependent commands with &&; send independent ones as separate calls in one answer.\n- Output over 30000 characters is shortened in the middle. Commands time out after 2 minutes unless timeout (ms, at most 600000) says otherwise.\n- run_in_background: true starts a long-running command (a server, a watcher, a long build) and returns its id at once; read its output with BashOutput and stop it with KillShell. Do not use `&` for that.\n- Do not use interactive commands (vi, less, git rebase -i, anything waiting for input).",
            "input_schema": schema(json!({
                "command": {"type": "string", "description": "The command to execute"},
                "timeout": {"type": "number", "description": "Optional timeout in milliseconds (max 600000)"},
                "description": {"type": "string", "description": "Clear, concise description of what this command does in 5-10 words"},
                "run_in_background": {"type": "boolean", "description": "Run the command in the background and return its id at once"}
            }), &["command"])}),
        json!({"name": "BashOutput", "description": "Read the output a background shell (started by Bash with run_in_background) has written since the last read, and whether it is still running.",
            "input_schema": schema(json!({
                "bash_id": {"type": "string", "description": "The id of the background shell"},
                "filter": {"type": "string", "description": "Optional regular expression: only lines that match"}
            }), &["bash_id"])}),
        json!({"name": "KillShell", "description": "Stop a background shell by its id.",
            "input_schema": schema(json!({"shell_id": {"type": "string", "description": "The id of the background shell to kill"}}), &["shell_id"])}),
        json!({"name": "Glob", "description": "Find files by name pattern, e.g. \"**/*.rs\" or \"src/**/test_*.py\". Returns matching paths, most recently modified first (at most 100). .git and node_modules are skipped.",
            "input_schema": schema(json!({
                "pattern": {"type": "string", "description": "The glob pattern to match files against"},
                "path": {"type": "string", "description": "The directory to search in (default: the working directory)"}
            }), &["pattern"])}),
        json!({"name": "Grep", "description": "Search file contents with a regular expression (Rust regex syntax), recursively; .git and node_modules are skipped, binary files too.\n- output_mode: \"files_with_matches\" (default: the paths), \"content\" (matching lines; -n line numbers, -A/-B/-C context) or \"count\".\n- glob filters files (\"*.js\", \"**/*.{ts,tsx}\"); type filters by language (js, py, rust, go, java, c, cpp, ts, md, …).\n- multiline: true lets . match newlines and patterns span lines.\n- Literal braces need escaping: interface\\{\\}.",
            "input_schema": schema(json!({
                "pattern": {"type": "string", "description": "The regular expression to search for"},
                "path": {"type": "string", "description": "File or directory to search in (default: the working directory)"},
                "glob": {"type": "string", "description": "Glob pattern to filter files"},
                "type": {"type": "string", "description": "File type to search"},
                "output_mode": {"type": "string", "enum": ["content", "files_with_matches", "count"]},
                "-i": {"type": "boolean", "description": "Case insensitive search"},
                "-n": {"type": "boolean", "description": "Show line numbers (content mode; default true)"},
                "-A": {"type": "number", "description": "Lines to show after each match (content mode)"},
                "-B": {"type": "number", "description": "Lines to show before each match (content mode)"},
                "-C": {"type": "number", "description": "Lines to show before and after each match (content mode)"},
                "multiline": {"type": "boolean", "description": "Patterns can span lines; . matches newlines"},
                "head_limit": {"type": "number", "description": "Only the first N results"}
            }), &["pattern"])}),
        json!({"name": "LS", "description": "List a directory: its files and subdirectories (sizes of files), non-recursively. Prefer Glob and Grep when you know what you are looking for.",
            "input_schema": schema(json!({
                "path": {"type": "string", "description": "The directory to list (default: the working directory)"},
                "ignore": {"type": "array", "items": {"type": "string"}, "description": "Glob patterns of names to leave out"}
            }), &[])}),
        json!({"name": "Read", "description": "Read a file. Text comes back with its lines numbered from 1 (cat -n style): up to 2000 lines from the start unless offset (first line, 1-based) and limit say otherwise; lines over 2000 characters are cut. Images (png, jpg, gif, webp) come back as images. Jupyter notebooks (.ipynb) come back as their cells with outputs. Read a file before you edit it. Several files can be read in one answer.",
            "input_schema": schema(json!({
                "file_path": {"type": "string", "description": "The path of the file to read (absolute, or relative to the working directory)"},
                "offset": {"type": "number", "description": "The line number to start reading from"},
                "limit": {"type": "number", "description": "The number of lines to read"}
            }), &["file_path"])}),
        json!({"name": "Edit", "description": "Replace text in a file exactly.\n- Read the file first in this conversation; an edit of an unread (or since changed) file is refused.\n- old_string must match the file character for character, indentation included (not the line-number prefix Read shows), and be unique in it unless replace_all is true; include surrounding lines to make it unique.\n- replace_all: true replaces every occurrence (renaming a variable, say).",
            "input_schema": schema(json!({
                "file_path": {"type": "string", "description": "The path of the file to modify"},
                "old_string": {"type": "string", "description": "The text to replace"},
                "new_string": {"type": "string", "description": "The text to replace it with (must differ)"},
                "replace_all": {"type": "boolean", "description": "Replace all occurrences (default false)"}
            }), &["file_path", "old_string", "new_string"])}),
        json!({"name": "MultiEdit", "description": "Several Edit-style replacements in one file, applied in order, all or none: each edit works on the result of the ones before it. Same rules as Edit for each (exact match, unique unless replace_all). Prefer it to several Edit calls on one file.",
            "input_schema": schema(json!({
                "file_path": {"type": "string", "description": "The path of the file to modify"},
                "edits": {"type": "array", "minItems": 1, "items": {"type": "object", "properties": {
                    "old_string": {"type": "string"}, "new_string": {"type": "string"}, "replace_all": {"type": "boolean"}
                }, "required": ["old_string", "new_string"]}}
            }), &["file_path", "edits"])}),
        json!({"name": "Write", "description": "Write a file, replacing it if it exists (parent directories are created). An existing file must have been Read first. Prefer Edit for changes to an existing file. Do not create documentation files (*.md, README) unless asked.",
            "input_schema": schema(json!({
                "file_path": {"type": "string", "description": "The path of the file to write"},
                "content": {"type": "string", "description": "The content to write to the file"}
            }), &["file_path", "content"])}),
        json!({"name": "NotebookEdit", "description": "Change a Jupyter notebook (.ipynb) cell: replace its source (edit_mode replace, the default), insert a new cell after the one named (insert; at the start without cell_id), or delete it (delete). Cells are named by their id, or \"cell-N\" for the N-th (0-based) when they have none.",
            "input_schema": schema(json!({
                "notebook_path": {"type": "string", "description": "The path of the notebook"},
                "cell_id": {"type": "string", "description": "The cell to change, or after which to insert"},
                "new_source": {"type": "string", "description": "The new source of the cell"},
                "cell_type": {"type": "string", "enum": ["code", "markdown"], "description": "Required for insert"},
                "edit_mode": {"type": "string", "enum": ["replace", "insert", "delete"]}
            }), &["notebook_path", "new_source"])}),
        json!({"name": "WebFetch", "description": "Fetch a URL (through the host, under the sandbox's network policy) and return its content as text (HTML reduced to readable text), at most 100000 characters. prompt says what to look for in it: the page is returned with it, for you to answer from. http:// URLs are fetched as given.",
            "input_schema": schema(json!({
                "url": {"type": "string", "description": "The URL to fetch content from"},
                "prompt": {"type": "string", "description": "What to find in the page"}
            }), &["url"])}),
        json!({"name": "TodoWrite", "description": "Keep a structured task list for the current session: pass the whole list each time. Use it for work with three or more steps, or when the user gives several tasks: it shows the user your progress. Exactly one task is in_progress while you work; mark each completed as soon as it is done (not in batches), and add follow-up tasks you discover. Skip it for single, trivial requests. content says what to do (\"Run the tests\"); activeForm says it while it happens (\"Running the tests\").",
            "input_schema": schema(json!({
                "todos": {"type": "array", "items": {"type": "object", "properties": {
                    "content": {"type": "string", "minLength": 1},
                    "status": {"type": "string", "enum": ["pending", "in_progress", "completed"]},
                    "activeForm": {"type": "string", "minLength": 1}
                }, "required": ["content", "status", "activeForm"]}}
            }), &["todos"])}),
        json!({"name": "ExitPlanMode", "description": "In plan mode, when your plan for a task that changes code is ready: present it (Markdown, concise) for the user to approve. Only after approval do you start making changes. Not for research-only tasks.",
            "input_schema": schema(json!({"plan": {"type": "string", "description": "The plan to approve"}}), &["plan"])}),
    ]
}

/// The Skill tool, when there are skills.
pub fn skill_spec(skills: &[crate::extensions::Skill]) -> Value {
    let list = skills.iter().map(|s| format!("- {}: {}", s.name, s.description)).collect::<Vec<_>>().join("\n");
    json!({"name": "Skill", "description": format!("Load a skill: instructions (and a folder of scripts and references) for a kind of task. When a request matches a skill's description, call this first and follow what it says. Available skills:\n{list}"),
        "input_schema": schema(json!({"skill": {"type": "string", "description": "The skill's name"}}), &["skill"])})
}

/// The Messages API's web search, run by Anthropic (not by this program).
pub fn web_search_tool() -> Value {
    json!({"type": "web_search_20250305", "name": "web_search", "max_uses": 5})
}

/// Runs a built-in tool that needs nothing but the context.
pub fn run(name: &str, input: &Value, context: &mut Context) -> Outcome {
    if let Some(raw) = input.get("__invalid_arguments") {
        return fail(format!("InputValidationError: the arguments were not valid JSON: {raw}"));
    }
    match name {
        "Bash" => bash::bash(input, context),
        "BashOutput" => bash::bash_output(input, context),
        "KillShell" => bash::kill_shell(input, context),
        "Read" => files::read(input, context),
        "Write" => files::write(input, context),
        "Edit" => files::edit(input, context),
        "MultiEdit" => files::multi_edit(input, context),
        "NotebookEdit" => files::notebook_edit(input, context),
        "LS" => files::ls(input, context),
        "Glob" => search::glob_tool(input, context),
        "Grep" => search::grep(input, context),
        "WebFetch" => web::fetch(input),
        "TodoWrite" => todo::write(input, context),
        other => fail(format!("No such tool available: {other}")),
    }
}

/// One line for the terminal: what the call is about, e.g. Bash(npm test).
pub fn summary(name: &str, input: &Value, cwd: &Path) -> String {
    let field = |key: &str| input.get(key).and_then(Value::as_str).unwrap_or("").to_string();
    let path = |key: &str| {
        let full = field(key);
        match Path::new(&full).strip_prefix(cwd) {
            Ok(relative) if !relative.as_os_str().is_empty() => relative.display().to_string(),
            _ => full,
        }
    };
    let text = match name {
        "Bash" => field("command"),
        "Read" | "Write" | "Edit" | "MultiEdit" => path("file_path"),
        "NotebookEdit" => path("notebook_path"),
        "LS" => path("path"),
        "Glob" => field("pattern"),
        "Grep" => {
            let mut text = format!("pattern: \"{}\"", field("pattern"));
            if input.get("path").is_some() {
                text.push_str(&format!(", path: \"{}\"", path("path")));
            }
            text
        }
        "WebFetch" => field("url"),
        "web_search" => field("query"),
        "BashOutput" => field("bash_id"),
        "KillShell" => field("shell_id"),
        "Task" => field("description"),
        "TodoWrite" | "ExitPlanMode" => String::new(),
        _ => {
            let compact = input.to_string();
            if compact == "{}" {
                String::new()
            } else {
                compact
            }
        }
    };
    let first = text.lines().next().unwrap_or("");
    crate::term::truncate(first, 160)
}

/// The name the UI shows: Claude Code's, e.g. "Update" for Edit.
pub fn display_name(name: &str) -> String {
    match name {
        "Edit" | "MultiEdit" => "Update".into(),
        "Grep" => "Search".into(),
        "LS" => "List".into(),
        "TodoWrite" => "Update Todos".into(),
        "web_search" => "Web Search".into(),
        "WebFetch" => "Fetch".into(),
        "NotebookEdit" => "Edit Notebook".into(),
        other => match other.strip_prefix("mcp__") {
            Some(rest) => {
                let (server, tool) = rest.split_once("__").unwrap_or((rest, ""));
                format!("{server} - {tool} (MCP)")
            }
            None => other.to_string(),
        },
    }
}
