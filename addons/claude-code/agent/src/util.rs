//! Small things several modules need: random ids, time stamps, base64.
use std::io::Read;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

pub fn random_bytes(count: usize) -> Vec<u8> {
    let mut bytes = vec![0u8; count];
    if std::fs::File::open("/dev/urandom").and_then(|mut file| file.read_exact(&mut bytes)).is_err() {
        // Only if /dev/urandom is missing: the clock and the pid, spread out.
        let mut seed = SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_nanos() as u64).unwrap_or(1) ^ (std::process::id() as u64) << 32;
        for byte in bytes.iter_mut() {
            seed ^= seed << 13;
            seed ^= seed >> 7;
            seed ^= seed << 17;
            *byte = seed as u8;
        }
    }
    bytes
}

pub fn random_hex(count: usize) -> String {
    random_bytes(count).iter().map(|byte| format!("{byte:02x}")).collect()
}

/// A version 4 UUID.
pub fn uuid() -> String {
    let mut b = random_bytes(16);
    b[6] = (b[6] & 0x0f) | 0x40;
    b[8] = (b[8] & 0x3f) | 0x80;
    let hex: String = b.iter().map(|byte| format!("{byte:02x}")).collect();
    format!("{}-{}-{}-{}-{}", &hex[..8], &hex[8..12], &hex[12..16], &hex[16..20], &hex[20..])
}

/// (year, month, day, hour, minute, second) in UTC.
pub fn civil(time: SystemTime) -> (i64, u32, u32, u32, u32, u32) {
    let seconds = time.duration_since(UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0) as i64;
    let (days, rest) = (seconds.div_euclid(86_400), seconds.rem_euclid(86_400));
    // Howard Hinnant's civil_from_days.
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z.rem_euclid(146_097);
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let day = (doy - (153 * mp + 2) / 5 + 1) as u32;
    let month = if mp < 10 { mp + 3 } else { mp - 9 } as u32;
    let year = yoe + era * 400 + i64::from(month <= 2);
    (year, month, day, (rest / 3600) as u32, (rest % 3600 / 60) as u32, (rest % 60) as u32)
}

pub fn today() -> String {
    let (y, m, d, ..) = civil(SystemTime::now());
    format!("{y:04}-{m:02}-{d:02}")
}

/// ISO 8601 in UTC, with milliseconds.
pub fn timestamp() -> String {
    let now = SystemTime::now();
    let (y, mo, d, h, mi, s) = civil(now);
    let millis = now.duration_since(UNIX_EPOCH).map(|d| d.subsec_millis()).unwrap_or(0);
    format!("{y:04}-{mo:02}-{d:02}T{h:02}:{mi:02}:{s:02}.{millis:03}Z")
}

pub fn base64(bytes: &[u8]) -> String {
    const ALPHABET: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut out = String::with_capacity(bytes.len().div_ceil(3) * 4);
    for chunk in bytes.chunks(3) {
        let n = chunk.iter().enumerate().fold(0u32, |n, (i, byte)| n | (*byte as u32) << (16 - 8 * i));
        for i in 0..4 {
            out.push(match i <= chunk.len() {
                true => ALPHABET[(n >> (18 - 6 * i) & 63) as usize] as char,
                false => '=',
            });
        }
    }
    out
}

/// "3s", "1m 20s", "2h 5m".
pub fn duration(elapsed: Duration) -> String {
    let seconds = elapsed.as_secs();
    match seconds {
        0..=59 => format!("{seconds}s"),
        60..=3599 => format!("{}m {}s", seconds / 60, seconds % 60),
        _ => format!("{}h {}m", seconds / 3600, seconds % 3600 / 60),
    }
}

/// "950", "1.2k", "34k".
pub fn tokens(count: u64) -> String {
    match count {
        0..=999 => count.to_string(),
        1000..=9999 => format!("{:.1}k", count as f64 / 1000.0),
        _ => format!("{}k", count / 1000),
    }
}

/// A Markdown file's YAML front matter (flat `key: value` lines) and its body.
pub fn front_matter(text: &str) -> (std::collections::HashMap<String, String>, String) {
    let mut fields = std::collections::HashMap::new();
    let text = text.trim_start_matches('\u{feff}');
    let Some(rest) = text.strip_prefix("---").filter(|rest| rest.starts_with('\n') || rest.starts_with("\r\n")) else {
        return (fields, text.to_string());
    };
    let Some(end) = rest.find("\n---") else { return (fields, text.to_string()) };
    for line in rest[..end].lines() {
        if let Some((key, value)) = line.split_once(':') {
            let value = value.trim().trim_matches('"').trim_matches('\'').to_string();
            if !key.trim().is_empty() && !key.starts_with(' ') {
                fields.insert(key.trim().to_string(), value);
            }
        }
    }
    let body = rest[end + 4..].trim_start_matches(['-']).trim_start_matches(['\r', '\n']).to_string();
    (fields, body)
}

/// "Read, Grep, Bash(git:*)" → ["Read", "Grep", "Bash(git:*)"] (commas inside parentheses kept).
pub fn split_list(text: &str) -> Vec<String> {
    let (mut items, mut current, mut depth) = (Vec::new(), String::new(), 0);
    for c in text.chars() {
        match c {
            '(' => depth += 1,
            ')' => depth -= 1,
            ',' if depth == 0 => {
                items.push(std::mem::take(&mut current));
                continue;
            }
            _ => {}
        }
        current.push(c);
    }
    items.push(current);
    items.into_iter().map(|item| item.trim().trim_matches(['[', ']', '"']).to_string()).filter(|item| !item.is_empty()).collect()
}

#[cfg(test)]
mod tests {
    #[test]
    fn front_matter_and_lists() {
        let (fields, body) = super::front_matter("---\nname: reviewer\ndescription: \"Reviews code\"\ntools: Read, Bash(git diff:*)\n---\nYou review.\n");
        assert_eq!(fields["name"], "reviewer");
        assert_eq!(fields["description"], "Reviews code");
        assert_eq!(super::split_list(&fields["tools"]), ["Read", "Bash(git diff:*)"]);
        assert_eq!(body, "You review.\n");
        assert_eq!(super::front_matter("no front matter").1, "no front matter");
    }

    use super::*;

    #[test]
    fn ids_and_encodings() {
        let id = uuid();
        assert_eq!(id.len(), 36);
        assert_eq!(&id[14..15], "4");
        assert_eq!(base64(b"hello!?"), "aGVsbG8hPw==");
        assert_eq!(civil(UNIX_EPOCH + Duration::from_secs(1_700_000_000)), (2023, 11, 14, 22, 13, 20));
        assert_eq!(tokens(12_345), "12k");
        assert_eq!(duration(Duration::from_secs(80)), "1m 20s");
    }
}
