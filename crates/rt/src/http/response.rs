//! HTTP/1.1 response builder + serializer.
//!
//! Adapted from `reference/crates/wo-http/src/response.rs`. Adds:
//!   * `Status` constants for the codes the REST samples assert on
//!     (200/201/204/400/404/405/500/501).
//!   * `Response::json(&serde_json::Value)` matching the cutover-handler
//!     shape in `docs/plan/04-cutover-remove-tokio-axum.md`.

use serde_json::Value;

#[derive(Debug, Clone, Copy)]
pub struct Status(pub u16, pub &'static str);

impl Status {
    pub const OK:                    Status = Status(200, "OK");
    pub const CREATED:               Status = Status(201, "Created");
    pub const NO_CONTENT:            Status = Status(204, "No Content");
    pub const BAD_REQUEST:           Status = Status(400, "Bad Request");
    pub const NOT_FOUND:             Status = Status(404, "Not Found");
    pub const METHOD_NOT_ALLOWED:    Status = Status(405, "Method Not Allowed");
    pub const INTERNAL_SERVER_ERROR: Status = Status(500, "Internal Server Error");
    pub const NOT_IMPLEMENTED:       Status = Status(501, "Not Implemented");
}

#[derive(Debug, Clone)]
pub struct Response {
    pub status:  Status,
    pub headers: Vec<(String, String)>,
    pub body:    Vec<u8>,
}

impl Response {
    pub fn status(s: Status) -> Self {
        Self { status: s, headers: Vec::new(), body: Vec::new() }
    }

    pub fn ok()        -> Self { Self::status(Status::OK) }
    pub fn created()   -> Self { Self::status(Status::CREATED) }
    pub fn no_content() -> Self { Self::status(Status::NO_CONTENT) }

    pub fn header(mut self, k: &str, v: &str) -> Self {
        self.headers.push((k.to_string(), v.to_string()));
        self
    }

    pub fn body(mut self, body: impl Into<Vec<u8>>) -> Self {
        self.body = body.into();
        self
    }

    /// Plain-text body with `Content-Type: text/plain; charset=utf-8`.
    pub fn text(self, body: impl Into<String>) -> Self {
        let body: String = body.into();
        self.header("Content-Type", "text/plain; charset=utf-8")
            .body(body.into_bytes())
    }

    /// JSON body with `Content-Type: application/json`.
    pub fn json(self, value: &Value) -> Self {
        let buf = serde_json::to_vec(value).unwrap_or_else(|_| b"null".to_vec());
        self.header("Content-Type", "application/json").body(buf)
    }

    /// Serialize to the wire format. Auto-injects `Content-Length` and
    /// `Connection: close` (no keep-alive in Stage 2).
    pub fn to_bytes(&self) -> Vec<u8> {
        let mut buf = Vec::with_capacity(256 + self.body.len());
        buf.extend_from_slice(
            format!("HTTP/1.1 {} {}\r\n", self.status.0, self.status.1).as_bytes(),
        );
        for (k, v) in &self.headers {
            buf.extend_from_slice(format!("{k}: {v}\r\n").as_bytes());
        }
        buf.extend_from_slice(format!("Content-Length: {}\r\n", self.body.len()).as_bytes());
        buf.extend_from_slice(b"Connection: close\r\n");
        buf.extend_from_slice(b"\r\n");
        buf.extend_from_slice(&self.body);
        buf
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn ok_text_body() {
        let r = Response::ok().text("ok");
        let s = String::from_utf8(r.to_bytes()).unwrap();
        assert!(s.starts_with("HTTP/1.1 200 OK\r\n"));
        assert!(s.contains("Content-Type: text/plain"));
        assert!(s.contains("Content-Length: 2\r\n"));
        assert!(s.ends_with("\r\n\r\nok"));
    }

    #[test]
    fn json_body() {
        let r = Response::created().json(&json!({"id": 1, "title": "Hi"}));
        let s = String::from_utf8(r.to_bytes()).unwrap();
        assert!(s.starts_with("HTTP/1.1 201 Created\r\n"));
        assert!(s.contains("Content-Type: application/json"));
        assert!(s.contains(r#"{"id":1,"title":"Hi"}"#));
    }

    #[test]
    fn no_content_status() {
        let r = Response::no_content();
        let s = String::from_utf8(r.to_bytes()).unwrap();
        assert!(s.starts_with("HTTP/1.1 204 No Content\r\n"));
        assert!(s.ends_with("\r\n\r\n"));
    }
}
