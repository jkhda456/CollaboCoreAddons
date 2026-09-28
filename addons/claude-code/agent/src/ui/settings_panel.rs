//! /config: the settings as a panel, as Claude Code shows them. ↑↓ move, Enter or Space
//! changes (a switch flips, a choice moves on, text and numbers open an edit line, the model
//! opens a list), ← → step a choice, Delete returns a value to its default, Tab picks the file
//! changes go to (local, project, user), Esc closes. A change is saved and in effect at once.
use serde_json::{json, Value};

use super::editor::Key;
use super::terminal::TerminalUi;
use crate::agent::Session;
use crate::config::{Provider, ALIASES};
use crate::settings::Scope;
use crate::term;

enum Kind {
    Switch,
    Choice(Vec<(&'static str, &'static str)>),
    Text,
    Secret,
    Number,
    Model,
    Style,
}

struct Field {
    section: &'static str,
    pointer: &'static str,
    label: &'static str,
    help: &'static str,
    kind: Kind,
}

fn fields() -> Vec<Field> {
    vec![
        Field { section: "Model connection", pointer: "/provider", label: "Provider", help: "anthropic: the Messages API (api.anthropic.com or a gateway). openai: any OpenAI-compatible server, such as a local LLM.", kind: Kind::Choice(vec![("anthropic", "anthropic"), ("openai", "openai (compatible)")]) },
        Field { section: "Model connection", pointer: "/baseUrl", label: "Base URL", help: "Where requests go. localhost is the host computer (it makes the request): allow it with allowHostLoopback.", kind: Kind::Text },
        Field { section: "Model connection", pointer: "/model", label: "Model", help: "The model for new requests. Enter opens the list: Claude's aliases, or the models the server offers.", kind: Kind::Model },
        Field { section: "Model connection", pointer: "/apiKey", label: "API key", help: "Sent from the sandbox. For https endpoints prefer the app's apiKey: the host adds it and the sandbox never sees it.", kind: Kind::Secret },
        Field { section: "Model connection", pointer: "/maxTokens", label: "Max output tokens", help: "The longest answer, in tokens.", kind: Kind::Number },
        Field { section: "Model connection", pointer: "/contextWindow", label: "Context window", help: "Tokens the model takes in; the conversation is compacted before it fills.", kind: Kind::Number },
        Field { section: "Model connection", pointer: "/stream", label: "Streaming", help: "Show answers as they arrive. Turn off for servers that stream tool calls badly.", kind: Kind::Switch },
        Field { section: "Behaviour", pointer: "/permissions/defaultMode", label: "Default permission mode", help: "The mode sessions start in (Shift+Tab changes it in a session).", kind: Kind::Choice(vec![("default", "default (ask)"), ("acceptEdits", "accept edits"), ("plan", "plan"), ("bypassPermissions", "bypass permissions")]) },
        Field { section: "Behaviour", pointer: "/autoCompactEnabled", label: "Auto-compact", help: "Summarize the conversation when the context is nearly full.", kind: Kind::Switch },
        Field { section: "Behaviour", pointer: "/alwaysThinkingEnabled", label: "Thinking mode", help: "Extended thinking on every request (Anthropic). Tab in the input box toggles it for the session.", kind: Kind::Switch },
        Field { section: "Behaviour", pointer: "/maxThinkingTokens", label: "Thinking budget", help: "Tokens Claude may think with when thinking is on.", kind: Kind::Number },
        Field { section: "Behaviour", pointer: "/webSearch", label: "Web search", help: "Offer the WebSearch tool (Anthropic's server tool).", kind: Kind::Switch },
        Field { section: "Behaviour", pointer: "/outputStyle", label: "Output style", help: "How Claude answers: default, Explanatory, Learning, or your own from .claude/output-styles.", kind: Kind::Style },
        Field { section: "Behaviour", pointer: "/theme", label: "Theme", help: "Colors: dark, light, their colorblind-friendly versions, or plain terminal colors.", kind: Kind::Choice(vec![("dark", "dark"), ("light", "light"), ("dark-daltonized", "dark (colorblind)"), ("light-daltonized", "light (colorblind)"), ("ansi", "terminal colors")]) },
        Field { section: "Behaviour", pointer: "/editorMode", label: "Editor mode", help: "Keys in the input box: normal, or vim (Esc for normal mode; i a o to insert; h l w b 0 $ x dd cw …).", kind: Kind::Choice(vec![("normal", "normal"), ("vim", "vim")]) },
        Field { section: "Behaviour", pointer: "/verbose", label: "Verbose output", help: "Show thinking and full tool output in the conversation.", kind: Kind::Switch },
        Field { section: "Behaviour", pointer: "/preferredNotifChannel", label: "Notifications", help: "Ring the terminal bell when a long turn finishes.", kind: Kind::Choice(vec![("none", "none"), ("terminal_bell", "terminal bell")]) },
        Field { section: "Behaviour", pointer: "/maxTurns", label: "Max turns", help: "Model calls per prompt before Claude stops.", kind: Kind::Number },
        Field { section: "Behaviour", pointer: "/cleanupPeriodDays", label: "Keep transcripts (days)", help: "Conversations older than this are deleted at start.", kind: Kind::Number },
    ]
}

/// The value in effect when no settings file sets it.
fn default_text(field: &Field, session: &Session) -> String {
    let config = &session.config;
    match field.pointer {
        "/provider" => "anthropic".into(),
        "/baseUrl" => match config.provider {
            Provider::Anthropic => crate::config::ANTHROPIC_BASE_URL.into(),
            Provider::OpenAi => crate::config::OPENAI_BASE_URL.into(),
        },
        "/model" => match config.provider {
            Provider::Anthropic => crate::config::ANTHROPIC_MODEL.into(),
            Provider::OpenAi => "the server's first model".into(),
        },
        "/apiKey" => "not set".into(),
        "/maxTokens" => match config.provider {
            Provider::Anthropic => crate::config::ANTHROPIC_MAX_TOKENS.to_string(),
            Provider::OpenAi => "server default".into(),
        },
        "/contextWindow" => match config.provider {
            Provider::Anthropic => "200000".into(),
            Provider::OpenAi => "32768".into(),
        },
        "/stream" | "/autoCompactEnabled" | "/webSearch" => "true".into(),
        "/alwaysThinkingEnabled" | "/verbose" => "false".into(),
        "/maxThinkingTokens" => "10000".into(),
        "/permissions/defaultMode" => "default".into(),
        "/outputStyle" => "default".into(),
        "/preferredNotifChannel" => "none".into(),
        "/theme" => "dark".into(),
        "/editorMode" => "normal".into(),
        "/maxTurns" => "200".into(),
        "/cleanupPeriodDays" => "30".into(),
        _ => String::new(),
    }
}

fn shown(field: &Field, value: Option<&Value>, session: &Session) -> String {
    match (value, &field.kind) {
        (None, _) => default_text(field, session),
        (Some(_), Kind::Secret) => "set ••••••••".into(),
        (Some(Value::String(text)), Kind::Choice(options)) => options.iter().find(|(v, _)| v == text).map_or(text.clone(), |(_, label)| label.to_string()),
        (Some(Value::String(text)), _) => text.clone(),
        (Some(other), _) => other.to_string(),
    }
}

fn scope_label(scope: Scope) -> &'static str {
    match scope {
        Scope::Local => "Local (.claude/settings.local.json, this machine only)",
        Scope::Project => "Project (.claude/settings.json, shared)",
        Scope::User => "User (~/.claude/settings.json, every project)",
        Scope::Addon => "the app's settings",
        Scope::Cli => "--settings",
    }
}

struct Editing {
    buffer: Vec<char>,
    secret: bool,
}

pub fn open(session: &mut Session, ui: &mut TerminalUi) {
    let fields = fields();
    let mut selected = 0usize;
    let mut scope = Scope::Local;
    let mut status = String::new();
    let mut editing: Option<Editing> = None;
    let mut drawn = 0usize;
    let mode = term::Mode::raw();
    loop {
        let columns = term::columns().max(40);
        let (_, rows) = term::size();
        let inner = columns - 4;
        let mut lines = vec![term::accent(&format!("╭{}╮", "─".repeat(columns - 2)))];
        let row = |lines: &mut Vec<String>, text: String| {
            let text = term::truncate(&text, inner);
            let pad = inner.saturating_sub(term::width(&text));
            lines.push(format!("{} {text}{} {}", term::accent("│"), " ".repeat(pad), term::accent("│")));
        };
        row(&mut lines, term::bold("Settings"));
        row(&mut lines, term::dim(&format!("Changes are saved to: {}   (tab to change)", scope_label(scope))));
        row(&mut lines, String::new());
        // A window over the list when the terminal is short.
        let room = rows.saturating_sub(12).max(6);
        let first = selected.saturating_sub(room.saturating_sub(3)).min(fields.len().saturating_sub(room));
        let label_width = 26;
        let mut section = "";
        for (index, field) in fields.iter().enumerate().skip(first).take(room) {
            if field.section != section {
                section = field.section;
                row(&mut lines, term::dim(section));
            }
            let value = session.settings.get(field.pointer);
            let source = match session.settings.source_of(field.pointer) {
                Some(scope) => format!(" ({})", scope.name()),
                None => " (default)".into(),
            };
            let text = format!("{:<label_width$}{}", field.label, shown(field, value, session));
            row(&mut lines, match index == selected {
                true => format!("{} {}{}", term::accent("❯"), term::accent(&text), term::dim(&source)),
                false => format!("  {text}{}", term::dim(&source)),
            });
        }
        row(&mut lines, String::new());
        let field = &fields[selected];
        match &editing {
            Some(edit) => {
                let text: String = if edit.secret { "•".repeat(edit.buffer.len()) } else { edit.buffer.iter().collect() };
                row(&mut lines, format!("{} {text}▏", term::bold(&format!("{}:", field.label))));
                row(&mut lines, term::dim("enter to save · empty to reset to the default · esc to cancel"));
            }
            None => {
                row(&mut lines, term::dim(field.help));
                row(&mut lines, match status.is_empty() {
                    true => term::dim("↑↓ move · enter/space change · ←→ choose · delete reset · tab save location · esc close"),
                    false => status.clone(),
                });
            }
        }
        lines.push(term::accent(&format!("╰{}╯", "─".repeat(columns - 2))));
        let mut out = String::from("\r");
        if drawn > 0 {
            out.push_str(&format!("\x1b[{drawn}A"));
        }
        out.push_str("\x1b[J");
        out.push_str(&lines.join("\r\n"));
        term::raw(&out);
        drawn = lines.len() - 1;

        let Some(key) = ui.keys.read() else { break };
        if let Some(edit) = editing.as_mut() {
            match key {
                Key::Enter => {
                    let text: String = edit.buffer.iter().collect::<String>().trim().to_string();
                    let value = match (text.is_empty(), &field.kind) {
                        (true, _) => Ok(None),
                        (false, Kind::Number) => text.parse::<u64>().map(|n| Some(json!(n))).map_err(|_| format!("{} must be a number", field.label)),
                        (false, _) => Ok(Some(json!(text))),
                    };
                    status = match value {
                        Ok(value) => save(session, scope, field, value),
                        Err(message) => term::red(&message),
                    };
                    editing = None;
                }
                Key::Esc | Key::CtrlC => editing = None,
                Key::Backspace => {
                    edit.buffer.pop();
                }
                Key::Char(c) => edit.buffer.push(c),
                Key::Paste(text) => edit.buffer.extend(text.trim().chars()),
                Key::KillStart | Key::KillEnd => edit.buffer.clear(),
                _ => {}
            }
            continue;
        }
        status.clear();
        match key {
            Key::Up => selected = (selected + fields.len() - 1) % fields.len(),
            Key::Down => selected = (selected + 1) % fields.len(),
            Key::Tab => {
                scope = match scope {
                    Scope::Local => Scope::Project,
                    Scope::Project => Scope::User,
                    _ => Scope::Local,
                }
            }
            Key::Esc | Key::CtrlC | Key::CtrlD | Key::Char('q') => break,
            Key::Delete | Key::Backspace => {
                status = save(session, scope, field, None);
            }
            Key::Enter | Key::Char(' ') | Key::Right | Key::Left => {
                let backwards = key == Key::Left;
                let current = session.settings.get(field.pointer).cloned();
                match &field.kind {
                    Kind::Switch => {
                        let on = current.and_then(|v| v.as_bool()).unwrap_or(default_text(field, session) == "true");
                        status = save(session, scope, field, Some(json!(!on)));
                    }
                    Kind::Choice(options) => {
                        let now = current.and_then(|v| v.as_str().map(str::to_string)).unwrap_or_else(|| default_text(field, session));
                        let index = options.iter().position(|(v, _)| *v == now).unwrap_or(0);
                        let next = if backwards { (index + options.len() - 1) % options.len() } else { (index + 1) % options.len() };
                        status = save(session, scope, field, Some(json!(options[next].0)));
                    }
                    Kind::Style => {
                        let styles = crate::extensions::styles(&session.settings.project_root);
                        let now = current.and_then(|v| v.as_str().map(str::to_string)).unwrap_or_else(|| "default".into());
                        let index = styles.iter().position(|s| s.name.eq_ignore_ascii_case(&now)).unwrap_or(0);
                        let next = if backwards { (index + styles.len() - 1) % styles.len() } else { (index + 1) % styles.len() };
                        status = save(session, scope, field, Some(json!(styles[next].name)));
                    }
                    Kind::Model if key == Key::Enter || key == Key::Char(' ') => {
                        // The list replaces the panel for a moment.
                        let mut clear = String::from("\r");
                        if drawn > 0 {
                            clear.push_str(&format!("\x1b[{drawn}A"));
                        }
                        term::raw(&(clear + "\x1b[J"));
                        drawn = 0;
                        let mut options: Vec<(String, String)> = match session.config.provider {
                            Provider::Anthropic => ALIASES.iter().map(|(alias, id, about)| (id.to_string(), format!("{alias}   {about}"))).collect(),
                            Provider::OpenAi => crate::llm::list_models(&session.config).unwrap_or_default().into_iter().take(12).map(|id| (id.clone(), id)).collect(),
                        };
                        options.push((String::new(), "Other… (type a model name)".into()));
                        let labels: Vec<String> = options.iter().map(|(_, label)| label.clone()).collect();
                        match ui.choose_public(&format!("Model (now: {})", session.model), &[], "", &labels) {
                            Some(index) if options[index].0.is_empty() => {
                                editing = Some(Editing { buffer: session.model.chars().collect(), secret: false });
                            }
                            Some(index) => status = save(session, scope, field, Some(json!(options[index].0))),
                            None => {}
                        }
                    }
                    Kind::Text | Kind::Number | Kind::Model => {
                        let prefill = current.map(|v| match v {
                            Value::String(text) => text,
                            other => other.to_string(),
                        });
                        editing = Some(Editing { buffer: prefill.unwrap_or_default().chars().collect(), secret: false });
                    }
                    Kind::Secret => editing = Some(Editing { buffer: Vec::new(), secret: true }),
                }
            }
            _ => {}
        }
    }
    let mut clear = String::from("\r");
    if drawn > 0 {
        clear.push_str(&format!("\x1b[{drawn}A"));
    }
    term::raw(&(clear + "\x1b[J"));
    drop(mode);
    term::set_line_start(true);
}

/// Writes one change and brings the session up to date; what to say about it.
fn save(session: &mut Session, scope: Scope, field: &Field, value: Option<Value>) -> String {
    let removing = value.is_none();
    let path = match session.settings.set_value(scope, field.pointer, value) {
        Ok(path) => path,
        Err(message) => return term::red(&message),
    };
    if let Err(message) = session.reload_settings() {
        return term::red(&format!("Saved, but the settings do not work: {message}"));
    }
    match field.pointer {
        "/theme" => term::set_theme(&session.settings.str("/theme").unwrap_or_else(|| "dark".into())),
        "/verbose" => super::terminal::VERBOSE.store(session.settings.bool("/verbose").unwrap_or(false), std::sync::atomic::Ordering::Relaxed),
        "/outputStyle" => {
            let name = session.settings.str("/outputStyle").unwrap_or_else(|| "default".into());
            session.style = crate::extensions::styles(&session.settings.project_root).into_iter().find(|s| s.name.eq_ignore_ascii_case(&name));
        }
        "/model" => {
            if let Some(model) = &session.config.model {
                session.model = model.clone();
            }
        }
        _ => {}
    }
    let file = path.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
    // A higher layer (or the environment) can still win over the file just written.
    let effective = session.settings.source_of(field.pointer);
    let note = match effective {
        Some(winner) if winner != scope && !removing => format!(" (but the {} settings set it too, and win)", winner.name()),
        _ => String::new(),
    };
    term::green(&format!("✓ {} {} {file}{note}", field.label, if removing { "reset in" } else { "saved to" }))
}
