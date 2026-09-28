//! A session: the conversation and everything a turn goes through, as in Claude Code.
//!
//!   prompt → UserPromptSubmit hooks → @file attachments → the model
//!   each tool call → PreToolUse hooks → permission (rules, mode, the user) → the tool →
//!                    PostToolUse hooks → the result back to the model
//!   no more tool calls → Stop hooks (which may send it back to work) → the answer
//!
//! The conversation is compacted (summarized) when it nears the context window, or on /compact.
//! Task runs a sub-agent: a session of its own with the agent's prompt and tools, whose final
//! answer is the tool's result.
use std::cell::RefCell;
use std::path::PathBuf;
use std::rc::Rc;
use std::time::{Duration, Instant};

use serde_json::{json, Value};

use crate::config::{Config, Provider};
use crate::extensions::{Agent, OutputStyle};
use crate::hooks::Hooks;
use crate::llm::{self, Usage};
use crate::mcp::Mcp;
use crate::permissions::{Decision, Mode, Rules};
use crate::session::Transcript;
use crate::settings::{Scope, Settings};
use crate::tools::{self, Outcome};

/// The user's answer to "Do you want to …?".
pub enum Approval {
    Once,
    /// Don't ask again: the rule goes to .claude/settings.local.json.
    Always,
    /// Yes, and accept all edits for the rest of the session.
    AcceptEdits,
    /// No, with what to do instead (if the user said).
    No(Option<String>),
}

/// The user's answer to a plan (ExitPlanMode).
pub enum PlanAnswer {
    Approve(Mode),
    KeepPlanning(Option<String>),
}

/// How a session shows itself: the terminal, -p's plain or JSON output, a sub-agent's line.
pub trait Ui {
    fn begin_request(&mut self) {}
    fn text(&mut self, text: &str);
    fn thinking(&mut self, _text: &str) {}
    fn end_message(&mut self) {}
    fn notice(&mut self, text: &str);
    fn error(&mut self, text: &str);
    fn tool_call(&mut self, _name: &str, _input: &Value, _summary: &str) {}
    fn tool_result(&mut self, _name: &str, _input: &Value, _outcome: &Outcome) {}
    fn ask(&mut self, name: &str, input: &Value, summary: &str, suggestion: &str) -> Approval;
    fn approve_plan(&mut self, plan: &str) -> PlanAnswer;
    /// Each message as it joins the conversation (stream-json output).
    fn message(&mut self, _message: &Value) {}
    fn usage(&mut self, _total_output: u64) {}
    fn interrupted(&mut self) {}
    /// Where a sub-agent reports its steps.
    fn sub_step(&mut self, _agent: &str, _line: &str) {}
    /// The permission mode: the session's, or the one the user switched to meanwhile
    /// (Shift+Tab while Claude works).
    fn sync_mode(&mut self, current: Mode) -> Mode {
        current
    }
}

struct SinkAdapter<'a> {
    ui: &'a mut dyn Ui,
}

impl llm::Sink for SinkAdapter<'_> {
    fn text(&mut self, text: &str) {
        self.ui.text(text);
    }
    fn thinking(&mut self, text: &str) {
        self.ui.thinking(text);
    }
    fn notice(&mut self, text: &str) {
        self.ui.notice(text);
    }
}

/// Shows a sub-agent's work as steps under its Task line, and asks through the parent.
struct SubUi<'a> {
    parent: &'a mut dyn Ui,
    label: String,
}

impl Ui for SubUi<'_> {
    fn text(&mut self, _text: &str) {}
    fn notice(&mut self, text: &str) {
        self.parent.notice(text);
    }
    fn error(&mut self, text: &str) {
        self.parent.sub_step(&self.label, &format!("error: {text}"));
    }
    fn tool_call(&mut self, name: &str, _input: &Value, summary: &str) {
        self.parent.sub_step(&self.label, &format!("{}({summary})", tools::display_name(name)));
    }
    fn ask(&mut self, name: &str, input: &Value, summary: &str, suggestion: &str) -> Approval {
        self.parent.ask(name, input, summary, suggestion)
    }
    fn approve_plan(&mut self, plan: &str) -> PlanAnswer {
        self.parent.approve_plan(plan)
    }
    fn sync_mode(&mut self, current: Mode) -> Mode {
        self.parent.sync_mode(current)
    }
}

pub struct Session {
    pub config: Config,
    pub settings: Settings,
    pub model: String,
    pub messages: Vec<Value>,
    pub context: tools::Context,
    pub mode: Mode,
    pub bypass_available: bool,
    pub rules: Rules,
    pub extra_dirs: Vec<PathBuf>,
    /// --allowedTools / --disallowedTools: rules for this run only.
    pub cli_allow: Vec<String>,
    pub cli_deny: Vec<String>,
    pub hooks: Hooks,
    pub transcript: Option<Transcript>,
    pub mcp: Rc<RefCell<Mcp>>,
    pub agents: Vec<Agent>,
    pub style: Option<OutputStyle>,
    pub usage: Usage,
    /// The last request's whole input (what fills the context window).
    pub context_tokens: u64,
    pub api_time: Duration,
    pub started: Instant,
    pub thinking_on: bool,
    pub depth: usize,
    /// A sub-agent's definition: its prompt and tools.
    pub agent: Option<Agent>,
    pub web_search: bool,
    pub id: String,
    /// What the command line set (it keeps winning when the settings are read again).
    pub overrides: crate::config::Overrides,
    /// /goal: Claude keeps working until this holds (a model judges it at each stop).
    pub goal: Option<String>,
    pub skills: Vec<crate::extensions::Skill>,
    /// Tools a loaded skill allows, for the rest of the prompt.
    pub skill_allow: Vec<String>,
    pub checkpoints: Vec<Checkpoint>,
    /// --plugin-dir folders (kept for reloading the plugins).
    pub plugin_dirs: Vec<PathBuf>,
}

/// Where the conversation and the files stood when a prompt was sent (/rewind goes back to it).
#[derive(Clone)]
pub struct Checkpoint {
    pub prompt: String,
    pub at: std::time::SystemTime,
    /// The conversation's length before the prompt; when the prompt joined a user message
    /// already there, that message's block count too.
    pub messages: usize,
    pub blocks: Option<usize>,
    /// Files the edit tools changed during this prompt, as they were before (None: absent).
    pub files: Vec<(PathBuf, Option<Vec<u8>>)>,
}

/// How many times a goal sends Claude back to work within one prompt.
const GOAL_ROUNDS: u32 = 30;

const REJECTED: &str = "The user doesn't want to proceed with this tool use. The tool use was rejected (eg. if it was a file edit, the new_string was NOT written to the file). STOP what you are doing and wait for the user to tell you how to proceed.";
const INTERRUPTED: &str = "[Request interrupted by user]";
const INTERRUPTED_TOOL: &str = "[Request interrupted by user for tool use]";

/// The thinking budget a prompt asks for in words, as Claude Code reads them.
pub fn thinking_budget(prompt: &str) -> Option<u32> {
    let text = prompt.to_lowercase();
    const MAX: [&str; 7] = ["ultrathink", "think harder", "think intensely", "think longer", "think really hard", "think super hard", "think very hard"];
    const MID: [&str; 5] = ["think hard", "think a lot", "think deeply", "think more", "megathink"];
    if MAX.iter().any(|p| text.contains(p)) {
        return Some(31_999);
    }
    if MID.iter().any(|p| text.contains(p)) {
        return Some(10_000);
    }
    let word = regex::Regex::new(r"\bthink\b").unwrap();
    word.is_match(&text).then_some(4_000)
}

fn text_block(text: impl Into<String>) -> Value {
    json!({"type": "text", "text": text.into()})
}

fn reminder(text: &str) -> Value {
    text_block(format!("<system-reminder>\n{text}\n</system-reminder>"))
}

impl Session {
    fn record(&mut self, message: &Value, ui: &mut dyn Ui) {
        if let Some(transcript) = &mut self.transcript {
            transcript.message(message);
        }
        ui.message(message);
    }

    /// Adds blocks as a user message, merged into the last one if that is the user's too.
    pub fn push_user(&mut self, blocks: Vec<Value>, ui: &mut dyn Ui) {
        if let Some(last) = self.messages.last_mut() {
            if last["role"] == "user" {
                if let Some(content) = last["content"].as_array_mut() {
                    content.extend(blocks.clone());
                    let message = json!({"role": "user", "content": blocks});
                    self.record(&message, ui);
                    return;
                }
            }
        }
        let message = json!({"role": "user", "content": blocks});
        self.messages.push(message.clone());
        self.record(&message, ui);
    }

    /// The settings changed (/config set): the connection, the rules, the hooks and the
    /// environment follow at once. The model changes only if the settings' model did.
    pub fn reload_settings(&mut self) -> Result<(), String> {
        self.settings.apply_env();
        let config = crate::config::load(&self.settings, &self.overrides)?;
        if config.model != self.config.model || config.provider != self.config.provider {
            if let Some(model) = &config.model {
                self.model = model.clone();
            }
        }
        self.config = config;
        self.hooks.config = self.settings.get("/hooks").cloned().unwrap_or_else(|| json!({}));
        crate::plugins::merge_hooks(&mut self.hooks.config);
        self.agents = crate::extensions::agents(&self.settings.project_root);
        self.skills = crate::extensions::skills(&self.settings.project_root);
        self.reload_rules();
        Ok(())
    }

    pub fn reload_rules(&mut self) {
        self.rules = Rules::from_settings(&self.settings, &self.context.cwd, &self.extra_dirs);
    }

    pub fn directories(&self) -> Vec<PathBuf> {
        self.rules.directories.clone()
    }

    pub fn system_prompt(&self) -> String {
        let mcp_instructions = self.mcp.borrow().instructions();
        crate::prompt::system(&crate::prompt::Parts {
            cwd: &self.context.cwd,
            model: &self.model,
            mode: self.mode,
            directories: &self.rules.directories,
            agents: if self.depth == 0 { &self.agents } else { &[] },
            goal: if self.depth == 0 { self.goal.as_deref() } else { None },
            style: self.style.as_ref(),
            mcp_instructions: &mcp_instructions,
            replace: self.config.system_prompt.as_deref(),
            append: self.config.append_system_prompt.as_deref(),
            agent_prompt: self.agent.as_ref().map(|a| a.prompt.as_str()),
        })
    }

    /// The tools this session offers the model now.
    pub fn tool_specs(&self) -> Vec<Value> {
        let mut specs = tools::specs();
        specs.extend(self.mcp.borrow().specs());
        let allowed_by_agent = self.agent.as_ref().and_then(|a| a.tools.clone());
        specs.retain(|spec| {
            let name = spec["name"].as_str().unwrap_or("");
            if name == "Task" && self.depth > 0 {
                return false;
            }
            if name == "ExitPlanMode" && self.mode != Mode::Plan {
                return false;
            }
            if self.cli_deny.iter().any(|rule| rule == name) {
                return false;
            }
            if let Some(allowed) = &allowed_by_agent {
                return allowed.iter().any(|tool| tool == name || tool.split('(').next() == Some(name) || (tool.starts_with("mcp__") && name.starts_with(tool.as_str())));
            }
            true
        });
        if !self.skills.is_empty() && !self.cli_deny.iter().any(|r| r == "Skill") && allowed_by_agent.as_ref().is_none_or(|a| a.iter().any(|t| t == "Skill")) {
            specs.push(tools::skill_spec(&self.skills));
        }
        if self.web_search && self.config.provider == Provider::Anthropic && !self.cli_deny.iter().any(|r| r == "WebSearch") {
            let agent_allows = allowed_by_agent.as_ref().is_none_or(|allowed| allowed.iter().any(|t| t == "WebSearch"));
            if agent_allows {
                specs.push(tools::web_search_tool());
            }
        }
        specs
    }

    fn decide(&self, name: &str, input: &Value) -> Decision {
        let cwd = &self.context.cwd;
        let mut rules = self.rules.clone();
        rules.allow.extend(self.cli_allow.iter().chain(&self.skill_allow).filter_map(|r| crate::permissions::Rule::parse(r)));
        rules.deny.extend(self.cli_deny.iter().filter_map(|r| crate::permissions::Rule::parse(r)));
        rules.check(self.mode, name, input, cwd)
    }

    fn run_tool(&mut self, name: &str, mut input: Value, ui: &mut dyn Ui) -> Outcome {
        let cwd = self.context.cwd.clone();
        let summary = tools::summary(name, &input, &cwd);
        ui.tool_call(name, &input, &summary);
        self.mode = ui.sync_mode(self.mode);

        let pre = self.hooks.run("PreToolUse", Some(name), &cwd, json!({"tool_name": name, "tool_input": input}));
        for message in &pre.messages {
            ui.notice(message);
        }
        if let Some(updated) = pre.updated_input.clone() {
            input = updated;
        }
        let decision = if let Some(reason) = pre.blocked {
            Decision::Deny(format!("PreToolUse hook blocked this call: {reason}"))
        } else {
            match pre.permission.as_ref().map(|(d, r)| (d.as_str(), r.clone())) {
                Some(("allow", _)) => match self.decide(name, &input) {
                    Decision::Deny(reason) => Decision::Deny(reason),
                    _ => Decision::Allow,
                },
                Some(("deny", reason)) => Decision::Deny(format!("A hook denied this call: {reason}")),
                Some(("ask", _)) => Decision::Ask,
                _ => self.decide(name, &input),
            }
        };
        let outcome = match decision {
            Decision::Deny(reason) => tools::fail(reason),
            Decision::Ask => {
                let notification = self.hooks.run("Notification", None, &cwd, json!({"message": format!("Claude needs your permission to use {}", tools::display_name(name))}));
                for message in notification.messages {
                    ui.notice(&message);
                }
                let suggestion = Rules::suggestion(name, &input);
                match ui.ask(name, &input, &summary, &suggestion) {
                    Approval::No(feedback) => {
                        let mut text = REJECTED.to_string();
                        if let Some(feedback) = feedback.filter(|f| !f.trim().is_empty()) {
                            text = format!("The user doesn't want to proceed with this tool use. The tool use was rejected. The user said:\n{feedback}");
                        }
                        let outcome = tools::fail(text);
                        ui.tool_result(name, &input, &outcome);
                        return outcome;
                    }
                    Approval::Always => {
                        if let Err(message) = self.settings.add_rule(Scope::Local, "allow", &suggestion) {
                            ui.error(&message);
                        }
                        self.reload_rules();
                        self.execute(name, &input, ui)
                    }
                    Approval::AcceptEdits => {
                        self.mode = ui.sync_mode(Mode::AcceptEdits);
                        self.execute(name, &input, ui)
                    }
                    Approval::Once => self.execute(name, &input, ui),
                }
            }
            Decision::Allow => self.execute(name, &input, ui),
        };
        let mut outcome = outcome;
        if !crate::interrupt::is_set() {
            let post = self.hooks.run(
                "PostToolUse",
                Some(name),
                &self.context.cwd.clone(),
                json!({"tool_name": name, "tool_input": input, "tool_response": {"output": outcome.text, "is_error": outcome.is_error}}),
            );
            for message in &post.messages {
                ui.notice(message);
            }
            if let Some(reason) = post.blocked {
                outcome.text.push_str(&format!("\n\n<system-reminder>PostToolUse hook feedback:\n{reason}</system-reminder>"));
            }
            for context in post.context {
                outcome.text.push_str(&format!("\n\n<system-reminder>{context}</system-reminder>"));
            }
        }
        ui.tool_result(name, &input, &outcome);
        outcome
    }

    fn execute(&mut self, name: &str, input: &Value, ui: &mut dyn Ui) -> Outcome {
        match name {
            "Task" => self.run_subagent(input, ui),
            "Skill" => {
                let name = input.get("skill").and_then(Value::as_str).unwrap_or("").trim_start_matches('/');
                match self.skills.iter().find(|s| s.name == name) {
                    Some(skill) => {
                        self.skill_allow.extend(skill.allowed_tools.iter().cloned());
                        tools::ok(format!("Launching skill: {}\n\nBase directory for this skill: {}\n\n{}", skill.name, skill.dir.display(), skill.body))
                    }
                    None => tools::fail(format!("Unknown skill: {name}. Available: {}", self.skills.iter().map(|s| s.name.as_str()).collect::<Vec<_>>().join(", "))),
                }
            }
            "ExitPlanMode" => {
                let plan = input.get("plan").and_then(Value::as_str).unwrap_or("");
                match ui.approve_plan(plan) {
                    PlanAnswer::Approve(mode) => {
                        self.mode = ui.sync_mode(mode);
                        tools::ok("User has approved your plan. You can now start coding. Start with updating your todo list if applicable")
                    }
                    PlanAnswer::KeepPlanning(feedback) => tools::fail(match feedback {
                        Some(text) => format!("The user doesn't want to proceed with this plan yet. They said:\n{text}\nKeep planning; do not make changes."),
                        None => "The user doesn't want to proceed with this plan yet. Stay in plan mode and ask what they want changed.".into(),
                    }),
                }
            }
            n if n.starts_with("mcp__") => self.mcp.borrow_mut().call(n, input),
            n => {
                if crate::permissions::is_edit(n) {
                    self.snapshot(input);
                }
                tools::run(n, input, &mut self.context)
            }
        }
    }

    fn run_subagent(&mut self, input: &Value, ui: &mut dyn Ui) -> Outcome {
        let kind = input.get("subagent_type").and_then(Value::as_str).unwrap_or("general-purpose");
        let description = input.get("description").and_then(Value::as_str).unwrap_or(kind).to_string();
        let Some(prompt) = input.get("prompt").and_then(Value::as_str) else { return tools::fail("InputValidationError: prompt is required") };
        let Some(agent) = self.agents.iter().find(|a| a.name == kind).cloned() else {
            let names = self.agents.iter().map(|a| a.name.as_str()).collect::<Vec<_>>().join(", ");
            return tools::fail(format!("Agent type '{kind}' not found. Available agents: {names}"));
        };
        let model = agent.model.as_deref().map(|m| crate::config::resolve_model(self.config.provider, m)).unwrap_or_else(|| self.model.clone());
        let mut child = Session {
            config: self.config.clone(),
            settings: self.settings.clone(),
            model,
            messages: Vec::new(),
            context: tools::Context::new(self.context.cwd.clone()),
            mode: if self.mode == Mode::Plan { Mode::Plan } else { self.mode },
            bypass_available: self.bypass_available,
            rules: self.rules.clone(),
            extra_dirs: self.extra_dirs.clone(),
            cli_allow: self.cli_allow.clone(),
            cli_deny: self.cli_deny.clone(),
            hooks: self.hooks.clone(),
            transcript: None,
            mcp: self.mcp.clone(),
            agents: Vec::new(),
            style: None,
            usage: Usage::default(),
            context_tokens: 0,
            api_time: Duration::ZERO,
            started: Instant::now(),
            thinking_on: false,
            depth: self.depth + 1,
            agent: Some(agent.clone()),
            web_search: self.web_search,
            id: self.id.clone(),
            overrides: self.overrides.clone(),
            goal: None,
            skills: self.skills.clone(),
            skill_allow: Vec::new(),
            checkpoints: Vec::new(),
            plugin_dirs: self.plugin_dirs.clone(),
        };
        child.config.system_prompt = None;
        let started = Instant::now();
        let mut sub_ui = SubUi { parent: ui, label: description.clone() };
        let result = child.prompt(prompt, &mut sub_ui);
        self.usage.add(child.usage);
        self.api_time += child.api_time;
        let tool_uses = child.messages.iter().flat_map(|m| m["content"].as_array().cloned().unwrap_or_default()).filter(|b| b["type"] == "tool_use").count();
        match result {
            Ok(text) => Outcome {
                text: if text.trim().is_empty() { "(the agent finished without a report)".into() } else { text },
                display: None,
                ..tools::ok(String::new())
            }
            .with_note(format!("Done ({tool_uses} tool uses · {} tokens · {})", crate::util::tokens(child.usage.output + child.usage.input), crate::util::duration(started.elapsed()))),
            Err(message) => tools::fail(format!("The agent failed: {message}")),
        }
    }

    /// The conversation is near the context window: summarize it (settings autoCompactEnabled
    /// false turns this off).
    fn needs_compaction(&self) -> bool {
        self.settings.bool("/autoCompactEnabled") != Some(false) && self.context_tokens > self.config.context_window * 85 / 100 && self.messages.len() > 2
    }

    pub fn compact(&mut self, instructions: Option<&str>, auto: bool, ui: &mut dyn Ui) -> Result<(), String> {
        let trigger = if auto { "auto" } else { "manual" };
        let cwd = self.context.cwd.clone();
        let pre = self.hooks.run("PreCompact", Some(trigger), &cwd, json!({"trigger": trigger, "custom_instructions": instructions.unwrap_or("")}));
        let mut ask = String::from(
            "Your task is to create a detailed summary of the conversation so far, paying close attention to the user's explicit requests and your previous actions. It must capture everything needed to continue the work without losing context.\n\n\
             Organize it in these sections:\n1. Primary Request and Intent: all of the user's explicit requests, in detail\n2. Key Technical Concepts\n3. Files and Code Sections: the files examined, changed or created, with the important snippets and why they matter\n4. Errors and fixes: what went wrong and how it was fixed, and any user feedback\n5. Problem Solving\n6. All user messages (not tool results), in order\n7. Pending Tasks\n8. Current Work: precisely what was being worked on just before this summary\n9. Optional Next Step: only if it follows directly from the latest request\n\nReply with the summary only, no tool calls.",
        );
        if let Some(instructions) = instructions.filter(|i| !i.trim().is_empty()) {
            ask.push_str(&format!("\n\nAdditional instructions: {instructions}"));
        }
        for context in &pre.context {
            ask.push_str(&format!("\n\n{context}"));
        }
        let mut messages = self.messages.clone();
        // The request must end with the user; a dangling tool call gets a stub result.
        if messages.last().is_some_and(|m| m["role"] == "user") {
            if let Some(content) = messages.last_mut().and_then(|m| m["content"].as_array_mut()) {
                content.push(text_block(ask.clone()));
            }
        } else {
            messages.push(json!({"role": "user", "content": [text_block(ask.clone())]}));
        }
        struct Quiet;
        impl llm::Sink for Quiet {
            fn text(&mut self, _: &str) {}
            fn thinking(&mut self, _: &str) {}
        }
        ui.begin_request();
        let started = Instant::now();
        let turn = llm::complete(
            &self.config,
            &llm::Request { model: &self.model, system: "You are a helpful AI assistant tasked with summarizing conversations.", messages: &messages, tools: &[], thinking: None },
            &mut Quiet,
        )
        .map_err(|error| error.message)?;
        self.api_time += started.elapsed();
        self.usage.add(turn.usage);
        let summary: String = turn.content.iter().filter(|b| b["type"] == "text").filter_map(|b| b["text"].as_str()).collect::<Vec<_>>().join("\n");
        if summary.trim().is_empty() {
            return Err("the model returned no summary".into());
        }
        let mut text = format!("This session is being continued from a previous conversation that ran out of context. The conversation is summarized below:\n{summary}");
        if auto {
            text.push_str("\n\nPlease continue the conversation from where we left it off without asking the user any further questions. Continue with the last task that you were asked to work on.");
        }
        let mut blocks = vec![text_block(text)];
        if !self.context.todos.is_empty() {
            let list = self.context.todos.iter().map(|t| format!("- [{}] {}", t.status, t.content)).collect::<Vec<_>>().join("\n");
            blocks.push(reminder(&format!("The todo list before the summary:\n{list}")));
        }
        self.messages = vec![json!({"role": "user", "content": blocks})];
        self.context_tokens = turn.usage.output;
        self.context.read_files.clear();
        if let Some(transcript) = &mut self.transcript {
            transcript.reset(&self.messages, "compact");
        }
        let start = self.hooks.run("SessionStart", Some("compact"), &cwd, json!({"source": "compact"}));
        if !start.context.is_empty() {
            let blocks = start.context.iter().map(|c| reminder(c)).collect();
            self.push_user(blocks, ui);
        }
        Ok(())
    }

    /// Files the prompt names as @path: attached, as if read.
    fn attachments(&mut self, prompt: &str) -> Vec<Value> {
        let mut blocks = Vec::new();
        let pattern = regex::Regex::new(r#"(?:^|\s)@("[^"]+"|[^\s]+)"#).unwrap();
        for caps in pattern.captures_iter(prompt) {
            let raw = caps[1].trim_matches('"').trim_end_matches([',', '.', ')', ';', ':']);
            let path = self.context.resolve(raw);
            if path.is_dir() {
                let listing = tools::files::ls(&json!({"path": path.to_string_lossy()}), &mut self.context);
                blocks.push(reminder(&format!("Called the LS tool for @{raw}. Result:\n{}", listing.text)));
            } else if path.is_file() {
                let read = tools::files::read(&json!({"file_path": path.to_string_lossy()}), &mut self.context);
                match read.content {
                    Some(Value::Array(content)) => blocks.extend(content),
                    _ => blocks.push(reminder(&format!("Called the Read tool with the following input: {{\"file_path\":\"{}\"}}\nResult of calling the Read tool:\n{}", path.display(), read.text))),
                }
            }
        }
        blocks
    }

    /// Keeps a file's content before its first change in this prompt (for /rewind).
    fn snapshot(&mut self, input: &Value) {
        let Some(path) = input.get("file_path").or(input.get("notebook_path")).and_then(Value::as_str).map(|p| self.context.resolve(p)) else { return };
        if let Some(checkpoint) = self.checkpoints.last_mut() {
            if !checkpoint.files.iter().any(|(seen, _)| *seen == path) {
                checkpoint.files.push((path.clone(), std::fs::read(&path).ok()));
            }
        }
    }

    /// Puts the files back as they were when checkpoint `index` was taken. The files restored.
    pub fn rewind_code(&mut self, index: usize) -> Vec<PathBuf> {
        let mut restored = Vec::new();
        // Latest first, so the earliest state of each file is the one left.
        for checkpoint in self.checkpoints[index..].iter().rev() {
            for (path, content) in &checkpoint.files {
                let _ = match content {
                    Some(bytes) => std::fs::write(path, bytes),
                    None => std::fs::remove_file(path),
                };
                self.context.read_files.remove(path);
                if !restored.contains(path) {
                    restored.push(path.clone());
                }
            }
        }
        for checkpoint in self.checkpoints[index..].iter_mut() {
            checkpoint.files.clear();
        }
        restored
    }

    /// The conversation as it was before checkpoint `index`'s prompt; that prompt's text.
    pub fn rewind_conversation(&mut self, index: usize) -> String {
        let checkpoint = self.checkpoints[index].clone();
        self.messages.truncate(checkpoint.messages);
        if let (Some(blocks), Some(last)) = (checkpoint.blocks, self.messages.last_mut()) {
            if let Some(content) = last["content"].as_array_mut() {
                content.truncate(blocks);
            }
        }
        if self.messages.last().is_some_and(|m| m["content"].as_array().is_some_and(|c| c.is_empty())) {
            self.messages.pop();
        }
        self.checkpoints.truncate(index);
        self.context_tokens = 0;
        if let Some(transcript) = &mut self.transcript {
            transcript.reset(&self.messages, "rewind");
        }
        checkpoint.prompt
    }

    /// Asks the model whether the goal holds now. (met, why)
    fn check_goal(&mut self, goal: &str) -> Result<(bool, String), String> {
        let mut messages = self.messages.clone();
        let question = format!(
            "<goal-check>\nThe user set this goal for the session: {goal}\n\nJudge from the conversation above (what was actually done and verified, not what was promised) whether the goal is fully achieved now. Reply with one JSON object and nothing else: {{\"met\": true or false, \"reason\": \"one short sentence\"}}\n</goal-check>"
        );
        match messages.last_mut() {
            Some(last) if last["role"] == "user" => {
                if let Some(content) = last["content"].as_array_mut() {
                    content.push(text_block(question));
                }
            }
            _ => messages.push(json!({"role": "user", "content": [text_block(question)]})),
        }
        struct Quiet;
        impl llm::Sink for Quiet {
            fn text(&mut self, _: &str) {}
            fn thinking(&mut self, _: &str) {}
        }
        let started = Instant::now();
        let turn = llm::complete(
            &self.config,
            &llm::Request { model: &self.model, system: "You check whether a coding session has achieved a goal. Be strict: unverified or partial work is not achieved.", messages: &messages, tools: &[], thinking: None },
            &mut Quiet,
        )
        .map_err(|error| error.message)?;
        self.api_time += started.elapsed();
        self.usage.add(turn.usage);
        let text: String = turn.content.iter().filter(|b| b["type"] == "text").filter_map(|b| b["text"].as_str()).collect();
        let json_part = text.find('{').and_then(|start| text.rfind('}').map(|end| &text[start..=end])).unwrap_or("");
        let verdict: Value = serde_json::from_str(json_part).map_err(|_| format!("the goal check gave no verdict: {}", text.chars().take(200).collect::<String>()))?;
        Ok((verdict["met"].as_bool().unwrap_or(false), verdict["reason"].as_str().unwrap_or("").to_string()))
    }

    /// One prompt, to the end of the model's work on it. The last answer's text.
    pub fn prompt(&mut self, text: &str, ui: &mut dyn Ui) -> Result<String, String> {
        crate::interrupt::clear();
        self.skill_allow.clear();
        let cwd = self.context.cwd.clone();
        // A sub-agent's prompt comes from the model, not the user: no UserPromptSubmit for it.
        let hook = match self.depth {
            0 => self.hooks.run("UserPromptSubmit", None, &cwd, json!({"prompt": text})),
            _ => crate::hooks::Outcome::default(),
        };
        for message in &hook.messages {
            ui.notice(message);
        }
        if let Some(reason) = hook.blocked {
            return Err(format!("Prompt blocked by a UserPromptSubmit hook: {reason}"));
        }
        if let Some(reason) = hook.stop {
            return Err(reason);
        }
        if self.depth == 0 {
            let blocks = match self.messages.last() {
                Some(last) if last["role"] == "user" => last["content"].as_array().map(Vec::len),
                _ => None,
            };
            self.checkpoints.push(Checkpoint { prompt: text.to_string(), at: std::time::SystemTime::now(), messages: self.messages.len(), blocks, files: Vec::new() });
        }
        let mut blocks = vec![text_block(text)];
        blocks.extend(self.attachments(text));
        for context in hook.context {
            blocks.push(reminder(&context));
        }
        if self.mode == Mode::Plan {
            blocks.push(reminder("Plan mode is active: research with read-only tools, then present your plan with ExitPlanMode. Make no changes before the user approves."));
        }
        self.push_user(blocks, ui);
        let thinking = match self.config.provider {
            Provider::Anthropic => thinking_budget(text).or((self.thinking_on || self.config.always_thinking).then_some(self.config.thinking_budget)),
            Provider::OpenAi => None,
        };
        self.turns(ui, thinking)
    }

    /// Continues the conversation as it is (after --continue, or a prompt already added).
    pub fn turns(&mut self, ui: &mut dyn Ui, thinking: Option<u32>) -> Result<String, String> {
        let mut stop_hook_active = false;
        let mut goal_rounds = 0u32;
        let mut last_text = String::new();
        for _ in 0..self.config.max_turns {
            self.mode = ui.sync_mode(self.mode);
            if crate::interrupt::take() {
                self.push_user(vec![text_block(INTERRUPTED)], ui);
                ui.interrupted();
                return Ok(last_text);
            }
            if self.needs_compaction() {
                ui.notice("Context is almost full: compacting the conversation…");
                if let Err(message) = self.compact(None, true, ui) {
                    ui.error(&format!("Compaction failed: {message}"));
                }
            }
            // Background shells that finished meanwhile.
            let finished = self.context.shells.newly_finished();
            if !finished.is_empty() && self.messages.last().is_some_and(|m| m["role"] == "user") {
                let lines = finished.iter().map(|(id, command, code)| format!("Background Bash {id} (command: {command}) has completed with exit code {code}. Read its output with BashOutput.")).collect::<Vec<_>>().join("\n");
                self.push_user(vec![reminder(&lines)], ui);
            }

            let system = self.system_prompt();
            let specs = self.tool_specs();
            ui.begin_request();
            let started = Instant::now();
            let result = llm::complete(
                &self.config,
                &llm::Request { model: &self.model, system: &system, messages: &self.messages, tools: &specs, thinking },
                &mut SinkAdapter { ui },
            );
            self.api_time += started.elapsed();
            ui.end_message();
            let turn = match result {
                Ok(turn) => turn,
                Err(error) if error.interrupted => {
                    crate::interrupt::clear();
                    self.push_user(vec![text_block(INTERRUPTED)], ui);
                    ui.interrupted();
                    return Ok(last_text);
                }
                Err(error) => return Err(error.message),
            };
            self.usage.add(turn.usage);
            self.context_tokens = turn.usage.input + turn.usage.cache_read + turn.usage.cache_write + turn.usage.output;
            ui.usage(self.usage.output);
            if turn.content.is_empty() {
                return Ok(last_text);
            }
            let message = json!({"role": "assistant", "content": turn.content});
            self.messages.push(message.clone());
            self.record(&message, ui);
            last_text = turn.content.iter().filter(|b| b["type"] == "text").filter_map(|b| b["text"].as_str()).collect::<Vec<_>>().join("");

            // Web searches Anthropic ran for this answer.
            for block in turn.content.iter().filter(|b| b["type"] == "server_tool_use") {
                let name = block["name"].as_str().unwrap_or("web_search");
                ui.tool_call(name, &block["input"], &tools::summary(name, &block["input"], &self.context.cwd));
            }
            for block in turn.content.iter().filter(|b| b["type"] == "web_search_tool_result") {
                let results = block["content"].as_array().map_or(0, Vec::len);
                ui.tool_result("web_search", &json!({}), &tools::ok(format!("Did 1 search ({results} results)")));
            }

            let calls: Vec<(String, String, Value)> = turn
                .content
                .iter()
                .filter(|b| b["type"] == "tool_use")
                .map(|b| (b["id"].as_str().unwrap_or("").to_string(), b["name"].as_str().unwrap_or("").to_string(), b.get("input").cloned().unwrap_or(json!({}))))
                .collect();
            if calls.is_empty() {
                if turn.stop_reason == "max_tokens" {
                    ui.notice("The answer reached the output token limit (maxTokens).");
                }
                let event = if self.depth == 0 { "Stop" } else { "SubagentStop" };
                let stop = self.hooks.run(event, None, &self.context.cwd.clone(), json!({"stop_hook_active": stop_hook_active}));
                for message in &stop.messages {
                    ui.notice(message);
                }
                if let (Some(reason), None) = (stop.blocked, &stop.stop) {
                    stop_hook_active = true;
                    self.push_user(vec![text_block(format!("{event} hook feedback:\n{reason}"))], ui);
                    continue;
                }
                // /goal: not done until the goal holds.
                if let (Some(goal), 0) = (self.goal.clone(), self.depth) {
                    ui.begin_request();
                    match self.check_goal(&goal) {
                        Ok((true, reason)) => {
                            self.goal = None;
                            ui.notice(&format!("◎ Goal achieved: {goal}{}", if reason.is_empty() { String::new() } else { format!(" ({reason})") }));
                        }
                        Ok((false, reason)) if goal_rounds < GOAL_ROUNDS => {
                            goal_rounds += 1;
                            ui.notice(&format!("◎ Goal not met yet: {reason}"));
                            self.push_user(vec![text_block(format!("The goal is not met yet: {reason}\nKeep working toward it (do not stop to ask): {goal}"))], ui);
                            continue;
                        }
                        Ok((false, reason)) => ui.notice(&format!("◎ Stopped after {GOAL_ROUNDS} rounds; the goal is still not met: {reason}")),
                        Err(message) => ui.error(&format!("Goal check failed: {message}")),
                    }
                }
                return Ok(last_text);
            }

            let mut results = Vec::new();
            let mut stopped = false;
            for (id, name, input) in calls {
                if stopped || crate::interrupt::is_set() {
                    stopped = true;
                    results.push(json!({"type": "tool_result", "tool_use_id": id, "content": INTERRUPTED_TOOL, "is_error": true}));
                    continue;
                }
                let outcome = self.run_tool(&name, input, ui);
                let content = match outcome.content {
                    Some(content) => content,
                    None => json!(if outcome.text.is_empty() { "(no output)".to_string() } else { outcome.text.clone() }),
                };
                results.push(json!({"type": "tool_result", "tool_use_id": id, "content": content, "is_error": outcome.is_error}));
                if crate::interrupt::is_set() {
                    stopped = true;
                }
            }
            self.push_user(results, ui);
            if stopped {
                crate::interrupt::clear();
                self.push_user(vec![text_block(INTERRUPTED_TOOL)], ui);
                ui.interrupted();
                return Ok(last_text);
            }
        }
        Err(format!("Reached the maximum number of turns ({})", self.config.max_turns))
    }

    /// /clear: a fresh conversation (and transcript).
    pub fn clear(&mut self, ui: &mut dyn Ui) {
        self.messages.clear();
        self.checkpoints.clear();
        self.context_tokens = 0;
        self.context.read_files.clear();
        self.context.todos.clear();
        let cwd = self.context.cwd.clone();
        self.hooks.run("SessionEnd", None, &cwd, json!({"reason": "clear"}));
        self.transcript = Some(Transcript::new(&cwd, None));
        if let Some(transcript) = &self.transcript {
            self.id = transcript.id.clone();
            self.hooks.session_id = transcript.id.clone();
            self.hooks.transcript_path = transcript.path.display().to_string();
        }
        let start = self.hooks.run("SessionStart", Some("clear"), &cwd, json!({"source": "clear"}));
        if !start.context.is_empty() {
            self.push_user(start.context.iter().map(|c| reminder(c)).collect(), ui);
        }
    }
}

trait WithNote {
    fn with_note(self, note: String) -> Outcome;
}

impl WithNote for Outcome {
    /// A sub-agent's result keeps its report as the text; the note is only for the terminal.
    fn with_note(mut self, note: String) -> Outcome {
        self.display = Some(tools::Display::Note(note));
        self
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn thinking_words() {
        assert_eq!(thinking_budget("please ultrathink about this"), Some(31_999));
        assert_eq!(thinking_budget("Think hard!"), Some(10_000));
        assert_eq!(thinking_budget("what do you think"), Some(4_000));
        assert_eq!(thinking_budget("rethinking the design"), None);
    }
}
