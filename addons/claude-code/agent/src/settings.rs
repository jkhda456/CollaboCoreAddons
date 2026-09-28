//! Settings files, merged as Claude Code merges them. From lowest to highest precedence:
//!
//!   addon     /etc/collabo/addons/claude-code.json   what the app gave at start (the engine
//!             writes it; CLAUDE_CODE_CONFIG names another file)
//!   user      ~/.claude/settings.json
//!   project   <project>/.claude/settings.json        shared with the team
//!   local     <project>/.claude/settings.local.json  this machine only; where "don't ask
//!             again" answers go
//!   cli       --settings FILE-or-JSON
//!
//! Objects merge key by key, arrays (permission rules, hooks) are joined, other values are
//! replaced. The project is the nearest directory up from the working directory with a .claude
//! folder or a .git; failing that, the working directory.
use std::path::{Path, PathBuf};

use serde_json::{json, Map, Value};

#[derive(Clone, Copy, PartialEq, Debug)]
pub enum Scope {
    Addon,
    User,
    Project,
    Local,
    Cli,
}

impl Scope {
    pub fn name(self) -> &'static str {
        match self {
            Scope::Addon => "addon",
            Scope::User => "user",
            Scope::Project => "project",
            Scope::Local => "local",
            Scope::Cli => "cli",
        }
    }
}

#[derive(Clone)]
pub struct Layer {
    pub scope: Scope,
    pub path: Option<PathBuf>,
    pub value: Value,
}

#[derive(Clone)]
pub struct Settings {
    pub layers: Vec<Layer>,
    pub merged: Value,
    pub project_root: PathBuf,
}

pub fn home() -> PathBuf {
    PathBuf::from(std::env::var("HOME").ok().filter(|h| !h.is_empty()).unwrap_or_else(|| "/root".into()))
}

/// ~/.claude, or CLAUDE_CONFIG_DIR.
pub fn user_dir() -> PathBuf {
    match std::env::var("CLAUDE_CONFIG_DIR") {
        Ok(dir) if !dir.is_empty() => PathBuf::from(dir),
        _ => home().join(".claude"),
    }
}

pub fn project_root(cwd: &Path) -> PathBuf {
    for dir in cwd.ancestors() {
        if dir == home() {
            break;
        }
        if dir.join(".claude").is_dir() || dir.join(".git").exists() {
            return dir.to_path_buf();
        }
    }
    cwd.to_path_buf()
}

fn read_json(path: &Path) -> Result<Option<Value>, String> {
    match std::fs::read(path) {
        Err(_) => Ok(None),
        Ok(bytes) if bytes.iter().all(u8::is_ascii_whitespace) => Ok(None),
        Ok(bytes) => match serde_json::from_slice::<Value>(&bytes) {
            Ok(value @ Value::Object(_)) => Ok(Some(value)),
            Ok(_) => Err(format!("{} is not a JSON object", path.display())),
            Err(error) => Err(format!("{}: {error}", path.display())),
        },
    }
}

/// `over` into `base`: objects key by key, arrays joined without repeats, the rest replaced.
pub fn merge(base: &mut Value, over: &Value) {
    match (base, over) {
        (Value::Object(base), Value::Object(over)) => {
            for (key, value) in over {
                match base.get_mut(key) {
                    Some(existing) => merge(existing, value),
                    None => {
                        base.insert(key.clone(), value.clone());
                    }
                }
            }
        }
        (Value::Array(base), Value::Array(over)) => {
            for item in over {
                if !base.contains(item) {
                    base.push(item.clone());
                }
            }
        }
        (base, over) => *base = over.clone(),
    }
}

impl Settings {
    pub fn load(cwd: &Path, cli: Option<&str>) -> Result<Settings, String> {
        let root = project_root(cwd);
        let addon = std::env::var("CLAUDE_CODE_CONFIG").map(PathBuf::from).unwrap_or_else(|_| PathBuf::from(crate::config::SETTINGS_FILE));
        let mut layers = Vec::new();
        let mut warnings = Vec::new();
        for (scope, path) in [
            (Scope::Addon, addon),
            (Scope::User, user_dir().join("settings.json")),
            (Scope::Project, root.join(".claude/settings.json")),
            (Scope::Local, root.join(".claude/settings.local.json")),
        ] {
            match read_json(&path) {
                Ok(Some(value)) => layers.push(Layer { scope, path: Some(path), value: normalize(scope, value) }),
                Ok(None) => layers.push(Layer { scope, path: Some(path), value: json!({}) }),
                Err(message) => {
                    warnings.push(message);
                    layers.push(Layer { scope, path: Some(path), value: json!({}) });
                }
            }
        }
        if let Some(cli) = cli {
            let value = match cli.trim_start().starts_with('{') {
                true => serde_json::from_str::<Value>(cli).map_err(|error| format!("--settings: {error}"))?,
                false => read_json(Path::new(cli))?.ok_or_else(|| format!("--settings: cannot read {cli}"))?,
            };
            layers.push(Layer { scope: Scope::Cli, path: None, value: normalize(Scope::Cli, value) });
        }
        if let Some(first) = warnings.first() {
            // A broken file is reported, not fatal: the others still apply.
            eprintln!("claude: settings: {first}");
        }
        let mut settings = Settings { layers, merged: json!({}), project_root: root };
        settings.remerge();
        Ok(settings)
    }

    pub fn remerge(&mut self) {
        let mut merged = json!({});
        for layer in &self.layers {
            merge(&mut merged, &layer.value);
        }
        self.merged = merged;
    }

    pub fn get(&self, pointer: &str) -> Option<&Value> {
        self.merged.pointer(pointer)
    }

    pub fn str(&self, pointer: &str) -> Option<String> {
        self.get(pointer).and_then(Value::as_str).map(str::to_string).filter(|text| !text.trim().is_empty())
    }

    pub fn bool(&self, pointer: &str) -> Option<bool> {
        self.get(pointer).and_then(Value::as_bool)
    }

    pub fn u64(&self, pointer: &str) -> Option<u64> {
        self.get(pointer).and_then(Value::as_u64)
    }

    pub fn strings(&self, pointer: &str) -> Vec<String> {
        self.get(pointer)
            .and_then(Value::as_array)
            .map(|items| items.iter().filter_map(Value::as_str).map(str::to_string).collect())
            .unwrap_or_default()
    }

    pub fn path_of(&self, scope: Scope) -> Option<PathBuf> {
        self.layers.iter().find(|layer| layer.scope == scope).and_then(|layer| layer.path.clone())
    }

    /// Changes one scope's file (read fresh, so edits made meanwhile are kept) and the merged
    /// view. `change` gets the file's object.
    pub fn update(&mut self, scope: Scope, change: impl FnOnce(&mut Map<String, Value>)) -> Result<PathBuf, String> {
        let path = self.path_of(scope).ok_or_else(|| format!("the {} settings have no file", scope.name()))?;
        let mut value = read_json(&path)?.unwrap_or_else(|| json!({}));
        change(value.as_object_mut().expect("settings are an object"));
        if let Some(parent) = path.parent() {
            std::fs::create_dir_all(parent).map_err(|error| format!("{}: {error}", parent.display()))?;
        }
        let text = serde_json::to_string_pretty(&value).expect("settings serialize") + "\n";
        std::fs::write(&path, text).map_err(|error| format!("{}: {error}", path.display()))?;
        if let Some(layer) = self.layers.iter_mut().find(|layer| layer.scope == scope) {
            layer.value = normalize(scope, value);
        }
        self.remerge();
        Ok(path)
    }

    /// Sets (Some) or removes (None) one value in a scope's file; `pointer` is a JSON pointer
    /// such as "/permissions/defaultMode" (objects on the way are made).
    pub fn set_value(&mut self, scope: Scope, pointer: &str, value: Option<Value>) -> Result<PathBuf, String> {
        let parts: Vec<String> = pointer.trim_start_matches('/').split('/').map(str::to_string).collect();
        self.update(scope, move |object| {
            let (last, parents) = parts.split_last().expect("a pointer names a key");
            let mut current = object;
            for part in parents {
                let entry = current.entry(part.clone()).or_insert_with(|| json!({}));
                if !entry.is_object() {
                    *entry = json!({});
                }
                current = entry.as_object_mut().unwrap();
            }
            match value {
                Some(value) => {
                    current.insert(last.clone(), value);
                }
                None => {
                    current.remove(last);
                }
            }
        })
    }

    /// The scope a value comes from (the highest layer that has it).
    pub fn source_of(&self, pointer: &str) -> Option<Scope> {
        self.layers.iter().rev().find(|layer| layer.value.pointer(pointer).is_some()).map(|layer| layer.scope)
    }

    /// Adds a permission rule ("allow", "ask" or "deny") to a scope's file.
    pub fn add_rule(&mut self, scope: Scope, kind: &str, rule: &str) -> Result<PathBuf, String> {
        let (kind, rule) = (kind.to_string(), rule.to_string());
        self.update(scope, move |object| {
            let permissions = object.entry("permissions").or_insert_with(|| json!({}));
            if !permissions.is_object() {
                *permissions = json!({});
            }
            let list = permissions.as_object_mut().unwrap().entry(kind).or_insert_with(|| json!([]));
            if let Some(list) = list.as_array_mut() {
                if !list.iter().any(|item| item.as_str() == Some(&rule)) {
                    list.push(json!(rule));
                }
            }
        })
    }

    pub fn remove_rule(&mut self, scope: Scope, kind: &str, rule: &str) -> Result<bool, String> {
        let mut removed = false;
        let (kind, rule) = (kind.to_string(), rule.to_string());
        let found = &mut removed;
        self.update(scope, move |object| {
            if let Some(list) = object.get_mut("permissions").and_then(|p| p.get_mut(&kind)).and_then(Value::as_array_mut) {
                let before = list.len();
                list.retain(|item| item.as_str() != Some(&rule));
                *found = list.len() != before;
            }
        })?;
        Ok(removed)
    }

    /// The settings' `env`, set in this process so tools and hooks inherit it.
    pub fn apply_env(&self) {
        if let Some(Value::Object(env)) = self.get("/env") {
            for (key, value) in env {
                let text = match value {
                    Value::String(text) => text.clone(),
                    other => other.to_string(),
                };
                if !key.is_empty() && !key.contains('=') {
                    std::env::set_var(key, text);
                }
            }
        }
    }
}

/// The add-on file speaks the app's flat keys (permissionMode "allow"/"ask"); they become
/// Claude Code's shape here.
fn normalize(scope: Scope, mut value: Value) -> Value {
    if scope != Scope::Addon {
        return value;
    }
    if let Some(mode) = value.get("permissionMode").and_then(Value::as_str).map(str::to_string) {
        let mode = match mode.as_str() {
            "allow" => "bypassPermissions".to_string(),
            "ask" => "default".to_string(),
            other => other.to_string(),
        };
        let object = value.as_object_mut().unwrap();
        object.remove("permissionMode");
        let permissions = object.entry("permissions").or_insert_with(|| json!({}));
        if let Some(permissions) = permissions.as_object_mut() {
            permissions.entry("defaultMode").or_insert(json!(mode));
        }
    }
    value
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn merging() {
        let mut base = json!({"model": "a", "permissions": {"allow": ["Read"], "defaultMode": "default"}, "env": {"A": "1"}});
        merge(&mut base, &json!({"model": "b", "permissions": {"allow": ["Bash(ls)", "Read"]}, "env": {"B": "2"}}));
        assert_eq!(
            base,
            json!({"model": "b", "permissions": {"allow": ["Read", "Bash(ls)"], "defaultMode": "default"}, "env": {"A": "1", "B": "2"}})
        );
    }

    #[test]
    fn layers_and_rules() {
        let dir = std::env::temp_dir().join(format!("claude-settings-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(dir.join("proj/.claude")).unwrap();
        std::fs::create_dir_all(dir.join("proj/sub")).unwrap();
        std::fs::write(dir.join("proj/.claude/settings.json"), r#"{"model": "project-model", "permissions": {"deny": ["Bash(rm:*)"]}}"#).unwrap();
        std::fs::write(dir.join("addon.json"), r#"{"provider": "openai", "permissionMode": "allow"}"#).unwrap();
        std::env::set_var("CLAUDE_CODE_CONFIG", dir.join("addon.json"));
        std::env::set_var("CLAUDE_CONFIG_DIR", dir.join("home/.claude"));
        let mut settings = Settings::load(&dir.join("proj/sub"), Some(r#"{"model": "cli-model"}"#)).unwrap();
        assert_eq!(settings.project_root, dir.join("proj"));
        assert_eq!(settings.str("/model").as_deref(), Some("cli-model"));
        assert_eq!(settings.str("/provider").as_deref(), Some("openai"));
        assert_eq!(settings.str("/permissions/defaultMode").as_deref(), Some("bypassPermissions"));
        settings.add_rule(Scope::Local, "allow", "Bash(npm test)").unwrap();
        assert_eq!(settings.strings("/permissions/allow"), ["Bash(npm test)"]);
        assert_eq!(settings.strings("/permissions/deny"), ["Bash(rm:*)"]);
        assert!(settings.remove_rule(Scope::Local, "allow", "Bash(npm test)").unwrap());
        assert!(settings.strings("/permissions/allow").is_empty());
        std::env::remove_var("CLAUDE_CODE_CONFIG");
        std::env::remove_var("CLAUDE_CONFIG_DIR");
    }
}
