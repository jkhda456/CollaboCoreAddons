//! Plugins, as Claude Code has them: a folder with .claude-plugin/plugin.json and any of
//! commands/ (→ /plugin:command), agents/, skills/, hooks/hooks.json and .mcp.json, where
//! ${CLAUDE_PLUGIN_ROOT} names the folder.
//!
//! They come from marketplaces — a folder or git repository with .claude-plugin/marketplace.json
//! listing plugins and their source — and are installed as copies:
//!
//!   ~/.claude/plugins/known_marketplaces.json   name → where the marketplace is
//!   ~/.claude/plugins/marketplaces/<name>/       a marketplace cloned from git
//!   ~/.claude/plugins/installed_plugins.json    "plugin@marketplace" → its copy
//!   ~/.claude/plugins/cache/<marketplace>/<plugin>/
//!
//! Settings `enabledPlugins` {"plugin@marketplace": true|false} turn them on and off, and
//! --plugin-dir DIR loads one for a single run. (Claude Code's TypeScript "mods" need a
//! JavaScript engine and are not supported here.)
use std::path::{Path, PathBuf};
use std::sync::Mutex;

use serde_json::{json, Map, Value};

use crate::settings::{Scope, Settings};

#[derive(Clone, Debug)]
pub struct Plugin {
    /// "name@marketplace", or the name alone for --plugin-dir.
    pub id: String,
    pub name: String,
    pub description: String,
    pub version: String,
    pub root: PathBuf,
    pub enabled: bool,
}

/// The plugins this process loaded (set at start and after /plugin changes).
static LOADED: Mutex<Vec<Plugin>> = Mutex::new(Vec::new());

pub fn loaded() -> Vec<Plugin> {
    LOADED.lock().unwrap().iter().filter(|p| p.enabled).cloned().collect()
}

fn dir() -> PathBuf {
    crate::settings::user_dir().join("plugins")
}

fn read_json(path: &Path) -> Value {
    std::fs::read_to_string(path).ok().and_then(|t| serde_json::from_str(&t).ok()).unwrap_or_else(|| json!({}))
}

fn write_json(path: &Path, value: &Value) -> Result<(), String> {
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent).map_err(|e| e.to_string())?;
    }
    std::fs::write(path, serde_json::to_string_pretty(value).unwrap() + "\n").map_err(|e| format!("{}: {e}", path.display()))
}

fn manifest(root: &Path) -> Value {
    read_json(&root.join(".claude-plugin/plugin.json"))
}

fn describe(id: &str, root: &Path, enabled: bool) -> Plugin {
    let manifest = manifest(root);
    Plugin {
        id: id.to_string(),
        name: manifest["name"].as_str().map(str::to_string).unwrap_or_else(|| id.split('@').next().unwrap_or(id).to_string()),
        description: manifest["description"].as_str().unwrap_or("").to_string(),
        version: manifest["version"].as_str().unwrap_or("").to_string(),
        root: root.to_path_buf(),
        enabled,
    }
}

/// Every installed plugin (enabled or not) and the --plugin-dir ones; remembered for loaded().
pub fn load(settings: &Settings, extra_dirs: &[PathBuf]) -> Vec<Plugin> {
    let installed = read_json(&dir().join("installed_plugins.json"));
    let enabled = settings.get("/enabledPlugins").cloned().unwrap_or_else(|| json!({}));
    let mut plugins = Vec::new();
    for (id, entry) in installed.as_object().into_iter().flatten() {
        let Some(path) = entry["installPath"].as_str() else { continue };
        let on = enabled.get(id).and_then(Value::as_bool).unwrap_or(true);
        plugins.push(describe(id, Path::new(path), on));
    }
    for extra in extra_dirs {
        let name = manifest(extra)["name"].as_str().map(str::to_string).unwrap_or_else(|| extra.file_name().unwrap_or_default().to_string_lossy().into_owned());
        plugins.push(describe(&name, extra, true));
    }
    *LOADED.lock().unwrap() = plugins.clone();
    plugins
}

/// The marketplaces known here: name → root folder.
pub fn marketplaces() -> Vec<(String, PathBuf)> {
    let known = read_json(&dir().join("known_marketplaces.json"));
    known.as_object().into_iter().flatten().filter_map(|(name, entry)| Some((name.clone(), PathBuf::from(entry["installLocation"].as_str()?)))).collect()
}

/// Registers a marketplace from a folder or a git URL (cloned); its name.
pub fn add_marketplace(source: &str, cwd: &Path) -> Result<String, String> {
    let local = crate::permissions::normalize(Path::new(&crate::permissions::expand_home(source)), cwd);
    let root = if local.join(".claude-plugin/marketplace.json").is_file() {
        local
    } else if source.contains("://") || source.ends_with(".git") || source.starts_with("git@") || (source.split('/').count() == 2 && !local.exists()) {
        let url = if source.contains("://") || source.starts_with("git@") { source.to_string() } else { format!("https://github.com/{source}.git") };
        let name = url.trim_end_matches(".git").rsplit('/').next().unwrap_or("marketplace").to_string();
        let target = dir().join("marketplaces").join(&name);
        if !target.exists() {
            std::fs::create_dir_all(dir().join("marketplaces")).map_err(|e| e.to_string())?;
            let status = std::process::Command::new("git").args(["clone", "--depth", "1", &url]).arg(&target).status().map_err(|e| format!("git clone {url}: {e} (is git in the sandbox? it comes with the tools image)"))?;
            if !status.success() {
                return Err(format!("git clone {url} failed"));
            }
        }
        target
    } else {
        return Err(format!("{source}: no .claude-plugin/marketplace.json there, and not a git URL"));
    };
    let listing = read_json(&root.join(".claude-plugin/marketplace.json"));
    let name = listing["name"].as_str().ok_or("marketplace.json has no name")?.to_string();
    let path = dir().join("known_marketplaces.json");
    let mut known = read_json(&path);
    known[&name] = json!({"source": source, "installLocation": root.display().to_string()});
    write_json(&path, &known)?;
    Ok(name)
}

/// A marketplace's plugins: (name, description, source folder if local).
pub fn marketplace_plugins(root: &Path) -> Vec<(String, String, Option<PathBuf>)> {
    let listing = read_json(&root.join(".claude-plugin/marketplace.json"));
    listing["plugins"]
        .as_array()
        .into_iter()
        .flatten()
        .filter_map(|p| {
            let name = p["name"].as_str()?.to_string();
            let source = match &p["source"] {
                Value::String(relative) => Some(root.join(relative.trim_start_matches("./"))),
                _ => None,
            };
            Some((name, p["description"].as_str().unwrap_or("").to_string(), source))
        })
        .collect()
}

fn copy_dir(from: &Path, to: &Path) -> std::io::Result<()> {
    std::fs::create_dir_all(to)?;
    for entry in std::fs::read_dir(from)? {
        let entry = entry?;
        let target = to.join(entry.file_name());
        if entry.file_type()?.is_dir() {
            if entry.file_name() != ".git" {
                copy_dir(&entry.path(), &target)?;
            }
        } else {
            std::fs::copy(entry.path(), &target)?;
        }
    }
    Ok(())
}

/// Installs "plugin@marketplace" (or "plugin" when one marketplace has it) and enables it.
pub fn install(spec: &str, settings: &mut Settings) -> Result<String, String> {
    let (name, market) = match spec.split_once('@') {
        Some((name, market)) => (name.to_string(), Some(market.to_string())),
        None => (spec.to_string(), None),
    };
    let markets = marketplaces();
    let candidates: Vec<(String, PathBuf)> = markets
        .iter()
        .filter(|(m, _)| market.as_ref().is_none_or(|want| want == m))
        .filter_map(|(m, root)| marketplace_plugins(root).into_iter().find(|(n, _, _)| *n == name).and_then(|(_, _, source)| Some((m.clone(), source?))))
        .collect();
    let (market, source) = match candidates.as_slice() {
        [one] => one.clone(),
        [] => return Err(format!("no plugin {spec} in the known marketplaces (/plugin marketplace add PATH-or-URL first)")),
        _ => return Err(format!("{name} is in several marketplaces: name one, as {name}@MARKETPLACE")),
    };
    let target = dir().join("cache").join(&market).join(&name);
    let _ = std::fs::remove_dir_all(&target);
    copy_dir(&source, &target).map_err(|e| format!("copying {}: {e}", source.display()))?;
    let id = format!("{name}@{market}");
    let path = dir().join("installed_plugins.json");
    let mut installed = read_json(&path);
    installed[&id] = json!({"installPath": target.display().to_string(), "installedAt": crate::util::timestamp()});
    write_json(&path, &installed)?;
    set_enabled(&id, true, settings)?;
    Ok(id)
}

pub fn uninstall(id: &str, settings: &mut Settings) -> Result<(), String> {
    let path = dir().join("installed_plugins.json");
    let mut installed = read_json(&path);
    let entry = installed.as_object_mut().and_then(|o| o.remove(id)).ok_or_else(|| format!("{id} is not installed"))?;
    if let Some(root) = entry["installPath"].as_str() {
        let _ = std::fs::remove_dir_all(root);
    }
    write_json(&path, &installed)?;
    let id = id.to_string();
    settings.update(Scope::User, move |object| {
        if let Some(map) = object.get_mut("enabledPlugins").and_then(Value::as_object_mut) {
            map.remove(&id);
        }
    })?;
    Ok(())
}

pub fn set_enabled(id: &str, on: bool, settings: &mut Settings) -> Result<(), String> {
    settings.set_value(Scope::User, &format!("/enabledPlugins/{id}"), Some(json!(on))).map(|_| ())
}

fn substitute(value: &Value, root: &Path) -> Value {
    match value {
        Value::String(text) => json!(text.replace("${CLAUDE_PLUGIN_ROOT}", &root.display().to_string())),
        Value::Array(items) => Value::Array(items.iter().map(|v| substitute(v, root)).collect()),
        Value::Object(map) => Value::Object(map.iter().map(|(k, v)| (k.clone(), substitute(v, root))).collect()),
        other => other.clone(),
    }
}

/// The enabled plugins' hooks, merged into `hooks` (the settings' own come first).
pub fn merge_hooks(hooks: &mut Value) {
    for plugin in loaded() {
        let file = read_json(&plugin.root.join("hooks/hooks.json"));
        let Some(events) = file.get("hooks").and_then(Value::as_object) else { continue };
        for (event, groups) in events {
            let groups = substitute(groups, &plugin.root);
            if !hooks.is_object() {
                *hooks = json!({});
            }
            let list = hooks.as_object_mut().unwrap().entry(event.clone()).or_insert_with(|| json!([]));
            if let (Some(list), Some(groups)) = (list.as_array_mut(), groups.as_array()) {
                list.extend(groups.iter().cloned());
            }
        }
    }
}

/// The enabled plugins' MCP servers: (name, "plugin:<id>", config).
pub fn mcp_servers() -> Vec<(String, String, Value)> {
    let mut servers = Vec::new();
    for plugin in loaded() {
        let file = read_json(&plugin.root.join(".mcp.json"));
        let map: Map<String, Value> = file.get("mcpServers").or(Some(&file)).and_then(Value::as_object).cloned().unwrap_or_default();
        for (name, config) in map {
            if config.is_object() {
                servers.push((format!("{}:{name}", plugin.name), format!("plugin:{}", plugin.id), substitute(&config, &plugin.root)));
            }
        }
    }
    servers
}

/// `claude plugin …` and `/plugin …`: what to print.
pub fn command(words: &[&str], settings: &mut Settings, cwd: &Path, extra_dirs: &[PathBuf]) -> Result<String, String> {
    let reload = |settings: &Settings| {
        load(settings, extra_dirs);
    };
    match words {
        [] | ["list"] => {
            let plugins = load(settings, extra_dirs);
            let mut out = String::new();
            if plugins.is_empty() {
                out.push_str("No plugins installed.\n");
            }
            for p in &plugins {
                out.push_str(&format!(
                    "{} {}{}  {}\n",
                    if p.enabled { "✔" } else { "✘" },
                    p.id,
                    if p.version.is_empty() { String::new() } else { format!(" v{}", p.version) },
                    p.description
                ));
            }
            for (name, root) in marketplaces() {
                let available = marketplace_plugins(&root);
                out.push_str(&format!("\nMarketplace {name} ({} plugins): {}\n", available.len(), root.display()));
                for (plugin, description, _) in available {
                    let id = format!("{plugin}@{name}");
                    let mark = if plugins.iter().any(|p| p.id == id) { "installed" } else { "" };
                    out.push_str(&format!("  {plugin:<28} {mark:<10} {}\n", crate::term::truncate(&description, 70)));
                }
            }
            out.push_str("\n/plugin marketplace add PATH-or-GIT-URL · /plugin install NAME@MARKETPLACE · /plugin enable|disable|uninstall NAME@MARKETPLACE");
            Ok(out)
        }
        ["marketplace", "add", source] => add_marketplace(source, cwd).map(|name| format!("Added marketplace {name}: /plugin install NAME@{name}")),
        ["marketplace", "list"] | ["marketplace"] => Ok(marketplaces().iter().map(|(n, r)| format!("{n}  {}", r.display())).collect::<Vec<_>>().join("\n")),
        ["marketplace", "remove", name] => {
            let path = dir().join("known_marketplaces.json");
            let mut known = read_json(&path);
            known.as_object_mut().and_then(|o| o.remove(*name)).ok_or_else(|| format!("no marketplace {name}"))?;
            write_json(&path, &known)?;
            Ok(format!("Removed marketplace {name}"))
        }
        ["install", spec] => {
            let id = install(spec, settings)?;
            reload(settings);
            Ok(format!("Installed and enabled {id}. Its commands, agents, skills and hooks are in effect now; MCP servers from it connect at the next start."))
        }
        ["uninstall", id] => {
            uninstall(id, settings)?;
            reload(settings);
            Ok(format!("Uninstalled {id}"))
        }
        ["enable", id] | ["disable", id] => {
            let on = words[0] == "enable";
            set_enabled(id, on, settings)?;
            reload(settings);
            Ok(format!("{} {id}", if on { "Enabled" } else { "Disabled" }))
        }
        _ => Err("usage: plugin [list] | marketplace add PATH-or-URL | marketplace list | marketplace remove NAME | install NAME@MARKETPLACE | uninstall|enable|disable NAME@MARKETPLACE".into()),
    }
}
