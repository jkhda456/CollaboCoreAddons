//! Server-sent events (text/event-stream), as both APIs stream their answers.
use std::io::{self, BufRead};

pub struct Event {
    pub event: String,
    pub data: String,
}

pub struct Events<R> {
    reader: R,
}

pub fn events<R: BufRead>(reader: R) -> Events<R> {
    Events { reader }
}

impl<R: BufRead> Iterator for Events<R> {
    type Item = io::Result<Event>;

    fn next(&mut self) -> Option<io::Result<Event>> {
        let (mut event, mut data, mut any) = (String::new(), String::new(), false);
        loop {
            let mut raw = Vec::new();
            match self.reader.read_until(b'\n', &mut raw) {
                Err(error) => return Some(Err(error)),
                Ok(0) => return any.then(|| Ok(Event { event, data })),
                Ok(_) => {}
            }
            let line = String::from_utf8_lossy(&raw);
            let line = line.trim_end_matches(['\n', '\r']);
            if line.is_empty() {
                if any {
                    return Some(Ok(Event { event, data }));
                }
                continue;
            }
            if line.starts_with(':') {
                continue;
            }
            let (field, value) = line.split_once(':').unwrap_or((line, ""));
            let value = value.strip_prefix(' ').unwrap_or(value);
            match field {
                "event" => {
                    event = value.to_string();
                    any = true;
                }
                "data" => {
                    if !data.is_empty() {
                        data.push('\n');
                    }
                    data.push_str(value);
                    any = true;
                }
                _ => {}
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_events() {
        let wire = b": ping\r\nevent: a\r\ndata: {\"x\":1}\r\n\r\ndata: one\ndata:two\n\ndata: [DONE]";
        let got: Vec<(String, String)> = events(&wire[..]).map(|e| e.unwrap()).map(|e| (e.event, e.data)).collect();
        assert_eq!(
            got,
            [("a".into(), "{\"x\":1}".into()), ("".into(), "one\ntwo".into()), ("".into(), "[DONE]".into())]
        );
    }
}
