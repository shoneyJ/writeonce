use std::collections::HashMap;

/// HTTP method.
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum Method {
    Get,
    Post,
    Head,
    Unknown,
}

/// A parsed HTTP/1.1 request.
#[derive(Debug, Clone)]
pub struct Request {
    pub method: Method,
    pub path: String,
    pub query: Option<String>,
    pub headers: HashMap<String, String>,
}

/// Result of attempting to parse a request from a byte buffer.
pub enum ParseResult {
    /// Request fully parsed; returns the request and number of bytes consumed.
    Complete(Request, usize),
    /// Need more data.
    Incomplete,
    /// Malformed request.
    Error(String),
}

/// Parse an HTTP/1.1 request from a byte buffer.
///
/// Returns `ParseResult::Complete` when the full header has been received
/// (delimited by `\r\n\r\n`).
pub fn parse(buf: &[u8]) -> ParseResult {
    // Find the end of headers.
    let header_end = match find_header_end(buf) {
        Some(pos) => pos,
        None => {
            if buf.len() > 8192 {
                return ParseResult::Error("request too large".into());
            }
            return ParseResult::Incomplete;
        }
    };

    let header_str = match std::str::from_utf8(&buf[..header_end]) {
        Ok(s) => s,
        Err(_) => return ParseResult::Error("invalid UTF-8 in headers".into()),
    };

    let mut lines = header_str.lines();

    // Request line: "GET /path HTTP/1.1"
    let request_line = match lines.next() {
        Some(l) => l,
        None => return ParseResult::Error("empty request".into()),
    };

    let mut parts = request_line.split_whitespace();
    let method = match parts.next() {
        Some("GET") => Method::Get,
        Some("POST") => Method::Post,
        Some("HEAD") => Method::Head,
        Some(_) => Method::Unknown,
        None => return ParseResult::Error("missing method".into()),
    };

    let raw_path = match parts.next() {
        Some(p) => p,
        None => return ParseResult::Error("missing path".into()),
    };

    // Split path and query string.
    let (path, query) = match raw_path.split_once('?') {
        Some((p, q)) => (p.to_string(), Some(q.to_string())),
        None => (raw_path.to_string(), None),
    };

    // Parse headers.
    let mut headers = HashMap::new();
    for line in lines {
        if line.is_empty() {
            break;
        }
        if let Some((key, value)) = line.split_once(':') {
            headers.insert(
                key.trim().to_lowercase(),
                value.trim().to_string(),
            );
        }
    }

    // Bytes consumed: header + \r\n\r\n delimiter.
    let consumed = header_end + 4;

    ParseResult::Complete(Request { method, path, query, headers }, consumed)
}

fn find_header_end(buf: &[u8]) -> Option<usize> {
    for i in 0..buf.len().saturating_sub(3) {
        if &buf[i..i + 4] == b"\r\n\r\n" {
            return Some(i);
        }
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_simple_get() {
        let raw = b"GET /blog/linux-misc HTTP/1.1\r\nHost: localhost\r\n\r\n";
        match parse(raw) {
            ParseResult::Complete(req, consumed) => {
                assert_eq!(req.method, Method::Get);
                assert_eq!(req.path, "/blog/linux-misc");
                assert!(req.query.is_none());
                assert_eq!(req.headers.get("host").unwrap(), "localhost");
                assert_eq!(consumed, raw.len());
            }
            _ => panic!("expected Complete"),
        }
    }

    #[test]
    fn parse_with_query_string() {
        let raw = b"GET /tag/rust?page=2 HTTP/1.1\r\n\r\n";
        match parse(raw) {
            ParseResult::Complete(req, _) => {
                assert_eq!(req.path, "/tag/rust");
                assert_eq!(req.query.as_deref(), Some("page=2"));
            }
            _ => panic!("expected Complete"),
        }
    }

    #[test]
    fn parse_incomplete() {
        let raw = b"GET / HTTP/1.1\r\nHost: local";
        assert!(matches!(parse(raw), ParseResult::Incomplete));
    }

    #[test]
    fn parse_multiple_headers() {
        let raw = b"GET / HTTP/1.1\r\nHost: localhost\r\nAccept: text/html\r\nConnection: keep-alive\r\n\r\n";
        match parse(raw) {
            ParseResult::Complete(req, _) => {
                assert_eq!(req.headers.len(), 3);
                assert_eq!(req.headers.get("accept").unwrap(), "text/html");
                assert_eq!(req.headers.get("connection").unwrap(), "keep-alive");
            }
            _ => panic!("expected Complete"),
        }
    }
}
