//! WebFetch: a page through the host's request API, reduced to text.
use std::io::Read;

use serde_json::Value;

use super::{fail, Outcome};
use crate::bridge;

const MAX_CHARS: usize = 100_000;

/// HTML to readable text: scripts, styles and navigation dropped, headings and list items kept
/// as Markdown, tags removed, entities decoded, blank runs collapsed.
pub fn html_to_text(html: &str) -> String {
    let dropped = regex::Regex::new(r"(?is)<(script|style|noscript|svg|head|nav|footer|iframe)\b.*?</(script|style|noscript|svg|head|nav|footer|iframe)>|<!--.*?-->").unwrap();
    let text = dropped.replace_all(html, "");
    let headings = regex::Regex::new(r"(?i)<h([1-6])[^>]*>").unwrap();
    let text = headings.replace_all(&text, |caps: &regex::Captures| format!("\n{} ", "#".repeat(caps[1].parse().unwrap_or(1))));
    let items = regex::Regex::new(r"(?i)<li[^>]*>").unwrap();
    let text = items.replace_all(&text, "\n- ");
    let breaks = regex::Regex::new(r"(?i)<(br|/p|/div|/h[1-6]|/tr|/pre|/table|/section|/article|/ul|/ol)[^>]*>").unwrap();
    let text = breaks.replace_all(&text, "\n");
    let links = regex::Regex::new(r#"(?is)<a\s[^>]*href="([^"]+)"[^>]*>(.*?)</a>"#).unwrap();
    let text = links.replace_all(&text, "[$2]($1)");
    let tags = regex::Regex::new(r"(?s)<[^>]*>").unwrap();
    let text = tags.replace_all(&text, "");
    let numeric = regex::Regex::new(r"&#(x?)([0-9a-fA-F]+);").unwrap();
    let text = numeric.replace_all(&text, |caps: &regex::Captures| {
        let radix = if caps[1].is_empty() { 10 } else { 16 };
        u32::from_str_radix(&caps[2], radix).ok().and_then(char::from_u32).map(String::from).unwrap_or_default()
    });
    let text = text
        .replace("&nbsp;", " ")
        .replace("&lt;", "<")
        .replace("&gt;", ">")
        .replace("&quot;", "\"")
        .replace("&apos;", "'")
        .replace("&amp;", "&");
    let mut out = String::new();
    let mut blank = 0;
    for line in text.lines() {
        let line = line.split_whitespace().collect::<Vec<_>>().join(" ");
        if line.is_empty() {
            blank += 1;
            if blank > 1 {
                continue;
            }
        } else {
            blank = 0;
        }
        out.push_str(&line);
        out.push('\n');
    }
    out.trim().to_string()
}

pub fn fetch(input: &Value) -> Outcome {
    let Some(url) = input.get("url").and_then(Value::as_str) else { return fail("InputValidationError: url is required") };
    if !url.starts_with("http://") && !url.starts_with("https://") {
        return fail("Invalid URL: it must start with http:// or https://");
    }
    let response = match bridge::request("GET", url, &[("accept", "text/html,text/markdown,text/plain,application/json;q=0.9,*/*;q=0.8")], &[]) {
        Ok(response) => response,
        Err(error) => return fail(format!("Failed to fetch {url}: {error}")),
    };
    let status = response.status;
    let kind = response.header("content-type").unwrap_or("").to_lowercase();
    let mut bytes = Vec::new();
    if let Err(error) = response.body.take(10 * 1024 * 1024).read_to_end(&mut bytes) {
        return fail(format!("Failed to fetch {url}: {error}"));
    }
    if !(kind.is_empty() || kind.starts_with("text/") || kind.contains("json") || kind.contains("xml") || kind.contains("javascript")) {
        return fail(format!("{url} is {kind} ({} bytes), not text", bytes.len()));
    }
    let text = String::from_utf8_lossy(&bytes);
    let text = if kind.contains("html") || text.trim_start().starts_with("<!") { html_to_text(&text) } else { text.into_owned() };
    let body: String = text.chars().take(MAX_CHARS).collect();
    let cut = if text.chars().count() > MAX_CHARS { "\n[… the rest of the page is cut]" } else { "" };
    let prompt = input.get("prompt").and_then(Value::as_str).map(|p| format!("\n\nWhat to look for: {p}")).unwrap_or_default();
    Outcome {
        text: format!("{}Content of {url}:\n\n{body}{cut}{prompt}", if status >= 400 { format!("HTTP {status}\n") } else { String::new() }),
        is_error: status >= 400,
        ..Default::default()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn html() {
        let text = html_to_text("<html><head><title>x</title></head><body><nav>menu</nav><script>var a;</script><h1>Hi &amp; bye</h1><p>one<br>two &#8212; <a href=\"/x\">link</a></p><ul><li>a</li><li>b</li></ul></body></html>");
        assert_eq!(text, "# Hi & bye\none\ntwo — [link](/x)\n\n- a\n- b");
    }
}
