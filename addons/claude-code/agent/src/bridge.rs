//! HTTP(S) through the host: the request API on vsock port 1080 (engine/src/http.rs), the one
//! hfetch speaks. The guest has no TLS and no route out of its own for this; the host makes the
//! request under the app's network policy, adds the app's API keys, and streams the answer back.
//!
//!   request   METHOD URL\n  name: value\n  content-length: N\n  \n  <N bytes>
//!   response  STATUS TEXT\n  name: value\n  \n  <hex-length>\n<bytes>…  0\n  OK\n
//!   failure   ERROR kind: message\n    (instead of the status line, or as the trailer)
//!
//! COLLABO_BRIDGE=tcp:HOST:PORT speaks the same protocol over TCP instead (tests on a
//! development machine); HFETCH_PORT picks another vsock port.
use std::fmt;
use std::io::{self, BufRead, BufReader, Read, Write};

const DEFAULT_PORT: u32 = 1080;

#[derive(Debug)]
pub struct Error {
    /// The host's kind (denied, timeout, network, header-not-allowed, …), or "io"/"protocol".
    pub kind: String,
    pub message: String,
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        write!(f, "{}: {}", self.kind, self.message)
    }
}

impl From<io::Error> for Error {
    fn from(error: io::Error) -> Error {
        Error { kind: "io".into(), message: error.to_string() }
    }
}

fn protocol(message: impl Into<String>) -> Error {
    Error { kind: "protocol".into(), message: message.into() }
}

trait Stream: Read + Write {}
impl<T: Read + Write> Stream for T {}

/// A connection whose reads and writes give up when the user interrupts (interrupt.rs): SIGINT
/// makes a blocked call return EINTR, which is otherwise retried.
struct Interruptible<S>(S);

fn interrupted() -> io::Error {
    io::Error::other("interrupted by the user")
}

impl<S: Read> Read for Interruptible<S> {
    fn read(&mut self, buffer: &mut [u8]) -> io::Result<usize> {
        loop {
            if crate::interrupt::is_set() {
                return Err(interrupted());
            }
            match self.0.read(buffer) {
                Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
                other => return other,
            }
        }
    }
}

impl<S: Write> Write for Interruptible<S> {
    fn write(&mut self, buffer: &[u8]) -> io::Result<usize> {
        loop {
            if crate::interrupt::is_set() {
                return Err(interrupted());
            }
            match self.0.write(buffer) {
                Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
                other => return other,
            }
        }
    }
    fn flush(&mut self) -> io::Result<()> {
        self.0.flush()
    }
}

pub struct Response {
    pub status: u16,
    pub headers: Vec<(String, String)>,
    pub body: Body,
}

impl Response {
    pub fn header(&self, name: &str) -> Option<&str> {
        self.headers.iter().find(|(key, _)| key.eq_ignore_ascii_case(name)).map(|(_, value)| value.as_str())
    }

    pub fn text(mut self) -> Result<String, Error> {
        let mut bytes = Vec::new();
        self.body.read_to_end(&mut bytes)?;
        Ok(String::from_utf8_lossy(&bytes).into_owned())
    }
}

/// The response body: the host's chunks, decoded. A failure after the head (the upstream
/// connection broke, the policy changed) is an io::Error carrying the host's message.
pub struct Body {
    reader: BufReader<Box<dyn Stream>>,
    left: usize,
    done: bool,
}

impl Read for Body {
    fn read(&mut self, buffer: &mut [u8]) -> io::Result<usize> {
        loop {
            if self.done || buffer.is_empty() {
                return Ok(0);
            }
            if self.left > 0 {
                let wanted = buffer.len().min(self.left);
                let read = self.reader.read(&mut buffer[..wanted])?;
                if read == 0 {
                    return Err(io::Error::new(io::ErrorKind::UnexpectedEof, "response cut off"));
                }
                self.left -= read;
                return Ok(read);
            }
            let line = read_line(&mut self.reader)?;
            let length = usize::from_str_radix(&line, 16)
                .map_err(|_| io::Error::new(io::ErrorKind::InvalidData, format!("malformed chunk length: {line}")))?;
            if length > 0 {
                self.left = length;
                continue;
            }
            self.done = true;
            let trailer = read_line(&mut self.reader)?;
            if trailer != "OK" {
                return Err(io::Error::other(trailer.strip_prefix("ERROR ").unwrap_or(&trailer).to_string()));
            }
            return Ok(0);
        }
    }
}

impl BufRead for Body {
    fn fill_buf(&mut self) -> io::Result<&[u8]> {
        // Only whole chunks are handed out: find the next non-empty one first.
        while !self.done && self.left == 0 {
            let line = read_line(&mut self.reader)?;
            let length = usize::from_str_radix(&line, 16)
                .map_err(|_| io::Error::new(io::ErrorKind::InvalidData, format!("malformed chunk length: {line}")))?;
            if length == 0 {
                self.done = true;
                let trailer = read_line(&mut self.reader)?;
                if trailer != "OK" {
                    return Err(io::Error::other(trailer.strip_prefix("ERROR ").unwrap_or(&trailer).to_string()));
                }
            } else {
                self.left = length;
            }
        }
        if self.done {
            return Ok(&[]);
        }
        let available = self.reader.fill_buf()?;
        if available.is_empty() {
            return Err(io::Error::new(io::ErrorKind::UnexpectedEof, "response cut off"));
        }
        let take = available.len().min(self.left);
        Ok(&available[..take])
    }

    fn consume(&mut self, amount: usize) {
        self.reader.consume(amount);
        self.left -= amount;
    }
}

fn read_line(reader: &mut impl BufRead) -> io::Result<String> {
    let mut line = Vec::new();
    if reader.read_until(b'\n', &mut line)? == 0 {
        return Err(io::Error::new(io::ErrorKind::UnexpectedEof, "the host closed the connection"));
    }
    if line.last() == Some(&b'\n') {
        line.pop();
    }
    Ok(String::from_utf8_lossy(&line).into_owned())
}

#[cfg(target_os = "linux")]
fn connect_vsock(port: u32) -> io::Result<std::fs::File> {
    use std::os::fd::FromRawFd;
    const AF_VSOCK: libc::c_int = 40;
    const VMADDR_CID_HOST: u32 = 2;
    // <linux/vm_sockets.h>'s sockaddr_vm.
    #[repr(C)]
    struct SockaddrVm {
        family: libc::sa_family_t,
        reserved1: u16,
        port: u32,
        cid: u32,
        flags: u8,
        zero: [u8; 3],
    }
    let address = SockaddrVm { family: AF_VSOCK as libc::sa_family_t, reserved1: 0, port, cid: VMADDR_CID_HOST, flags: 0, zero: [0; 3] };
    // SAFETY: plain socket calls; the descriptor is owned by the File on success and closed
    // here on failure.
    unsafe {
        let fd = libc::socket(AF_VSOCK, libc::SOCK_STREAM | libc::SOCK_CLOEXEC, 0);
        if fd < 0 {
            return Err(io::Error::last_os_error());
        }
        let result = libc::connect(
            fd,
            &address as *const SockaddrVm as *const libc::sockaddr,
            std::mem::size_of::<SockaddrVm>() as libc::socklen_t,
        );
        if result < 0 {
            let error = io::Error::last_os_error();
            libc::close(fd);
            return Err(error);
        }
        Ok(std::fs::File::from_raw_fd(fd))
    }
}

fn connect() -> Result<Box<dyn Stream>, Error> {
    if let Ok(spec) = std::env::var("COLLABO_BRIDGE") {
        if let Some(address) = spec.strip_prefix("tcp:") {
            return Ok(Box::new(Interruptible(std::net::TcpStream::connect(address)?)));
        }
    }
    let port = std::env::var("HFETCH_PORT").ok().and_then(|port| port.parse().ok()).unwrap_or(DEFAULT_PORT);
    #[cfg(target_os = "linux")]
    {
        connect_vsock(port).map(|file| Box::new(Interruptible(file)) as Box<dyn Stream>).map_err(|error| Error {
            kind: "io".into(),
            message: format!("cannot reach the host's request API on vsock port {port}: {error}"),
        })
    }
    #[cfg(not(target_os = "linux"))]
    {
        let _ = port;
        Err(Error { kind: "io".into(), message: "no vsock here; set COLLABO_BRIDGE=tcp:HOST:PORT".into() })
    }
}

/// Sends one request and returns once the response head has arrived.
pub fn request(method: &str, url: &str, headers: &[(&str, &str)], body: &[u8]) -> Result<Response, Error> {
    if url.contains([' ', '\r', '\n']) {
        return Err(protocol("the URL contains whitespace"));
    }
    let mut head = format!("{method} {url}\n");
    for (name, value) in headers {
        if name.contains(['\r', '\n', ':']) || value.contains(['\r', '\n']) {
            return Err(protocol(format!("header {name} contains a line break")));
        }
        head.push_str(&format!("{name}: {value}\n"));
    }
    if !body.is_empty() {
        head.push_str(&format!("content-length: {}\n", body.len()));
    }
    head.push('\n');

    let mut stream = connect()?;
    stream.write_all(head.as_bytes())?;
    stream.write_all(body)?;
    stream.flush()?;

    let mut reader = BufReader::with_capacity(64 * 1024, stream);
    let status_line = read_line(&mut reader)?;
    if let Some(failure) = status_line.strip_prefix("ERROR ") {
        let (kind, message) = failure.split_once(": ").unwrap_or(("error", failure));
        return Err(Error { kind: kind.into(), message: message.into() });
    }
    let status: u16 = status_line
        .split(' ')
        .next()
        .and_then(|code| code.parse().ok())
        .filter(|code| (100..600).contains(code))
        .ok_or_else(|| protocol(format!("malformed response: {status_line}")))?;
    let mut response_headers = Vec::new();
    loop {
        let line = read_line(&mut reader)?;
        if line.is_empty() {
            break;
        }
        if let Some((name, value)) = line.split_once(':') {
            response_headers.push((name.trim().to_lowercase(), value.trim().to_string()));
        }
    }
    Ok(Response { status, headers: response_headers, body: Body { reader, left: 0, done: false } })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn body(wire: &'static [u8]) -> Body {
        Body { reader: BufReader::new(Box::new(io::Cursor::new(wire.to_vec()))), left: 0, done: false }
    }

    #[test]
    fn chunks_are_joined() {
        let mut text = String::new();
        body(b"5\nhello1\n 5\nworld0\nOK\n").read_to_string(&mut text).unwrap();
        assert_eq!(text, "hello world");
    }

    #[test]
    fn lines_across_chunks() {
        let lines: Vec<String> = body(b"4\nab\nc3\nd\ne0\nOK\n").lines().map(Result::unwrap).collect();
        assert_eq!(lines, ["ab", "cd", "e"]);
    }

    #[test]
    fn a_failure_in_the_trailer_is_an_error() {
        let mut text = String::new();
        let error = body(b"2\nhi0\nERROR network: reset\n").read_to_string(&mut text).unwrap_err();
        assert_eq!(error.to_string(), "network: reset");
    }
}
