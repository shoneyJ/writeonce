/// HTTP response builder.
#[derive(Debug, Clone)]
pub struct Response {
    pub status: u16,
    pub status_text: String,
    pub headers: Vec<(String, String)>,
    pub body: Vec<u8>,
}

impl Response {
    pub fn new(status: u16, status_text: &str) -> Self {
        Self {
            status,
            status_text: status_text.to_string(),
            headers: vec![],
            body: vec![],
        }
    }

    /// 200 OK with HTML body.
    pub fn html(body: String) -> Self {
        let mut r = Self::new(200, "OK");
        r.header("Content-Type", "text/html; charset=utf-8");
        r.header("Content-Length", &body.len().to_string());
        r.body = body.into_bytes();
        r
    }

    /// 200 OK with a raw body and content type.
    pub fn ok(body: Vec<u8>, content_type: &str) -> Self {
        let mut r = Self::new(200, "OK");
        r.header("Content-Type", content_type);
        r.header("Content-Length", &body.len().to_string());
        r.body = body;
        r
    }

    /// 404 Not Found.
    pub fn not_found() -> Self {
        let body = "<h1>404 Not Found</h1>";
        let mut r = Self::new(404, "Not Found");
        r.header("Content-Type", "text/html; charset=utf-8");
        r.header("Content-Length", &body.len().to_string());
        r.body = body.as_bytes().to_vec();
        r
    }

    /// 500 Internal Server Error.
    pub fn internal_error(msg: &str) -> Self {
        let body = format!("<h1>500 Internal Server Error</h1><p>{}</p>", msg);
        let mut r = Self::new(500, "Internal Server Error");
        r.header("Content-Type", "text/html; charset=utf-8");
        r.header("Content-Length", &body.len().to_string());
        r.body = body.into_bytes();
        r
    }

    /// Add a header.
    pub fn header(&mut self, key: &str, value: &str) -> &mut Self {
        self.headers.push((key.to_string(), value.to_string()));
        self
    }

    /// Serialize the response to bytes for writing to a socket.
    pub fn to_bytes(&self) -> Vec<u8> {
        let mut buf = Vec::with_capacity(256 + self.body.len());

        // Status line.
        buf.extend_from_slice(
            format!("HTTP/1.1 {} {}\r\n", self.status, self.status_text).as_bytes(),
        );

        // Headers.
        for (key, value) in &self.headers {
            buf.extend_from_slice(format!("{}: {}\r\n", key, value).as_bytes());
        }

        // End of headers.
        buf.extend_from_slice(b"\r\n");

        // Body.
        buf.extend_from_slice(&self.body);

        buf
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn html_response() {
        let r = Response::html("<h1>Hello</h1>".into());
        let bytes = r.to_bytes();
        let s = String::from_utf8(bytes).unwrap();
        assert!(s.starts_with("HTTP/1.1 200 OK\r\n"));
        assert!(s.contains("Content-Type: text/html"));
        assert!(s.contains("<h1>Hello</h1>"));
    }

    #[test]
    fn not_found_response() {
        let r = Response::not_found();
        assert_eq!(r.status, 404);
        let bytes = r.to_bytes();
        let s = String::from_utf8(bytes).unwrap();
        assert!(s.contains("404 Not Found"));
    }
}
