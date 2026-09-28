//! -p: one prompt, no terminal UI. The answer goes to stdout as text, as one JSON object, or as
//! a stream of JSON lines (stream-json: every message as it happens, then the result). Tools that
//! need a yes are refused, as nobody is there to say it (allow them with --allowedTools,
//! settings or --permission-mode).
use serde_json::{json, Value};

use crate::agent::{Approval, PlanAnswer, Ui};
use crate::tools::{self, Outcome};

#[derive(PartialEq, Clone, Copy)]
pub enum Format {
    Text,
    Json,
    StreamJson,
}

pub struct PrintUi {
    pub format: Format,
    pub verbose: bool,
    pub session_id: String,
    pub denied: Vec<Value>,
}

impl PrintUi {
    fn log(&self, line: &str) {
        if self.verbose && self.format == Format::Text {
            eprintln!("{line}");
        }
    }
}

impl Ui for PrintUi {
    fn text(&mut self, _text: &str) {}

    fn thinking(&mut self, text: &str) {
        if self.verbose && self.format == Format::Text {
            eprint!("\x1b[2m{text}\x1b[0m");
        }
    }

    fn notice(&mut self, text: &str) {
        match self.format {
            Format::StreamJson => println!("{}", json!({"type": "system", "subtype": "notice", "message": text, "session_id": self.session_id})),
            _ => self.log(&format!("  ⎿  {text}")),
        }
    }

    fn error(&mut self, text: &str) {
        eprintln!("claude: {text}");
    }

    fn tool_call(&mut self, name: &str, _input: &Value, summary: &str) {
        self.log(&format!("⏺ {}({summary})", tools::display_name(name)));
    }

    fn tool_result(&mut self, _name: &str, _input: &Value, outcome: &Outcome) {
        let first = outcome.text.lines().next().unwrap_or("");
        self.log(&format!("  ⎿  {}{first}", if outcome.is_error { "Error: " } else { "" }));
    }

    fn ask(&mut self, name: &str, input: &Value, _summary: &str, _suggestion: &str) -> Approval {
        self.denied.push(json!({"tool_name": name, "tool_input": input}));
        self.log(&format!("  ⎿  {name} needs permission, and -p cannot ask: refused"));
        Approval::No(Some(format!(
            "(Running without a user to ask: {name} was not allowed. Allow it with --allowedTools, the settings' permissions, or --permission-mode.)"
        )))
    }

    fn approve_plan(&mut self, _plan: &str) -> PlanAnswer {
        PlanAnswer::KeepPlanning(Some("(Running without a user to approve the plan: present it as your answer.)".into()))
    }

    fn message(&mut self, message: &Value) {
        if self.format == Format::StreamJson {
            let kind = message["role"].as_str().unwrap_or("user");
            println!("{}", json!({"type": kind, "message": message, "session_id": self.session_id}));
        }
    }

    fn sub_step(&mut self, agent: &str, line: &str) {
        self.log(&format!("  ⎿  [{agent}] {line}"));
    }
}
