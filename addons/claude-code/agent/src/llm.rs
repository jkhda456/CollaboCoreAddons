//! The two APIs. The conversation is kept in the Messages API's shape (Anthropic content blocks:
//! text, tool_use, tool_result, thinking) and translated for an OpenAI-compatible server.
use std::io::Read;
use std::time::Duration;

use serde_json::{json, Value};

use crate::bridge;
use crate::config::{Config, Provider};
use crate::sse;

#[derive(Debug)]
pub struct Error {
    pub message: String,
    /// The user stopped it (Esc or Ctrl-C).
    pub interrupted: bool,
    /// Worth another try: rate limits, overload, a broken connection before any output.
    pub retry: bool,
    pub retry_after: Option<Duration>,
}

impl Error {
    fn fatal(message: impl Into<String>) -> Error {
        Error { message: message.into(), interrupted: false, retry: false, retry_after: None }
    }
}

impl From<bridge::Error> for Error {
    fn from(error: bridge::Error) -> Error {
        if crate::interrupt::is_set() {
            return Error { message: "interrupted by the user".into(), interrupted: true, retry: false, retry_after: None };
        }
        let retry = matches!(error.kind.as_str(), "network" | "timeout" | "io");
        let mut message = error.to_string();
        if message.contains("allowHostLoopback") {
            message.push_str(
                "\n  (the host makes this request, so localhost is the host computer: start the sandbox with \
                 network.allowHostLoopback: true, or --allow-loopback on the command line)",
            );
        } else if error.kind == "header-not-allowed" {
            message.push_str("\n  (is the claude-code add-on on? it lets the guest send the anthropic-version header)");
        }
        Error { message, interrupted: false, retry, retry_after: None }
    }
}

#[derive(Default, Clone, Copy)]
pub struct Usage {
    pub input: u64,
    pub output: u64,
    pub cache_read: u64,
    pub cache_write: u64,
}

impl Usage {
    pub fn add(&mut self, other: Usage) {
        self.input += other.input;
        self.output += other.output;
        self.cache_read += other.cache_read;
        self.cache_write += other.cache_write;
    }
}

/// One answer from the model: its content blocks (Messages API shape) and why it stopped.
pub struct Turn {
    pub content: Vec<Value>,
    pub stop_reason: String,
    pub usage: Usage,
}

/// Where streamed output goes while it arrives.
pub trait Sink {
    fn text(&mut self, text: &str);
    fn thinking(&mut self, text: &str);
    /// A retry, a fallback: said to the user, not part of the answer.
    fn notice(&mut self, _text: &str) {}
}

/// One call to the model.
pub struct Request<'a> {
    pub model: &'a str,
    pub system: &'a str,
    pub messages: &'a [Value],
    /// Tools in the Messages API's shape; server tools (web search) have a "type" instead of
    /// an input_schema and are left out for OpenAI-compatible servers.
    pub tools: &'a [Value],
    /// Extended thinking with this budget (Anthropic).
    pub thinking: Option<u32>,
}

pub fn complete(config: &Config, request: &Request, sink: &mut dyn Sink) -> Result<Turn, Error> {
    let mut attempt = 0;
    loop {
        attempt += 1;
        let mut wrote = false;
        let mut guard = Guard { sink, wrote: &mut wrote };
        let result = match config.provider {
            Provider::Anthropic => anthropic(config, request, &mut guard),
            Provider::OpenAi => openai(config, request, &mut guard),
        };
        match result {
            Err(error) if error.interrupted || crate::interrupt::is_set() => {
                return Err(Error { message: "interrupted by the user".into(), interrupted: true, retry: false, retry_after: None })
            }
            // Once output has been shown, a retry would repeat it: report instead.
            Err(error) if error.retry && !wrote && attempt < 6 => {
                let wait = error.retry_after.unwrap_or(Duration::from_secs(1 << attempt.min(5))).min(Duration::from_secs(60));
                sink.notice(&format!("{} · retrying in {} s (attempt {attempt}/5)", error.message.lines().next().unwrap_or(""), wait.as_secs()));
                let until = std::time::Instant::now() + wait;
                while std::time::Instant::now() < until {
                    if crate::interrupt::is_set() {
                        return Err(Error { message: "interrupted by the user".into(), interrupted: true, retry: false, retry_after: None });
                    }
                    std::thread::sleep(Duration::from_millis(50));
                }
            }
            other => return other,
        }
    }
}

/// Notes whether anything reached the terminal.
struct Guard<'a> {
    sink: &'a mut dyn Sink,
    wrote: &'a mut bool,
}

impl Sink for Guard<'_> {
    fn text(&mut self, text: &str) {
        *self.wrote = true;
        self.sink.text(text);
    }
    fn thinking(&mut self, text: &str) {
        *self.wrote = true;
        self.sink.thinking(text);
    }
    fn notice(&mut self, text: &str) {
        self.sink.notice(text);
    }
}

fn post(url: &str, headers: &[(&str, &str)], body: &Value) -> Result<bridge::Response, Error> {
    let bytes = serde_json::to_vec(body).expect("request serializes");
    let response = bridge::request("POST", url, headers, &bytes)?;
    if response.status < 400 {
        return Ok(response);
    }
    let status = response.status;
    let retry_after = response.header("retry-after").and_then(|value| value.parse::<u64>().ok()).map(Duration::from_secs);
    let text = response.text().unwrap_or_default();
    // Both APIs: {"error": {"message": …}} (OpenAI-compatible servers sometimes {"error": "…"}).
    let detail = serde_json::from_str::<Value>(&text)
        .ok()
        .and_then(|value| {
            let error = value.get("error")?;
            error.get("message").and_then(Value::as_str).or(error.as_str()).map(str::to_string)
        })
        .unwrap_or_else(|| text.chars().take(500).collect());
    let hint = match status {
        401 | 403 => "\n  (check the API key: `claude --show-config` says whether one is set here; set it with /config set apiKey KEY, OPENAI_API_KEY / ANTHROPIC_API_KEY, --api-key, or the add-on's apiKey when the sandbox starts)",
        _ => "",
    };
    Err(Error {
        interrupted: false,
        message: format!("HTTP {status} from {url}: {detail}{hint}"),
        retry: matches!(status, 408 | 409 | 429 | 500 | 502 | 503 | 504 | 529),
        retry_after,
    })
}

fn stream_error(error: std::io::Error) -> Error {
    let interrupted = crate::interrupt::is_set();
    Error { message: format!("the response broke off: {error}"), interrupted, retry: !interrupted, retry_after: None }
}

// ---- Anthropic Messages API ---------------------------------------------------------------

pub fn anthropic_url(base: &str, path: &str) -> String {
    match base.ends_with("/v1") {
        true => format!("{base}{path}"),
        false => format!("{base}/v1{path}"),
    }
}

/// Cache the stable prefix: tools and system prompt, and the conversation up to its last block.
fn with_cache_breakpoint(messages: &[Value]) -> Vec<Value> {
    let mut messages = messages.to_vec();
    if let Some(block) = messages
        .last_mut()
        .and_then(|message| message.get_mut("content"))
        .and_then(Value::as_array_mut)
        .and_then(|blocks| blocks.last_mut())
        .and_then(Value::as_object_mut)
    {
        block.insert("cache_control".into(), json!({"type": "ephemeral"}));
    }
    messages
}

fn anthropic(config: &Config, request: &Request, sink: &mut dyn Sink) -> Result<Turn, Error> {
    let (model, system, messages, tools) = (request.model, request.system, request.messages, request.tools);
    let mut max_tokens = config.max_tokens.unwrap_or(crate::config::ANTHROPIC_MAX_TOKENS);
    if let Some(budget) = request.thinking {
        max_tokens = max_tokens.max(budget + 4096);
    }
    let mut body = json!({
        "model": model,
        "max_tokens": max_tokens,
        "system": [{"type": "text", "text": system, "cache_control": {"type": "ephemeral"}}],
        "messages": with_cache_breakpoint(messages),
    });
    if let Some(budget) = request.thinking {
        body["thinking"] = json!({"type": "enabled", "budget_tokens": budget.max(1024)});
    }
    if !tools.is_empty() {
        body["tools"] = json!(tools);
    }
    if config.stream {
        body["stream"] = json!(true);
    }
    let mut headers = vec![("content-type", "application/json"), ("anthropic-version", "2023-06-01")];
    if config.stream {
        headers.push(("accept", "text/event-stream"));
    }
    if let Some(key) = &config.api_key {
        headers.push(("x-api-key", key));
    }
    let response = post(&anthropic_url(&config.base_url, "/messages"), &headers, &body)?;

    if !config.stream {
        let message: Value = serde_json::from_str(&response.text().map_err(Error::from)?)
            .map_err(|error| Error::fatal(format!("unreadable answer: {error}")))?;
        let content = message.get("content").and_then(Value::as_array).cloned().unwrap_or_default();
        for block in &content {
            match block.get("type").and_then(Value::as_str) {
                Some("text") => sink.text(block.get("text").and_then(Value::as_str).unwrap_or("")),
                Some("thinking") => sink.thinking(block.get("thinking").and_then(Value::as_str).unwrap_or("")),
                _ => {}
            }
        }
        return Ok(Turn {
            content,
            stop_reason: message.get("stop_reason").and_then(Value::as_str).unwrap_or("end_turn").to_string(),
            usage: anthropic_usage(message.get("usage")),
        });
    }

    let mut blocks: Vec<Value> = Vec::new();
    let mut partial_json: Vec<String> = Vec::new();
    let mut stop_reason = String::from("end_turn");
    let mut usage = Usage::default();
    for event in sse::events(response.body) {
        let event = event.map_err(stream_error)?;
        if event.data.is_empty() {
            continue;
        }
        let data: Value = serde_json::from_str(&event.data).map_err(|error| Error::fatal(format!("unreadable event: {error}")))?;
        match data.get("type").and_then(Value::as_str).unwrap_or(&event.event) {
            "message_start" => usage = anthropic_usage(data.pointer("/message/usage")),
            "content_block_start" => {
                let index = data.get("index").and_then(Value::as_u64).unwrap_or(blocks.len() as u64) as usize;
                let block = data.get("content_block").cloned().unwrap_or_else(|| json!({}));
                while blocks.len() <= index {
                    blocks.push(Value::Null);
                    partial_json.push(String::new());
                }
                blocks[index] = block;
            }
            "content_block_delta" => {
                let index = data.get("index").and_then(Value::as_u64).unwrap_or(0) as usize;
                let (Some(block), Some(delta)) = (blocks.get_mut(index), data.get("delta")) else { continue };
                let piece = |key: &str| delta.get(key).and_then(Value::as_str).unwrap_or("").to_string();
                match delta.get("type").and_then(Value::as_str).unwrap_or("") {
                    "text_delta" => {
                        let text = piece("text");
                        sink.text(&text);
                        append(block, "text", &text);
                    }
                    "thinking_delta" => {
                        let text = piece("thinking");
                        sink.thinking(&text);
                        append(block, "thinking", &text);
                    }
                    "signature_delta" => block["signature"] = json!(piece("signature")),
                    "input_json_delta" => partial_json[index].push_str(&piece("partial_json")),
                    _ => {}
                }
            }
            "content_block_stop" => {
                let index = data.get("index").and_then(Value::as_u64).unwrap_or(0) as usize;
                if let Some(block) = blocks.get_mut(index) {
                    if matches!(block.get("type").and_then(Value::as_str), Some("tool_use" | "server_tool_use")) {
                        let raw = &partial_json[index];
                        block["input"] = match raw.trim().is_empty() {
                            true => json!({}),
                            false => serde_json::from_str(raw).unwrap_or_else(|_| json!({"__invalid_arguments": raw})),
                        };
                    }
                }
            }
            "message_delta" => {
                if let Some(reason) = data.pointer("/delta/stop_reason").and_then(Value::as_str) {
                    stop_reason = reason.to_string();
                }
                if let Some(output) = data.pointer("/usage/output_tokens").and_then(Value::as_u64) {
                    usage.output = output;
                }
            }
            "error" => {
                let message = data.pointer("/error/message").and_then(Value::as_str).unwrap_or(&event.data).to_string();
                let overloaded = data.pointer("/error/type").and_then(Value::as_str) == Some("overloaded_error");
                return Err(Error { message, interrupted: false, retry: overloaded, retry_after: None });
            }
            "message_stop" => break,
            _ => {}
        }
    }
    Ok(Turn { content: blocks.into_iter().filter(|block| !block.is_null()).collect(), stop_reason, usage })
}

fn append(block: &mut Value, key: &str, text: &str) {
    let mut current = block.get(key).and_then(Value::as_str).unwrap_or("").to_string();
    current.push_str(text);
    block[key] = json!(current);
}

fn anthropic_usage(usage: Option<&Value>) -> Usage {
    let number = |key: &str| usage.and_then(|usage| usage.get(key)).and_then(Value::as_u64).unwrap_or(0);
    Usage {
        input: number("input_tokens"),
        output: number("output_tokens"),
        cache_read: number("cache_read_input_tokens"),
        cache_write: number("cache_creation_input_tokens"),
    }
}

// ---- OpenAI-compatible Chat Completions -----------------------------------------------------

/// Tool results' text, whether the content is a string or blocks.
fn flatten(content: Option<&Value>) -> String {
    match content {
        Some(Value::String(text)) => text.clone(),
        Some(Value::Array(blocks)) => blocks
            .iter()
            .filter_map(|block| block.get("text").and_then(Value::as_str))
            .collect::<Vec<_>>()
            .join("\n"),
        _ => String::new(),
    }
}

/// The conversation as chat messages: system first, tool results as role "tool" right after
/// the assistant message that called them, thinking left out.
pub fn to_openai_messages(system: &str, messages: &[Value]) -> Vec<Value> {
    let mut out = vec![json!({"role": "system", "content": system})];
    for message in messages {
        let role = message.get("role").and_then(Value::as_str).unwrap_or("user");
        let blocks: Vec<Value> = match message.get("content") {
            Some(Value::String(text)) => vec![json!({"type": "text", "text": text})],
            Some(Value::Array(blocks)) => blocks.clone(),
            _ => Vec::new(),
        };
        let texts: Vec<&str> = blocks
            .iter()
            .filter(|block| block.get("type").and_then(Value::as_str) == Some("text"))
            .filter_map(|block| block.get("text").and_then(Value::as_str))
            .collect();
        let text = texts.join("\n");
        if role == "assistant" {
            let calls: Vec<Value> = blocks
                .iter()
                .filter(|block| block.get("type").and_then(Value::as_str) == Some("tool_use"))
                .map(|block| {
                    json!({
                        "id": block.get("id").cloned().unwrap_or(json!("")),
                        "type": "function",
                        "function": {
                            "name": block.get("name").cloned().unwrap_or(json!("")),
                            "arguments": serde_json::to_string(block.get("input").unwrap_or(&json!({}))).unwrap(),
                        },
                    })
                })
                .collect();
            let mut entry = json!({"role": "assistant", "content": text});
            if !calls.is_empty() {
                entry["tool_calls"] = json!(calls);
            }
            out.push(entry);
        } else {
            for block in blocks.iter().filter(|block| block.get("type").and_then(Value::as_str) == Some("tool_result")) {
                let mut content = flatten(block.get("content"));
                if block.get("is_error").and_then(Value::as_bool) == Some(true) {
                    content = format!("Error: {content}");
                }
                out.push(json!({"role": "tool", "tool_call_id": block.get("tool_use_id").cloned().unwrap_or(json!("")), "content": content}));
            }
            if !text.is_empty() {
                out.push(json!({"role": "user", "content": text}));
            }
        }
    }
    out
}

fn to_openai_tools(tools: &[Value]) -> Vec<Value> {
    tools
        .iter()
        .filter(|tool| tool.get("input_schema").is_some())
        .map(|tool| {
            json!({"type": "function", "function": {
                "name": tool["name"], "description": tool["description"], "parameters": tool["input_schema"],
            }})
        })
        .collect()
}

/// A tool call as it arrives in pieces.
#[derive(Default)]
struct Call {
    id: String,
    name: String,
    arguments: String,
}

fn openai_content(text: String, calls: Vec<Call>) -> Vec<Value> {
    let mut content = Vec::new();
    if !text.is_empty() {
        content.push(json!({"type": "text", "text": text}));
    }
    for (index, call) in calls.into_iter().enumerate() {
        if call.name.is_empty() {
            continue;
        }
        // Some servers send no id, or the same one each time.
        let id = match call.id.is_empty() {
            true => format!("call_{index}_{}", std::process::id()),
            false => call.id,
        };
        let input = match call.arguments.trim() {
            "" => json!({}),
            raw => serde_json::from_str::<Value>(raw)
                .ok()
                .filter(Value::is_object)
                .unwrap_or_else(|| json!({"__invalid_arguments": raw})),
        };
        content.push(json!({"type": "tool_use", "id": id, "name": call.name, "input": input}));
    }
    content
}

fn openai_stop(reason: &str) -> String {
    match reason {
        "tool_calls" | "function_call" => "tool_use",
        "length" => "max_tokens",
        _ => "end_turn",
    }
    .to_string()
}

fn openai(config: &Config, request: &Request, sink: &mut dyn Sink) -> Result<Turn, Error> {
    let (model, system, messages) = (request.model, request.system, request.messages);
    let tools = to_openai_tools(request.tools);
    let mut body = json!({"model": model, "messages": to_openai_messages(system, messages)});
    if !tools.is_empty() {
        body["tools"] = json!(tools);
    }
    if let Some(max) = config.max_tokens {
        body["max_tokens"] = json!(max);
    }
    if config.stream {
        body["stream"] = json!(true);
    }
    let bearer = config.api_key.as_ref().map(|key| format!("Bearer {key}"));
    let mut headers = vec![("content-type", "application/json")];
    if config.stream {
        headers.push(("accept", "text/event-stream"));
    }
    if let Some(bearer) = &bearer {
        headers.push(("authorization", bearer));
    }
    let response = post(&format!("{}/chat/completions", config.base_url), &headers, &body)?;

    if !config.stream {
        let answer: Value = serde_json::from_str(&response.text().map_err(Error::from)?)
            .map_err(|error| Error::fatal(format!("unreadable answer: {error}")))?;
        let message = answer.pointer("/choices/0/message").cloned().unwrap_or_else(|| json!({}));
        if let Some(reasoning) = message.get("reasoning_content").or(message.get("reasoning")).and_then(Value::as_str) {
            sink.thinking(reasoning);
        }
        let text = message.get("content").and_then(Value::as_str).unwrap_or("").to_string();
        sink.text(&text);
        let calls = message
            .get("tool_calls")
            .and_then(Value::as_array)
            .map(|calls| {
                calls
                    .iter()
                    .map(|call| Call {
                        id: call.get("id").and_then(Value::as_str).unwrap_or("").into(),
                        name: call.pointer("/function/name").and_then(Value::as_str).unwrap_or("").into(),
                        arguments: match call.pointer("/function/arguments") {
                            Some(Value::String(text)) => text.clone(),
                            Some(other) => other.to_string(),
                            None => String::new(),
                        },
                    })
                    .collect()
            })
            .unwrap_or_default();
        return Ok(Turn {
            content: openai_content(text, calls),
            stop_reason: openai_stop(answer.pointer("/choices/0/finish_reason").and_then(Value::as_str).unwrap_or("")),
            usage: openai_usage(answer.get("usage")),
        });
    }

    let (mut text, mut calls, mut finish, mut usage) = (String::new(), Vec::<Call>::new(), String::new(), Usage::default());
    for event in sse::events(response.body) {
        let event = event.map_err(stream_error)?;
        if event.data.is_empty() {
            continue;
        }
        if event.data.trim() == "[DONE]" {
            break;
        }
        let chunk: Value = serde_json::from_str(&event.data).map_err(|error| Error::fatal(format!("unreadable event: {error}")))?;
        if let Some(error) = chunk.get("error") {
            let message = error.get("message").and_then(Value::as_str).map(str::to_string).unwrap_or_else(|| error.to_string());
            return Err(Error::fatal(message));
        }
        if chunk.get("usage").is_some_and(Value::is_object) {
            usage = openai_usage(chunk.get("usage"));
        }
        let Some(choice) = chunk.pointer("/choices/0") else { continue };
        if let Some(reason) = choice.get("finish_reason").and_then(Value::as_str) {
            finish = reason.to_string();
        }
        let Some(delta) = choice.get("delta") else { continue };
        if let Some(reasoning) = delta.get("reasoning_content").or(delta.get("reasoning")).and_then(Value::as_str) {
            sink.thinking(reasoning);
        }
        if let Some(piece) = delta.get("content").and_then(Value::as_str) {
            sink.text(piece);
            text.push_str(piece);
        }
        for call in delta.get("tool_calls").and_then(Value::as_array).into_iter().flatten() {
            let index = call.get("index").and_then(Value::as_u64).map(|i| i as usize).unwrap_or(calls.len().saturating_sub(1));
            while calls.len() <= index {
                calls.push(Call::default());
            }
            let entry = &mut calls[index];
            if let Some(id) = call.get("id").and_then(Value::as_str) {
                entry.id = id.to_string();
            }
            if let Some(name) = call.pointer("/function/name").and_then(Value::as_str) {
                entry.name.push_str(name);
            }
            match call.pointer("/function/arguments") {
                Some(Value::String(piece)) => entry.arguments.push_str(piece),
                Some(Value::Object(_)) => entry.arguments = call.pointer("/function/arguments").unwrap().to_string(),
                _ => {}
            }
        }
    }
    Ok(Turn { content: openai_content(text, calls), stop_reason: openai_stop(&finish), usage })
}

fn openai_usage(usage: Option<&Value>) -> Usage {
    let number = |key: &str| usage.and_then(|usage| usage.get(key)).and_then(Value::as_u64).unwrap_or(0);
    Usage {
        input: number("prompt_tokens"),
        output: number("completion_tokens"),
        cache_read: usage.and_then(|usage| usage.pointer("/prompt_tokens_details/cached_tokens")).and_then(Value::as_u64).unwrap_or(0),
        cache_write: 0,
    }
}

/// The models an OpenAI-compatible server offers (GET /models), in its order.
pub fn list_models(config: &Config) -> Result<Vec<String>, Error> {
    let bearer = config.api_key.as_ref().map(|key| format!("Bearer {key}"));
    let mut headers = vec![("accept", "application/json")];
    if let Some(bearer) = &bearer {
        headers.push(("authorization", bearer));
    }
    let url = match config.provider {
        Provider::Anthropic => anthropic_url(&config.base_url, "/models"),
        Provider::OpenAi => format!("{}/models", config.base_url),
    };
    if config.provider == Provider::Anthropic {
        headers.push(("anthropic-version", "2023-06-01"));
        if let Some(key) = &config.api_key {
            headers.push(("x-api-key", key));
        }
    }
    let response = bridge::request("GET", &url, &headers, &[])?;
    let status = response.status;
    let mut body = String::new();
    response.body.take(8 * 1024 * 1024).read_to_string(&mut body).map_err(|error| Error::fatal(error.to_string()))?;
    if status >= 400 {
        return Err(Error::fatal(format!("HTTP {status} from {url}: {}", body.chars().take(300).collect::<String>())));
    }
    let value: Value = serde_json::from_str(&body).map_err(|error| Error::fatal(format!("unreadable model list: {error}")))?;
    // OpenAI: {"data": [{"id"}]}; Ollama's native list has {"models": [{"name"}]}.
    let list = value.get("data").or(value.get("models")).and_then(Value::as_array).cloned().unwrap_or_default();
    Ok(list
        .iter()
        .filter_map(|model| model.get("id").or(model.get("name")).and_then(Value::as_str).map(str::to_string))
        .collect())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn conversation_to_openai() {
        let messages = vec![
            json!({"role": "user", "content": [{"type": "text", "text": "list files"}]}),
            json!({"role": "assistant", "content": [
                {"type": "thinking", "thinking": "hm", "signature": "s"},
                {"type": "text", "text": "Sure."},
                {"type": "tool_use", "id": "t1", "name": "Bash", "input": {"command": "ls"}},
            ]}),
            json!({"role": "user", "content": [
                {"type": "tool_result", "tool_use_id": "t1", "content": "a\nb"},
                {"type": "text", "text": "thanks"},
            ]}),
        ];
        let out = to_openai_messages("sys", &messages);
        assert_eq!(out[0], json!({"role": "system", "content": "sys"}));
        assert_eq!(out[1], json!({"role": "user", "content": "list files"}));
        assert_eq!(out[2]["content"], json!("Sure."));
        assert_eq!(out[2]["tool_calls"][0]["function"], json!({"name": "Bash", "arguments": "{\"command\":\"ls\"}"}));
        assert_eq!(out[3], json!({"role": "tool", "tool_call_id": "t1", "content": "a\nb"}));
        assert_eq!(out[4], json!({"role": "user", "content": "thanks"}));
    }

    #[test]
    fn calls_become_tool_use_blocks() {
        let calls = vec![
            Call { id: "c1".into(), name: "Read".into(), arguments: "{\"file_path\":\"/a\"}".into() },
            Call { id: "".into(), name: "Bash".into(), arguments: "not json".into() },
        ];
        let content = openai_content("hi".into(), calls);
        assert_eq!(content[0], json!({"type": "text", "text": "hi"}));
        assert_eq!(content[1]["input"], json!({"file_path": "/a"}));
        assert_eq!(content[2]["input"], json!({"__invalid_arguments": "not json"}));
        assert!(content[2]["id"].as_str().unwrap().starts_with("call_1_"));
    }

    #[test]
    fn anthropic_urls() {
        assert_eq!(anthropic_url("https://api.anthropic.com", "/messages"), "https://api.anthropic.com/v1/messages");
        assert_eq!(anthropic_url("http://gw/v1", "/messages"), "http://gw/v1/messages");
    }
}
