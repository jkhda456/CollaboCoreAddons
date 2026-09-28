//! MCP (Model Context Protocol) servers: their tools, as mcp__<server>__<tool>.
//!
//! Servers come from .mcp.json in the project, the settings' mcpServers and --mcp-config, as
//!   {"mcpServers": {"name": {"command": "…", "args": […], "env": {…}}}}              (stdio)
//!   {"mcpServers": {"name": {"type": "http", "url": "https://…", "headers": {…}}}}   (streamable HTTP)
//! ${VAR} and ${VAR:-default} in them are taken from the environment. A stdio server is a child
//! process speaking JSON-RPC on its stdin/stdout; an HTTP one is reached through the host's
//! request API, under the sandbox's network policy.
use std::io::{BufRead, BufReader, Write};
use std::os::fd::AsRawFd;
use std::process::{Child, ChildStdin, ChildStdout, Command, Stdio};
use std::time::{Duration, Instant};

use serde_json::{json, Map, Value};

use crate::bridge;
use crate::tools::{fail, Outcome};

const PROTOCOL_VERSION: &str = "2025-06-18";
const CALL_TIMEOUT: Duration = Duration::from_secs(600);
const START_TIMEOUT: Duration = Duration::from_secs(30);

enum Transport {
    Stdio { child: Child, stdin: ChildStdin, stdout: BufReader<ChildStdout> },
    Http { url: String, headers: Vec<(String, String)>, session: Option<String> },
}

pub struct Server {
    pub name: String,
    pub scope: String,
    /// "connected", or why not.
    pub status: String,
    pub tools: Vec<Value>,
    pub instructions: Option<String>,
    transport: Option<Transport>,
    next_id: u64,
}

#[derive(Default)]
pub struct Mcp {
    pub servers: Vec<Server>,
}

/// ${VAR} and ${VAR:-default}.
pub fn expand(text: &str) -> String {
    let pattern = regex::Regex::new(r"\$\{([A-Za-z_][A-Za-z0-9_]*)(?::-([^}]*))?\}").unwrap();
    pattern
        .replace_all(text, |caps: &regex::Captures| {
            std::env::var(&caps[1]).ok().filter(|v| !v.is_empty()).unwrap_or_else(|| caps.get(2).map_or(String::new(), |d| d.as_str().to_string()))
        })
        .into_owned()
}

fn expand_value(value: &Value) -> Value {
    match value {
        Value::String(text) => json!(expand(text)),
        Value::Array(items) => Value::Array(items.iter().map(expand_value).collect()),
        Value::Object(map) => Value::Object(map.iter().map(|(k, v)| (k.clone(), expand_value(v))).collect()),
        other => other.clone(),
    }
}

/// Names as the Messages API allows them: letters, digits, _ and -.
pub fn sanitize(name: &str) -> String {
    name.chars().map(|c| if c.is_ascii_alphanumeric() || c == '_' || c == '-' { c } else { '_' }).collect()
}

/// Every configured server: (name, scope, config), later sources winning.
pub fn configured(settings: &crate::settings::Settings, cli: &[String]) -> Vec<(String, String, Value)> {
    let mut servers: Vec<(String, String, Value)> = Vec::new();
    let mut add = |scope: &str, map: Option<&Map<String, Value>>| {
        for (name, config) in map.into_iter().flatten() {
            servers.retain(|(existing, _, _)| existing != name);
            servers.push((name.clone(), scope.to_string(), config.clone()));
        }
    };
    add("user", settings.layers.iter().find(|l| l.scope == crate::settings::Scope::User).and_then(|l| l.value.get("mcpServers")).and_then(Value::as_object));
    let project_file = settings.project_root.join(".mcp.json");
    if let Ok(text) = std::fs::read_to_string(&project_file) {
        if let Ok(value) = serde_json::from_str::<Value>(&text) {
            add("project", value.get("mcpServers").and_then(Value::as_object));
        }
    }
    for layer in &settings.layers {
        if !matches!(layer.scope, crate::settings::Scope::User) {
            add(layer.scope.name(), layer.value.get("mcpServers").and_then(Value::as_object));
        }
    }
    for source in cli {
        let text = match source.trim_start().starts_with('{') {
            true => source.clone(),
            false => std::fs::read_to_string(source).unwrap_or_default(),
        };
        if let Ok(value) = serde_json::from_str::<Value>(&text) {
            add("cli", value.get("mcpServers").and_then(Value::as_object));
        }
    }
    servers
}

fn content_to_outcome(result: &Value) -> Outcome {
    let is_error = result.get("isError").and_then(Value::as_bool).unwrap_or(false);
    let mut texts = Vec::new();
    let mut blocks = Vec::new();
    for item in result.get("content").and_then(Value::as_array).into_iter().flatten() {
        match item.get("type").and_then(Value::as_str) {
            Some("text") => {
                let text = item.get("text").and_then(Value::as_str).unwrap_or("");
                texts.push(text.to_string());
                blocks.push(json!({"type": "text", "text": text}));
            }
            Some("image") => {
                texts.push("[image]".into());
                blocks.push(json!({"type": "image", "source": {"type": "base64", "media_type": item["mimeType"], "data": item["data"]}}));
            }
            Some("resource") => {
                let text = item.pointer("/resource/text").and_then(Value::as_str).unwrap_or("[resource]");
                texts.push(text.to_string());
                blocks.push(json!({"type": "text", "text": text}));
            }
            _ => texts.push(item.to_string()),
        }
    }
    if texts.is_empty() {
        if let Some(structured) = result.get("structuredContent") {
            texts.push(structured.to_string());
        }
    }
    let text = crate::tools::shorten(&texts.join("\n"), 50_000);
    let has_image = blocks.iter().any(|b| b["type"] == "image");
    Outcome { text, is_error, content: has_image.then(|| Value::Array(blocks)), ..Default::default() }
}

impl Server {
    fn start(name: &str, scope: &str, config: &Value) -> Server {
        let mut server = Server { name: name.to_string(), scope: scope.to_string(), status: "connecting".into(), tools: Vec::new(), instructions: None, transport: None, next_id: 0 };
        let config = expand_value(config);
        let kind = config.get("type").and_then(Value::as_str).unwrap_or(if config.get("url").is_some() { "http" } else { "stdio" });
        let transport = match kind {
            "stdio" => {
                let Some(command) = config.get("command").and_then(Value::as_str) else {
                    server.status = "failed: no command".into();
                    return server;
                };
                let args: Vec<String> = config.get("args").and_then(Value::as_array).into_iter().flatten().filter_map(Value::as_str).map(str::to_string).collect();
                let mut cmd = Command::new(command);
                cmd.args(&args).stdin(Stdio::piped()).stdout(Stdio::piped()).stderr(Stdio::null());
                for (key, value) in config.get("env").and_then(Value::as_object).into_iter().flatten() {
                    cmd.env(key, value.as_str().map(str::to_string).unwrap_or_else(|| value.to_string()));
                }
                match cmd.spawn() {
                    Ok(mut child) => {
                        let (stdin, stdout) = (child.stdin.take().unwrap(), child.stdout.take().unwrap());
                        Transport::Stdio { child, stdin, stdout: BufReader::new(stdout) }
                    }
                    Err(error) => {
                        server.status = format!("failed: cannot start {command}: {error}");
                        return server;
                    }
                }
            }
            "http" | "streamable-http" | "sse" => {
                let Some(url) = config.get("url").and_then(Value::as_str) else {
                    server.status = "failed: no url".into();
                    return server;
                };
                let headers = config
                    .get("headers")
                    .and_then(Value::as_object)
                    .map(|map| map.iter().map(|(k, v)| (k.to_lowercase(), v.as_str().unwrap_or("").to_string())).collect())
                    .unwrap_or_default();
                Transport::Http { url: url.to_string(), headers, session: None }
            }
            other => {
                server.status = format!("failed: unknown transport {other}");
                return server;
            }
        };
        server.transport = Some(transport);
        let init = server.request(
            "initialize",
            json!({"protocolVersion": PROTOCOL_VERSION, "capabilities": {}, "clientInfo": {"name": "claude-code-collabo", "version": env!("CARGO_PKG_VERSION")}}),
            START_TIMEOUT,
        );
        match init {
            Ok(result) => {
                server.instructions = result.get("instructions").and_then(Value::as_str).map(str::to_string);
                server.notify("notifications/initialized", json!({}));
            }
            Err(error) => {
                server.status = format!("failed: {error}");
                server.stop();
                return server;
            }
        }
        let mut cursor: Option<String> = None;
        loop {
            let params = match &cursor {
                Some(cursor) => json!({"cursor": cursor}),
                None => json!({}),
            };
            match server.request("tools/list", params, START_TIMEOUT) {
                Ok(result) => {
                    server.tools.extend(result.get("tools").and_then(Value::as_array).cloned().unwrap_or_default());
                    cursor = result.get("nextCursor").and_then(Value::as_str).map(str::to_string);
                    if cursor.is_none() {
                        break;
                    }
                }
                Err(error) => {
                    server.status = format!("failed: tools/list: {error}");
                    return server;
                }
            }
        }
        server.status = "connected".into();
        server
    }

    fn stop(&mut self) {
        if let Some(Transport::Stdio { mut child, .. }) = self.transport.take() {
            let _ = child.kill();
            let _ = child.wait();
        }
    }

    fn notify(&mut self, method: &str, params: Value) {
        let message = json!({"jsonrpc": "2.0", "method": method, "params": params});
        match &mut self.transport {
            Some(Transport::Stdio { stdin, .. }) => {
                let _ = writeln!(stdin, "{message}");
                let _ = stdin.flush();
            }
            Some(Transport::Http { url, headers, session }) => {
                let _ = http_post(url, headers, session, &message);
            }
            None => {}
        }
    }

    fn request(&mut self, method: &str, params: Value, timeout: Duration) -> Result<Value, String> {
        self.next_id += 1;
        let id = self.next_id;
        let message = json!({"jsonrpc": "2.0", "id": id, "method": method, "params": params});
        let reply = match &mut self.transport {
            None => return Err("not connected".into()),
            Some(Transport::Http { url, headers, session }) => http_post(url, headers, session, &message)?.ok_or("no answer")?,
            Some(Transport::Stdio { child, stdin, stdout }) => {
                writeln!(stdin, "{message}").and_then(|_| stdin.flush()).map_err(|error| format!("the server's stdin: {error}"))?;
                let deadline = Instant::now() + timeout;
                loop {
                    let line = read_line(stdout, deadline).map_err(|error| {
                        match child.try_wait() {
                            Ok(Some(status)) => format!("the server exited ({status})"),
                            _ => error,
                        }
                    })?;
                    let Ok(incoming) = serde_json::from_str::<Value>(&line) else { continue };
                    if incoming.get("id").and_then(Value::as_u64) == Some(id) && incoming.get("method").is_none() {
                        break incoming;
                    }
                    // A request from the server (ping, roots/list, …): answered so it does not wait.
                    if let (Some(their_id), Some(method)) = (incoming.get("id"), incoming.get("method").and_then(Value::as_str)) {
                        let answer = match method {
                            "ping" => json!({"jsonrpc": "2.0", "id": their_id, "result": {}}),
                            "roots/list" => json!({"jsonrpc": "2.0", "id": their_id, "result": {"roots": []}}),
                            _ => json!({"jsonrpc": "2.0", "id": their_id, "error": {"code": -32601, "message": "not supported"}}),
                        };
                        let _ = writeln!(stdin, "{answer}");
                        let _ = stdin.flush();
                    }
                }
            }
        };
        if let Some(error) = reply.get("error") {
            return Err(error.get("message").and_then(Value::as_str).map(str::to_string).unwrap_or_else(|| error.to_string()));
        }
        Ok(reply.get("result").cloned().unwrap_or(Value::Null))
    }
}

/// A line from a stdio server, waiting at most until `deadline` (or the user's interrupt).
fn read_line(reader: &mut BufReader<ChildStdout>, deadline: Instant) -> Result<String, String> {
    loop {
        if reader.buffer().is_empty() {
            let left = deadline.saturating_duration_since(Instant::now());
            if left.is_zero() {
                return Err("timed out".into());
            }
            let mut poll = libc::pollfd { fd: reader.get_ref().as_raw_fd(), events: libc::POLLIN, revents: 0 };
            // SAFETY: one pollfd for the server's stdout.
            let ready = unsafe { libc::poll(&mut poll, 1, left.as_millis().min(500) as i32) };
            if crate::interrupt::is_set() {
                return Err("interrupted by the user".into());
            }
            if ready <= 0 {
                continue;
            }
        }
        let mut line = String::new();
        match reader.read_line(&mut line) {
            Ok(0) => return Err("the server closed its output".into()),
            Ok(_) if line.trim().is_empty() => continue,
            Ok(_) => return Ok(line),
            Err(error) if error.kind() == std::io::ErrorKind::Interrupted => continue,
            Err(error) => return Err(error.to_string()),
        }
    }
}

/// One JSON-RPC message to a streamable-HTTP server; its answer (JSON or an SSE stream).
fn http_post(url: &str, headers: &[(String, String)], session: &mut Option<String>, message: &Value) -> Result<Option<Value>, String> {
    let mut all: Vec<(&str, &str)> = vec![("content-type", "application/json"), ("accept", "application/json, text/event-stream"), ("mcp-protocol-version", PROTOCOL_VERSION)];
    if let Some(id) = session.as_deref() {
        all.push(("mcp-session-id", id));
    }
    all.extend(headers.iter().map(|(k, v)| (k.as_str(), v.as_str())));
    let body = serde_json::to_vec(message).expect("message serializes");
    let response = bridge::request("POST", url, &all, &body).map_err(|error| error.to_string())?;
    if let Some(id) = response.header("mcp-session-id") {
        *session = Some(id.to_string());
    }
    let status = response.status;
    let kind = response.header("content-type").unwrap_or("").to_string();
    if status == 202 || message.get("id").is_none() {
        return Ok(None);
    }
    if status >= 400 {
        return Err(format!("HTTP {status}: {}", response.text().unwrap_or_default().chars().take(300).collect::<String>()));
    }
    if kind.contains("text/event-stream") {
        let want = message.get("id").cloned();
        for event in crate::sse::events(response.body) {
            let event = event.map_err(|error| error.to_string())?;
            if let Ok(value) = serde_json::from_str::<Value>(&event.data) {
                if value.get("id") == want.as_ref() && value.get("method").is_none() {
                    return Ok(Some(value));
                }
            }
        }
        return Err("the stream ended without an answer".into());
    }
    let text = response.text().map_err(|error| error.to_string())?;
    serde_json::from_str(&text).map(Some).map_err(|error| format!("unreadable answer: {error}"))
}

impl Mcp {
    pub fn connect(servers: &[(String, String, Value)]) -> Mcp {
        Mcp { servers: servers.iter().map(|(name, scope, config)| Server::start(name, scope, config)).collect() }
    }

    /// The servers' tools in the Messages API's shape.
    pub fn specs(&self) -> Vec<Value> {
        let mut specs = Vec::new();
        for server in self.servers.iter().filter(|s| s.status == "connected") {
            for tool in &server.tools {
                let Some(name) = tool.get("name").and_then(Value::as_str) else { continue };
                let full: String = format!("mcp__{}__{}", sanitize(&server.name), sanitize(name)).chars().take(64).collect();
                let schema = tool.get("inputSchema").cloned().filter(Value::is_object).unwrap_or_else(|| json!({"type": "object", "properties": {}}));
                specs.push(json!({"name": full, "description": tool.get("description").and_then(Value::as_str).unwrap_or(""), "input_schema": schema}));
            }
        }
        specs
    }

    pub fn call(&mut self, full_name: &str, arguments: &Value) -> Outcome {
        let rest = full_name.strip_prefix("mcp__").unwrap_or(full_name);
        for server in self.servers.iter_mut() {
            let prefix = format!("{}__", sanitize(&server.name));
            let Some(tool_part) = rest.strip_prefix(&prefix) else { continue };
            let Some(tool) = server.tools.iter().filter_map(|t| t.get("name").and_then(Value::as_str)).find(|name| sanitize(name).chars().take(64).collect::<String>() == tool_part || sanitize(name) == tool_part).map(str::to_string) else {
                continue;
            };
            return match server.request("tools/call", json!({"name": tool, "arguments": arguments}), CALL_TIMEOUT) {
                Ok(result) => content_to_outcome(&result),
                Err(error) => fail(format!("MCP server {} failed: {error}", server.name)),
            };
        }
        fail(format!("No such tool available: {full_name}"))
    }

    pub fn instructions(&self) -> Vec<(String, String)> {
        self.servers.iter().filter_map(|s| Some((s.name.clone(), s.instructions.clone()?))).collect()
    }
}

impl Drop for Mcp {
    fn drop(&mut self) {
        for server in self.servers.iter_mut() {
            server.stop();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_stdio_server() {
        // A tiny MCP server in sh: answers initialize, tools/list and tools/call by id.
        let script = r#"
while IFS= read -r line; do
  id=$(echo "$line" | sed -n 's/.*"id":\([0-9]*\).*/\1/p')
  case "$line" in
    *'"initialize"'*) echo '{"jsonrpc":"2.0","method":"notifications/message","params":{}}'; echo "{\"jsonrpc\":\"2.0\",\"id\":$id,\"result\":{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},\"serverInfo\":{\"name\":\"t\"},\"instructions\":\"be nice\"}}" ;;
    *'"tools/list"'*) echo "{\"jsonrpc\":\"2.0\",\"id\":$id,\"result\":{\"tools\":[{\"name\":\"echo.it\",\"description\":\"Echo\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"}}}}]}}" ;;
    *'"tools/call"'*) echo "{\"jsonrpc\":\"2.0\",\"id\":$id,\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"echoed\"}]}}" ;;
  esac
done"#;
        let config = json!({"command": "/bin/sh", "args": ["-c", script]});
        let mut mcp = Mcp::connect(&[("my srv".into(), "project".into(), config)]);
        assert_eq!(mcp.servers[0].status, "connected");
        assert_eq!(mcp.instructions(), [("my srv".to_string(), "be nice".to_string())]);
        let specs = mcp.specs();
        assert_eq!(specs[0]["name"], "mcp__my_srv__echo_it");
        let outcome = mcp.call("mcp__my_srv__echo_it", &json!({"text": "x"}));
        assert_eq!((outcome.text.as_str(), outcome.is_error), ("echoed", false));
        let bad = Mcp::connect(&[("gone".into(), "user".into(), json!({"command": "/nonexistent"}))]);
        assert!(bad.servers[0].status.starts_with("failed"));
    }

    #[test]
    fn expansion() {
        std::env::set_var("MCP_TEST_VAR", "v");
        assert_eq!(expand("${MCP_TEST_VAR}/x ${MISSING_ONE:-d}"), "v/x d");
    }
}
