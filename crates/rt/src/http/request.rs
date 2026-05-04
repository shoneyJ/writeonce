//! Incremental HTTP/1.1 request parser.
//!
//! Adapted from `reference/crates/wo-http/src/request.rs`. v1 only parsed
//! request headers (the v1 blog is read-only HTML). The phase-04 cutover
//! needs JSON request bodies, so this parser also drains a
//! `Content-Length`-delimited body. Chunked transfer encoding is not
//! supported (no sample sends one — see `docs/plan/03-hand-rolled-http.md`).

use std::collections::HashMap;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Method {
    Get,
    Post,
    Patch,
    Put,
    Delete,
    Head,
    Options,
    Other,
}

impl Method {
    fn parse(token: &str) -> Method {
        match token {
            "GET"     => Method::Get,
            "POST"    => Method::Post,
            "PATCH"   => Method::Patch,
            "PUT"     => Method::Put,
            "DELETE"  => Method::Delete,
            "HEAD"    => Method::Head,
            "OPTIONS" => Method::Options,
            _         => Method::Other,
        }
    }
}

#[derive(Debug, Clone)]
pub struct Request {
    pub method:  Method,
    pub path:    String,
    pub query:   Option<String>,
    pub headers: HashMap<String, String>,
    pub body:    Vec<u8>,
}

pub enum ParseResult {
    Complete(Request),
    Incomplete,
    Error(String),
}

const MAX_HEADER_BYTES: usize = 8 * 1024;
const MAX_BODY_BYTES:   usize = 16 * 1024 * 1024;

/// Try to parse a complete HTTP/1.1 request out of `buf`.
/// Returns `Complete(req, bytes_consumed)` once headers + body are present.
pub fn parse(buf: &[u8]) -> ParseResult {
    let header_end = match find_header_end(buf) {
        Some(p) => p,
        None    => {
            if buf.len() > MAX_HEADER_BYTES {
                return ParseResult::Error("request headers too large".into());
            }
            return ParseResult::Incomplete;
        }
    };

    let header_str = match std::str::from_utf8(&buf[..header_end]) {
        Ok(s)  => s,
        Err(_) => return ParseResult::Error("non-UTF-8 in headers".into()),
    };

    let mut lines = header_str.lines();

    let request_line = match lines.next() {
        Some(l) => l,
        None    => return ParseResult::Error("empty request".into()),
    };
    let mut parts = request_line.split_whitespace();
    let method = match parts.next() {
        Some(t) => Method::parse(t),
        None    => return ParseResult::Error("missing method".into()),
    };
    let raw_path = match parts.next() {
        Some(p) => p,
        None    => return ParseResult::Error("missing path".into()),
    };
    let (path, query) = match raw_path.split_once('?') {
        Some((p, q)) => (p.to_string(), Some(q.to_string())),
        None         => (raw_path.to_string(), None),
    };

    let mut headers = HashMap::new();
    for line in lines {
        if line.is_empty() { break; }
        if let Some((k, v)) = line.split_once(':') {
            headers.insert(k.trim().to_ascii_lowercase(), v.trim().to_string());
        }
    }

    let body_len: usize = headers
        .get("content-length")
        .and_then(|v| v.parse().ok())
        .unwrap_or(0);
    if body_len > MAX_BODY_BYTES {
        return ParseResult::Error("Content-Length exceeds limit".into());
    }

    let header_bytes = header_end + 4; // include the trailing \r\n\r\n
    let total = header_bytes + body_len;
    if buf.len() < total {
        return ParseResult::Incomplete;
    }

    let body = buf[header_bytes..total].to_vec();
    ParseResult::Complete(Request { method, path, query, headers, body })
}

fn find_header_end(buf: &[u8]) -> Option<usize> {
    buf.windows(4).position(|w| w == b"\r\n\r\n")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_simple_get() {
        let raw = b"GET /api/articles HTTP/1.1\r\nHost: localhost\r\n\r\n";
        let ParseResult::Complete(req) = parse(raw) else { panic!("expected Complete") };
        assert_eq!(req.method, Method::Get);
        assert_eq!(req.path, "/api/articles");
        assert!(req.query.is_none());
        assert!(req.body.is_empty());
    }

    #[test]
    fn parses_query_string() {
        let raw = b"GET /tag/rust?page=2 HTTP/1.1\r\n\r\n";
        let ParseResult::Complete(req) = parse(raw) else { panic!() };
        assert_eq!(req.path, "/tag/rust");
        assert_eq!(req.query.as_deref(), Some("page=2"));
    }

    #[test]
    fn parses_post_with_body() {
        let body = b"{\"title\":\"hi\"}";
        let mut raw = Vec::new();
        raw.extend_from_slice(b"POST /api/articles HTTP/1.1\r\n");
        raw.extend_from_slice(b"Host: localhost\r\n");
        raw.extend_from_slice(b"Content-Type: application/json\r\n");
        raw.extend_from_slice(format!("Content-Length: {}\r\n", body.len()).as_bytes());
        raw.extend_from_slice(b"\r\n");
        raw.extend_from_slice(body);
        let ParseResult::Complete(req) = parse(&raw) else { panic!() };
        assert_eq!(req.method, Method::Post);
        assert_eq!(req.body, body);
    }

    #[test]
    fn incomplete_when_body_truncated() {
        let raw = b"POST / HTTP/1.1\r\nContent-Length: 10\r\n\r\nshort";
        assert!(matches!(parse(raw), ParseResult::Incomplete));
    }

    #[test]
    fn incomplete_when_headers_truncated() {
        let raw = b"GET / HTTP/1.1\r\nHost: local";
        assert!(matches!(parse(raw), ParseResult::Incomplete));
    }

    #[test]
    fn parses_patch_method() {
        let raw = b"PATCH /api/articles/1 HTTP/1.1\r\nContent-Length: 0\r\n\r\n";
        let ParseResult::Complete(req) = parse(raw) else { panic!() };
        assert_eq!(req.method, Method::Patch);
    }
}
