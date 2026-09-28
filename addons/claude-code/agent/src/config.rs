//! The model connection and the limits of a session, from the merged settings (settings.rs),
//! the environment and the command line, in that order of precedence:
//!
//!   settings   provider ("anthropic" | "openai"), baseUrl, model, apiKey, maxTokens, stream,
//!              appendSystemPrompt, maxTurns, contextWindow, alwaysThinkingEnabled,
//!              maxThinkingTokens, outputStyle
//!   environment CLAUDE_CODE_PROVIDER, then per provider ANTHROPIC_{BASE_URL,MODEL,API_KEY} or
//!              OPENAI_{BASE_URL,MODEL,API_KEY}; CLAUDE_CODE_MAX_TOKENS, MAX_THINKING_TOKENS
//!   command line --provider, --base-url, --model, --api-key, …
use crate::settings::Settings;

pub const SETTINGS_FILE: &str = "/etc/collabo/addons/claude-code.json";
pub const ANTHROPIC_BASE_URL: &str = "https://api.anthropic.com";
pub const ANTHROPIC_MODEL: &str = "claude-opus-5-5";
pub const ANTHROPIC_MAX_TOKENS: u32 = 32000;
pub const OPENAI_BASE_URL: &str = "https://api.openai.com/v1";

#[derive(Clone, Copy, PartialEq, Debug)]
pub enum Provider {
    Anthropic,
    OpenAi,
}

impl Provider {
    pub fn parse(text: &str) -> Result<Provider, String> {
        match text.to_ascii_lowercase().as_str() {
            "anthropic" | "claude" => Ok(Provider::Anthropic),
            "openai" | "openai-compatible" | "local" => Ok(Provider::OpenAi),
            other => Err(format!("unknown provider \"{other}\" (anthropic or openai)")),
        }
    }

    pub fn name(self) -> &'static str {
        match self {
            Provider::Anthropic => "anthropic",
            Provider::OpenAi => "openai",
        }
    }
}

/// Short names for Claude models, as /model and --model take them.
pub const ALIASES: [(&str, &str, &str); 3] = [
    ("opus", "claude-opus-5-5", "Opus 5.5: the most capable, for complex work"),
    ("sonnet", "claude-sonnet-5", "Sonnet 5: fast and capable, for everyday work"),
    ("haiku", "claude-haiku-4-5-20251001", "Haiku 4.5: the fastest, for simple tasks"),
];

pub fn resolve_model(provider: Provider, name: &str) -> String {
    if provider == Provider::Anthropic {
        if name == "default" {
            return ANTHROPIC_MODEL.to_string();
        }
        if let Some((_, id, _)) = ALIASES.iter().find(|(alias, _, _)| *alias == name) {
            return id.to_string();
        }
    }
    name.to_string()
}

#[derive(Clone, Debug)]
pub struct Config {
    pub provider: Provider,
    pub base_url: String,
    /// None for an OpenAI-compatible server: the first model it lists is used.
    pub model: Option<String>,
    /// Usually unset: the host adds the app's key to requests. Needed only for a plain-http
    /// server that wants one, or when the key is given inside the sandbox.
    pub api_key: Option<String>,
    /// None: the server's default (OpenAI-compatible only; Anthropic requires a value).
    pub max_tokens: Option<u32>,
    pub stream: bool,
    pub system_prompt: Option<String>,
    pub append_system_prompt: Option<String>,
    pub max_turns: u32,
    /// Tokens the model can take in; the conversation is compacted before it fills.
    pub context_window: u64,
    /// Extended thinking on every request (Anthropic), with this budget.
    pub always_thinking: bool,
    pub thinking_budget: u32,
    pub output_style: Option<String>,
}

/// What the command line set; None leaves the lower layers' value.
#[derive(Default, Clone)]
pub struct Overrides {
    pub provider: Option<String>,
    pub base_url: Option<String>,
    pub model: Option<String>,
    pub api_key: Option<String>,
    pub max_tokens: Option<u32>,
    pub stream: Option<bool>,
    pub system_prompt: Option<String>,
    pub append_system_prompt: Option<String>,
    pub max_turns: Option<u32>,
}

fn env(key: &str) -> Option<String> {
    std::env::var(key).ok().filter(|text| !text.trim().is_empty())
}

pub fn load(settings: &Settings, overrides: &Overrides) -> Result<Config, String> {
    let provider = match overrides.provider.clone().or_else(|| env("CLAUDE_CODE_PROVIDER")).or_else(|| settings.str("/provider")) {
        Some(name) => Provider::parse(&name)?,
        None => Provider::Anthropic,
    };
    let prefix = match provider {
        Provider::Anthropic => "ANTHROPIC",
        Provider::OpenAi => "OPENAI",
    };
    let base_url = overrides
        .base_url
        .clone()
        .or_else(|| env(&format!("{prefix}_BASE_URL")))
        .or_else(|| settings.str("/baseUrl"))
        .unwrap_or_else(|| match provider {
            Provider::Anthropic => ANTHROPIC_BASE_URL.into(),
            Provider::OpenAi => OPENAI_BASE_URL.into(),
        });
    if !base_url.starts_with("http://") && !base_url.starts_with("https://") {
        return Err(format!("base URL \"{base_url}\" must start with http:// or https://"));
    }
    let model = overrides
        .model
        .clone()
        .or_else(|| env(&format!("{prefix}_MODEL")))
        .or_else(|| settings.str("/model"))
        .map(|name| resolve_model(provider, &name))
        .or(match provider {
            Provider::Anthropic => Some(ANTHROPIC_MODEL.into()),
            Provider::OpenAi => None,
        });
    let api_key = overrides.api_key.clone().or_else(|| env(&format!("{prefix}_API_KEY"))).or_else(|| settings.str("/apiKey"));
    let max_tokens = overrides
        .max_tokens
        .or_else(|| env("CLAUDE_CODE_MAX_TOKENS").and_then(|text| text.parse().ok()))
        .or_else(|| settings.u64("/maxTokens").map(|n| n as u32))
        .or(match provider {
            Provider::Anthropic => Some(ANTHROPIC_MAX_TOKENS),
            Provider::OpenAi => None,
        });
    let thinking_budget = env("MAX_THINKING_TOKENS")
        .and_then(|text| text.parse().ok())
        .or_else(|| settings.u64("/maxThinkingTokens").map(|n| n as u32));
    Ok(Config {
        provider,
        base_url: base_url.trim_end_matches('/').to_string(),
        model,
        api_key,
        max_tokens,
        stream: overrides.stream.or_else(|| settings.bool("/stream")).unwrap_or(true),
        system_prompt: overrides.system_prompt.clone(),
        append_system_prompt: overrides.append_system_prompt.clone().or_else(|| settings.str("/appendSystemPrompt")),
        max_turns: overrides.max_turns.or_else(|| settings.u64("/maxTurns").map(|n| n as u32)).unwrap_or(200),
        context_window: settings.u64("/contextWindow").unwrap_or(match provider {
            Provider::Anthropic => 200_000,
            Provider::OpenAi => 32_768,
        }),
        always_thinking: settings.bool("/alwaysThinkingEnabled").unwrap_or(false) || thinking_budget.is_some(),
        thinking_budget: thinking_budget.unwrap_or(10_000),
        output_style: settings.str("/outputStyle"),
    })
}

impl Config {
    /// For /config and --show-config: never the key itself.
    pub fn describe(&self) -> String {
        format!(
            "provider:        {}\nbase URL:        {}\nmodel:           {}\nAPI key:         {}\nmax tokens:      {}\nstreaming:       {}\ncontext window:  {}\nthinking:        {}\nmax turns:       {}",
            self.provider.name(),
            self.base_url,
            self.model.as_deref().unwrap_or("(the server's first)"),
            match &self.api_key {
                Some(_) => "set in the sandbox",
                None => "none here (the host adds the app's key, if it has one)",
            },
            self.max_tokens.map_or("(server default)".to_string(), |n| n.to_string()),
            self.stream,
            self.context_window,
            match self.always_thinking {
                true => format!("always, {} tokens", self.thinking_budget),
                false => "when asked (think / think hard / ultrathink)".into(),
            },
            self.max_turns,
        )
    }
}
