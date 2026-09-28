//! claude: Claude Code for the collaboCore guest, as a native wasm32 program. It talks to the
//! Anthropic Messages API, or to any OpenAI-compatible Chat Completions server (a local LLM:
//! Ollama, llama.cpp, vLLM, LM Studio, …), through the host's request API, and works on the
//! sandbox's files with Claude Code's tools, settings, permissions, hooks, sessions and commands.
//! See addons/claude-code/README.md.
mod agent;
mod bridge;
mod config;
mod extensions;
mod hooks;
mod interrupt;
mod llm;
mod mcp;
mod permissions;
mod plugins;
mod prompt;
mod render;
mod repl;
mod session;
mod settings;
mod sse;
mod term;
mod tools;
mod ui;
mod util;

use std::cell::RefCell;
use std::io::{BufRead, IsTerminal};
use std::path::PathBuf;
use std::rc::Rc;
use std::time::{Duration, Instant};

use serde_json::{json, Map, Value};

use agent::Session;
use config::Overrides;
use permissions::Mode;
use settings::{Scope, Settings};
use ui::print::{Format, PrintUi};

/// The guest has no fork. std starts programs with posix_spawn (as Bash here does: an absolute
/// path, a working directory, a process group) and names fork only in fallbacks this program
/// never takes; this stub completes the link and fails if one ever is taken.
#[cfg(target_arch = "wasm32")]
#[no_mangle]
extern "C" fn fork() -> libc::pid_t {
    // SAFETY: errno is this thread's own.
    unsafe { *libc::__errno_location() = libc::ENOSYS };
    -1
}

const HELP: &str = "\
Usage: claude [options] [command] [prompt]

Claude Code - starts an interactive session by default, use -p/--print for non-interactive output

Arguments:
  prompt                           Your prompt

Options:
  -p, --print                      Print response and exit (useful for pipes)
  --output-format <format>         With --print: \"text\" (default), \"json\" (single result), or \"stream-json\" (realtime streaming)
  --input-format <format>          With --print: \"text\" (default) or \"stream-json\" (user messages as JSON lines on stdin)
  --verbose                        Show the full turn by turn output
  -c, --continue                   Continue the most recent conversation
  -r, --resume [sessionId]         Resume a conversation (the latest, or the one named)
  --session-id <uuid>              Use this session id
  --model <model>                  Model for the session: an alias (opus, sonnet, haiku) or a full name
  --provider <name>                anthropic (default) or openai (any OpenAI-compatible server)
  --base-url <url>                 The API's base URL, e.g. http://localhost:11434/v1 for Ollama
                                   (localhost is the host computer: requests are made there)
  --api-key <key>                  A key to send from here; normally the host adds the app's key
  --max-tokens <n>                 Longest answer, in tokens
  --max-turns <n>                  Stop after this many model calls in one prompt
  --no-stream                      Ask for whole answers instead of streamed ones
  --permission-mode <mode>         default, acceptEdits, plan or bypassPermissions
  --dangerously-skip-permissions   Bypass all permission checks (the sandbox is the boundary)
  --allowedTools, --allowed-tools <tools...>        Tools to allow, e.g. \"Bash(git:*) Edit\"
  --disallowedTools, --disallowed-tools <tools...>  Tools to deny
  --add-dir <directories...>       More directories the tools may work in
  --settings <file-or-json>        Settings file or JSON string to load on top
  --mcp-config <configs...>        MCP servers from JSON files or strings
  --plugin-dir <path>              Load a plugin from a folder for this run
  --system-prompt <prompt>         Replace the system prompt
  --append-system-prompt <prompt>  Append to the system prompt
  --show-config                    Print the connection settings in effect and exit
  --list-models                    Print the models the server offers and exit
  -v, --version                    Output the version number
  -h, --help                       Display help for command

Commands:
  config                           Manage settings (claude config list|get|set KEY [VALUE] [--global])
  mcp                              Configure and manage MCP servers (claude mcp list|get|add|remove)
  plugin                           Manage plugins (claude plugin list|install|marketplace add|…)

Settings: ~/.claude/settings.json, .claude/settings.json, .claude/settings.local.json and
/etc/collabo/addons/claude-code.json (what the app gave at start). Environment:
CLAUDE_CODE_PROVIDER, ANTHROPIC_BASE_URL, ANTHROPIC_MODEL, ANTHROPIC_API_KEY, OPENAI_BASE_URL,
OPENAI_MODEL, OPENAI_API_KEY, CLAUDE_CODE_MAX_TOKENS, MAX_THINKING_TOKENS.";

#[derive(Default)]
struct Args {
    overrides: Overrides,
    print: bool,
    verbose: bool,
    format: Option<String>,
    input_format: Option<String>,
    continue_last: bool,
    resume: Option<Option<String>>,
    session_id: Option<String>,
    permission_mode: Option<String>,
    allowed: Vec<String>,
    disallowed: Vec<String>,
    add_dirs: Vec<String>,
    settings: Option<String>,
    mcp_configs: Vec<String>,
    plugin_dirs: Vec<String>,
    show_config: bool,
    list_models: bool,
    prompt: Vec<String>,
}

/// "Bash(git log:*) Edit,Read" → rules (spaces and commas separate, not inside parentheses).
fn tool_list(text: &str) -> Vec<String> {
    let mut items = Vec::new();
    let (mut current, mut depth) = (String::new(), 0);
    for c in text.chars() {
        match c {
            '(' => depth += 1,
            ')' => depth -= 1,
            ' ' | ',' if depth == 0 => {
                if !current.trim().is_empty() {
                    items.push(std::mem::take(&mut current));
                }
                current.clear();
                continue;
            }
            _ => {}
        }
        current.push(c);
    }
    if !current.trim().is_empty() {
        items.push(current);
    }
    items
}

fn parse_args(argv: Vec<String>) -> Result<Args, String> {
    let mut args = Args::default();
    let mut iter = argv.into_iter().peekable();
    while let Some(arg) = iter.next() {
        let (flag, inline) = match arg.split_once('=') {
            Some((flag, value)) if flag.starts_with("--") => (flag.to_string(), Some(value.to_string())),
            _ => (arg.clone(), None),
        };
        let mut value = |name: &str| inline.clone().or_else(|| iter.next()).ok_or(format!("{name} needs a value"));
        let number = |name: &str, text: String| text.parse::<u32>().map_err(|_| format!("{name}: not a number: {text}"));
        match flag.as_str() {
            "-h" | "--help" => {
                println!("{HELP}");
                std::process::exit(0);
            }
            "-v" | "--version" => {
                println!("{} (Claude Code, {})", repl::VERSION, repl::EDITION);
                std::process::exit(0);
            }
            "-p" | "--print" => args.print = true,
            "--verbose" => args.verbose = true,
            "-c" | "--continue" => args.continue_last = true,
            "-r" | "--resume" => {
                let id = match inline {
                    Some(id) => Some(id),
                    None => iter.next_if(|next| !next.starts_with('-') && next.len() >= 8 && next.chars().all(|c| c.is_ascii_hexdigit() || c == '-')),
                };
                args.resume = Some(id);
            }
            "--session-id" => args.session_id = Some(value("--session-id")?),
            "--provider" => args.overrides.provider = Some(value("--provider")?),
            "--base-url" => args.overrides.base_url = Some(value("--base-url")?),
            "--model" | "-m" => args.overrides.model = Some(value("--model")?),
            "--api-key" => args.overrides.api_key = Some(value("--api-key")?),
            "--max-tokens" => args.overrides.max_tokens = Some(number("--max-tokens", value("--max-tokens")?)?),
            "--max-turns" => args.overrides.max_turns = Some(number("--max-turns", value("--max-turns")?)?),
            "--no-stream" => args.overrides.stream = Some(false),
            "--permission-mode" => args.permission_mode = Some(value("--permission-mode")?),
            "--dangerously-skip-permissions" => args.permission_mode = Some("bypassPermissions".into()),
            "--allowedTools" | "--allowed-tools" => args.allowed.extend(tool_list(&value("--allowedTools")?)),
            "--disallowedTools" | "--disallowed-tools" => args.disallowed.extend(tool_list(&value("--disallowedTools")?)),
            "--add-dir" => args.add_dirs.push(value("--add-dir")?),
            "--settings" => args.settings = Some(value("--settings")?),
            "--mcp-config" => args.mcp_configs.push(value("--mcp-config")?),
            "--plugin-dir" => args.plugin_dirs.push(value("--plugin-dir")?),
            "--system-prompt" => args.overrides.system_prompt = Some(value("--system-prompt")?),
            "--append-system-prompt" => args.overrides.append_system_prompt = Some(value("--append-system-prompt")?),
            "--output-format" => args.format = Some(value("--output-format")?),
            "--input-format" => args.input_format = Some(value("--input-format")?),
            "--show-config" => args.show_config = true,
            "--list-models" => args.list_models = true,
            "--" => args.prompt.extend(iter.by_ref()),
            other if other.starts_with('-') && other.len() > 1 => return Err(format!("unknown option '{other}' (claude --help)")),
            _ => args.prompt.push(arg),
        }
    }
    Ok(args)
}

fn fail(message: &str) -> ! {
    eprintln!("claude: {message}");
    std::process::exit(1)
}

/// claude mcp …: servers in ~/.claude/settings.json (user), .mcp.json (project) or
/// .claude/settings.local.json (local, the default).
fn mcp_command(argv: &[String], settings: &mut Settings) -> i32 {
    let mut scope = "local".to_string();
    let mut env: Vec<(String, String)> = Vec::new();
    let mut transport = "stdio".to_string();
    let mut headers: Vec<(String, String)> = Vec::new();
    let mut positional = Vec::new();
    let mut iter = argv.iter().skip(1).cloned();
    let mut rest: Vec<String> = Vec::new();
    while let Some(arg) = iter.next() {
        match arg.as_str() {
            "-s" | "--scope" => scope = iter.next().unwrap_or_default(),
            "-e" | "--env" => {
                if let Some((k, v)) = iter.next().unwrap_or_default().split_once('=') {
                    env.push((k.into(), v.into()));
                }
            }
            "-t" | "--transport" => transport = iter.next().unwrap_or_default(),
            "-H" | "--header" => {
                if let Some((k, v)) = iter.next().unwrap_or_default().split_once(':') {
                    headers.push((k.trim().into(), v.trim().into()));
                }
            }
            "--" => {
                rest.extend(iter.by_ref());
            }
            _ => positional.push(arg),
        }
    }
    let project_file = settings.project_root.join(".mcp.json");
    let all = mcp::configured(settings, &[]);
    match positional.first().map(String::as_str) {
        Some("list") | None => {
            if all.is_empty() {
                println!("No MCP servers configured. Use `claude mcp add` to add a server.");
            }
            let connected = mcp::Mcp::connect(&all);
            for server in &connected.servers {
                println!("{}: {} - {}", server.name, server.scope, if server.status == "connected" { format!("✓ Connected ({} tools)", server.tools.len()) } else { format!("✗ {}", server.status) });
            }
            0
        }
        Some("get") => {
            let name = positional.get(1).cloned().unwrap_or_default();
            match all.iter().find(|(n, _, _)| *n == name) {
                Some((_, scope, config)) => {
                    println!("{name} ({scope}):\n{}", serde_json::to_string_pretty(config).unwrap_or_default());
                    0
                }
                None => {
                    eprintln!("No MCP server named {name}");
                    1
                }
            }
        }
        Some("add") | Some("add-json") => {
            let Some(name) = positional.get(1).cloned() else {
                eprintln!("usage: claude mcp add [-s local|project|user] [-e KEY=VALUE] NAME -- COMMAND [ARGS…]\n       claude mcp add --transport http NAME URL [-H 'Header: value']\n       claude mcp add-json NAME '{{…}}'");
                return 1;
            };
            let config = if positional[0] == "add-json" {
                match serde_json::from_str::<Value>(positional.get(2).map(String::as_str).unwrap_or("")) {
                    Ok(value) => value,
                    Err(error) => {
                        eprintln!("invalid JSON: {error}");
                        return 1;
                    }
                }
            } else if transport == "http" || transport == "sse" {
                let url = positional.get(2).cloned().unwrap_or_default();
                let headers: Map<String, Value> = headers.into_iter().map(|(k, v)| (k, json!(v))).collect();
                json!({"type": "http", "url": url, "headers": headers})
            } else {
                let mut command: Vec<String> = positional[2..].to_vec();
                command.extend(rest);
                if command.is_empty() {
                    eprintln!("no command given for {name} (claude mcp add NAME -- COMMAND ARGS…)");
                    return 1;
                }
                let env: Map<String, Value> = env.into_iter().map(|(k, v)| (k, json!(v))).collect();
                json!({"type": "stdio", "command": command[0], "args": command[1..], "env": env})
            };
            let result = match scope.as_str() {
                "project" => {
                    let mut file: Value = std::fs::read_to_string(&project_file).ok().and_then(|t| serde_json::from_str(&t).ok()).unwrap_or_else(|| json!({}));
                    file["mcpServers"][&name] = config;
                    std::fs::write(&project_file, serde_json::to_string_pretty(&file).unwrap() + "\n").map(|_| project_file.clone()).map_err(|e| e.to_string())
                }
                other => {
                    let scope = if other == "user" { Scope::User } else { Scope::Local };
                    settings.update(scope, |object| {
                        let servers = object.entry("mcpServers").or_insert_with(|| json!({}));
                        servers[&name] = config;
                    })
                }
            };
            match result {
                Ok(path) => {
                    println!("Added MCP server {name} to {}", path.display());
                    0
                }
                Err(message) => {
                    eprintln!("{message}");
                    1
                }
            }
        }
        Some("remove") => {
            let name = positional.get(1).cloned().unwrap_or_default();
            let mut removed = false;
            for scope in [Scope::Local, Scope::Project, Scope::User] {
                let _ = settings.update(scope, |object| {
                    if let Some(servers) = object.get_mut("mcpServers").and_then(Value::as_object_mut) {
                        removed |= servers.remove(&name).is_some();
                    }
                });
            }
            if let Ok(text) = std::fs::read_to_string(&project_file) {
                if let Ok(mut file) = serde_json::from_str::<Value>(&text) {
                    if let Some(servers) = file.get_mut("mcpServers").and_then(Value::as_object_mut) {
                        if servers.remove(&name).is_some() {
                            removed = true;
                            let _ = std::fs::write(&project_file, serde_json::to_string_pretty(&file).unwrap() + "\n");
                        }
                    }
                }
            }
            println!("{}", if removed { format!("Removed MCP server {name}") } else { format!("No MCP server named {name}") });
            i32::from(!removed)
        }
        Some(other) => {
            eprintln!("unknown mcp command {other} (list, get, add, add-json, remove)");
            1
        }
    }
}

/// claude config …
fn config_command(argv: &[String], settings: &mut Settings) -> i32 {
    let global = argv.iter().any(|a| a == "--global" || a == "-g");
    let words: Vec<&String> = argv.iter().skip(1).filter(|a| !a.starts_with('-')).collect();
    let scope = if global { Scope::User } else { Scope::Local };
    match words.as_slice() {
        [] | [_] if words.first().is_none_or(|w| *w == "list" || *w == "ls") => {
            println!("{}", serde_json::to_string_pretty(&settings.merged).unwrap_or_default());
            0
        }
        [get, key] if get.as_str() == "get" => {
            println!("{}", settings.get(&format!("/{}", key.replace('.', "/"))).map(|v| v.to_string()).unwrap_or_default());
            0
        }
        [set, key, value @ ..] if set.as_str() == "set" && !value.is_empty() => {
            let text = value.iter().map(|v| v.as_str()).collect::<Vec<_>>().join(" ");
            let parsed: Value = serde_json::from_str(&text).unwrap_or(json!(text));
            match settings.update(scope, |object| {
                object.insert(key.to_string(), parsed.clone());
            }) {
                Ok(path) => {
                    println!("Set {key} in {}", path.display());
                    0
                }
                Err(message) => {
                    eprintln!("{message}");
                    1
                }
            }
        }
        [remove, key] if remove.as_str() == "remove" || remove.as_str() == "rm" => match settings.update(scope, |object| {
            object.remove(key.as_str());
        }) {
            Ok(_) => 0,
            Err(message) => {
                eprintln!("{message}");
                1
            }
        },
        _ => {
            eprintln!("usage: claude config list | get KEY | set KEY VALUE | remove KEY  [--global]");
            1
        }
    }
}

fn main() {
    let argv: Vec<String> = std::env::args().skip(1).collect();
    let cwd = std::env::current_dir().unwrap_or_else(|_| PathBuf::from("/"));

    if matches!(argv.first().map(String::as_str), Some("mcp") | Some("config") | Some("plugin") | Some("plugins")) {
        let mut settings = Settings::load(&cwd, None).unwrap_or_else(|message| fail(&message));
        settings.apply_env();
        plugins::load(&settings, &[]);
        std::process::exit(match argv[0].as_str() {
            "mcp" => mcp_command(&argv, &mut settings),
            "config" => config_command(&argv, &mut settings),
            _ => {
                let words: Vec<&str> = argv[1..].iter().map(String::as_str).collect();
                match plugins::command(&words, &mut settings, &cwd, &[]) {
                    Ok(text) => {
                        println!("{text}");
                        0
                    }
                    Err(message) => {
                        eprintln!("{message}");
                        1
                    }
                }
            }
        });
    }

    let args = parse_args(argv).unwrap_or_else(|message| {
        eprintln!("claude: {message}");
        std::process::exit(2)
    });
    let settings = Settings::load(&cwd, args.settings.as_deref()).unwrap_or_else(|message| fail(&message));
    settings.apply_env();
    let config = config::load(&settings, &args.overrides).unwrap_or_else(|message| fail(&message));
    if args.show_config {
        println!("{}", config.describe());
        return;
    }
    if args.list_models {
        match llm::list_models(&config) {
            Ok(models) => models.iter().for_each(|model| println!("{model}")),
            Err(error) => fail(&error.message),
        }
        return;
    }
    term::init_color(args.print);
    let interactive = !args.print;
    if interactive && !std::io::stdin().is_terminal() && args.prompt.is_empty() {
        // Piped input without -p: the same as -p.
    }

    let model = match &config.model {
        Some(model) => model.clone(),
        None => match llm::list_models(&config) {
            Ok(models) if !models.is_empty() => models[0].clone(),
            Ok(_) => fail(&format!("{} lists no models; name one with --model", config.base_url)),
            Err(error) => fail(&format!("no --model given, and the server's model list failed: {}", error.message)),
        },
    };
    let mode = match args.permission_mode.clone().or_else(|| settings.str("/permissions/defaultMode")) {
        Some(mode) => Mode::parse(&mode).unwrap_or_else(|message| fail(&message)),
        None => Mode::Default,
    };

    // The conversation: new, the latest (--continue), or one named (--resume).
    session::cleanup(settings.u64("/cleanupPeriodDays").unwrap_or(30));
    let mut messages = Vec::new();
    let mut source = "startup";
    let transcript = if args.continue_last || args.resume.is_some() {
        let path = match (&args.resume, args.continue_last) {
            (Some(Some(id)), _) => session::find(&cwd, id).unwrap_or_else(|| fail(&format!("No conversation found with session ID: {id}"))),
            _ => session::list(&cwd).into_iter().next().map(|info| info.path).unwrap_or_else(|| fail("No conversation found to continue")),
        };
        messages = session::load(&path).unwrap_or_else(|message| fail(&message));
        source = "resume";
        session::Transcript::reopen(&cwd, &path)
    } else {
        session::Transcript::new(&cwd, args.session_id.clone())
    };

    let plugin_dirs: Vec<PathBuf> = args.plugin_dirs.iter().map(|d| permissions::normalize(std::path::Path::new(&permissions::expand_home(d)), &cwd)).collect();
    plugins::load(&settings, &plugin_dirs);
    let extra_dirs: Vec<PathBuf> = args.add_dirs.iter().map(|d| permissions::normalize(std::path::Path::new(&permissions::expand_home(d)), &cwd)).collect();
    let rules = permissions::Rules::from_settings(&settings, &cwd, &extra_dirs);
    let mut hook_config = settings.get("/hooks").cloned().unwrap_or_else(|| json!({}));
    plugins::merge_hooks(&mut hook_config);
    let hooks = hooks::Hooks {
        config: hook_config,
        session_id: transcript.id.clone(),
        transcript_path: transcript.path.display().to_string(),
        project_dir: settings.project_root.clone(),
    };
    let mut servers = mcp::configured(&settings, &args.mcp_configs);
    servers.extend(plugins::mcp_servers());
    if interactive && !servers.is_empty() {
        eprint!("{}", term::dim(&format!("Connecting to {} MCP server(s)…\r", servers.len())));
    }
    let mcp = mcp::Mcp::connect(&servers);
    if interactive && !servers.is_empty() {
        eprint!("\r\x1b[2K");
    }
    let styles = extensions::styles(&settings.project_root);
    let style = config.output_style.as_ref().and_then(|name| styles.iter().find(|s| s.name.eq_ignore_ascii_case(name)).cloned());
    let mut context = tools::Context::new(cwd.clone());
    if interactive {
        context.progress = Some(Box::new(|path: &std::path::Path| {
            // The command's latest output line under the spinner.
            if let Ok(text) = std::fs::read_to_string(path) {
                if let Some(last) = text.lines().rev().find(|l| !l.trim().is_empty()) {
                    ui::spinner::set_note(Some(last.to_string()));
                }
            }
        }));
    }
    let mut session = Session {
        web_search: settings.bool("/webSearch").unwrap_or(true),
        agents: extensions::agents(&settings.project_root),
        style,
        id: transcript.id.clone(),
        overrides: args.overrides.clone(),
        goal: None,
        skills: extensions::skills(&settings.project_root),
        skill_allow: Vec::new(),
        checkpoints: Vec::new(),
        plugin_dirs,
        config,
        model,
        messages,
        context,
        mode,
        bypass_available: mode == Mode::Bypass,
        rules,
        extra_dirs,
        cli_allow: args.allowed.clone(),
        cli_deny: args.disallowed.clone(),
        hooks,
        transcript: Some(transcript),
        mcp: Rc::new(RefCell::new(mcp)),
        usage: llm::Usage::default(),
        context_tokens: 0,
        api_time: Duration::ZERO,
        started: Instant::now(),
        thinking_on: false,
        depth: 0,
        agent: None,
        settings,
    };
    // A throttle on the progress callback: reading the output file every 20 ms is wasteful.
    if let Some(progress) = session.context.progress.take() {
        let last = std::cell::Cell::new(Instant::now());
        session.context.progress = Some(Box::new(move |path: &std::path::Path| {
            if last.get().elapsed() > Duration::from_millis(300) {
                last.set(Instant::now());
                progress(path);
            }
        }));
    }

    let start = session.hooks.run("SessionStart", Some(source), &cwd, json!({"source": source}));
    let mut first = args.prompt.join(" ");

    if interactive {
        if !start.context.is_empty() {
            let mut quiet = PrintUi { format: Format::Text, verbose: false, session_id: session.id.clone(), denied: Vec::new() };
            session.push_user(start.context.iter().map(|c| json!({"type": "text", "text": format!("<system-reminder>\n{c}\n</system-reminder>")})).collect(), &mut quiet);
        }
        repl::run(&mut session, Some(first));
        return;
    }

    // -p
    let format = match args.format.as_deref() {
        None | Some("text") => Format::Text,
        Some("json") => Format::Json,
        Some("stream-json") => Format::StreamJson,
        Some(other) => fail(&format!("--output-format: text, json or stream-json, not {other}")),
    };
    let mut ui = PrintUi { format, verbose: args.verbose, session_id: session.id.clone(), denied: Vec::new() };
    if !start.context.is_empty() {
        session.push_user(start.context.iter().map(|c| json!({"type": "text", "text": format!("<system-reminder>\n{c}\n</system-reminder>")})).collect(), &mut ui);
    }
    if format == Format::StreamJson {
        let tools: Vec<Value> = session.tool_specs().iter().map(|t| t["name"].clone()).collect();
        let servers: Vec<Value> = session.mcp.borrow().servers.iter().map(|s| json!({"name": s.name, "status": s.status})).collect();
        println!(
            "{}",
            json!({"type": "system", "subtype": "init", "cwd": cwd.display().to_string(), "session_id": session.id, "tools": tools,
                   "mcp_servers": servers, "model": session.model, "permissionMode": session.mode.name(), "apiKeySource": "none",
                   "provider": session.config.provider.name()})
        );
    }
    // The prompts: the arguments (or stdin), or JSON lines of user messages.
    let mut prompts: Vec<String> = Vec::new();
    if args.input_format.as_deref() == Some("stream-json") {
        for line in std::io::stdin().lock().lines().map_while(Result::ok) {
            let Ok(value) = serde_json::from_str::<Value>(&line) else { continue };
            let content = &value["message"]["content"];
            let text = match content {
                Value::String(text) => text.clone(),
                Value::Array(blocks) => blocks.iter().filter_map(|b| b["text"].as_str()).collect::<Vec<_>>().join("\n"),
                _ => continue,
            };
            prompts.push(text);
        }
    } else {
        if first.trim().is_empty() && !std::io::stdin().is_terminal() {
            first = ui::terminal::read_all_stdin();
        } else if !std::io::stdin().is_terminal() {
            // Piped input with a prompt: the input comes first, as context.
            let piped = ui::terminal::read_all_stdin();
            if !piped.trim().is_empty() {
                first = format!("{piped}\n\n{first}");
            }
        }
        if first.trim().is_empty() && session.messages.is_empty() {
            fail("Input must be provided either through stdin or as a prompt argument when using --print");
        }
        prompts.push(first);
    }
    let started = Instant::now();
    let mut result: Result<String, String> = Ok(String::new());
    for mut prompt in prompts {
        if let Some(expanded) = repl::expand_slash(&mut session, prompt.trim()) {
            prompt = expanded;
        }
        result = match prompt.trim().is_empty() {
            true => session.turns(&mut ui, None),
            false => session.prompt(prompt.trim(), &mut ui),
        };
        if result.is_err() {
            break;
        }
    }
    let cwd = session.context.cwd.clone();
    session.hooks.run("SessionEnd", None, &cwd, json!({"reason": "other"}));
    let failed = result.is_err();
    let turns = session.messages.iter().filter(|m| m["role"] == "assistant").count();
    match format {
        Format::Text => match &result {
            Ok(text) => println!("{}", text.trim_end()),
            Err(message) => eprintln!("claude: {message}"),
        },
        Format::Json | Format::StreamJson => {
            let usage = session.usage;
            let (text, subtype) = match &result {
                Ok(text) => (text.clone(), "success"),
                Err(message) if message.starts_with("Reached the maximum number of turns") => (message.clone(), "error_max_turns"),
                Err(message) => (message.clone(), "error_during_execution"),
            };
            println!(
                "{}",
                json!({
                    "type": "result", "subtype": subtype, "is_error": failed,
                    "duration_ms": started.elapsed().as_millis() as u64, "duration_api_ms": session.api_time.as_millis() as u64,
                    "num_turns": turns, "result": text, "session_id": session.id, "model": session.model,
                    "provider": session.config.provider.name(),
                    "usage": {"input_tokens": usage.input, "output_tokens": usage.output,
                              "cache_read_input_tokens": usage.cache_read, "cache_creation_input_tokens": usage.cache_write},
                    "permission_denials": ui.denied,
                })
            );
        }
    }
    std::process::exit(i32::from(failed));
}
