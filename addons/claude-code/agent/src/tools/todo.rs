//! TodoWrite: the task list the model keeps and the user sees.
use serde_json::Value;

use super::{fail, Context, Display, Outcome};

#[derive(Clone, Debug, PartialEq)]
pub struct Todo {
    pub content: String,
    pub status: String,
    pub active_form: String,
}

pub fn write(input: &Value, context: &mut Context) -> Outcome {
    let Some(items) = input.get("todos").and_then(Value::as_array) else { return fail("InputValidationError: todos must be an array") };
    let mut todos = Vec::new();
    for (index, item) in items.iter().enumerate() {
        let field = |key: &str| item.get(key).and_then(Value::as_str).unwrap_or("").trim().to_string();
        let (content, status) = (field("content"), field("status"));
        if content.is_empty() || !matches!(status.as_str(), "pending" | "in_progress" | "completed") {
            return fail(format!("InputValidationError: todo {} needs content and a status of pending, in_progress or completed", index + 1));
        }
        let active_form = Some(field("activeForm")).filter(|form| !form.is_empty()).unwrap_or_else(|| content.clone());
        todos.push(Todo { content, status, active_form });
    }
    context.todos = todos.clone();
    Outcome {
        text: "Todos have been modified successfully. Ensure that you continue to use the todo list to track your progress. Please proceed with the current tasks if applicable".into(),
        display: Some(Display::Todos(todos)),
        ..Default::default()
    }
}

/// The task being worked on, as the spinner says it.
pub fn active(todos: &[Todo]) -> Option<&str> {
    todos.iter().find(|todo| todo.status == "in_progress").map(|todo| todo.active_form.as_str())
}
