/// Map a file extension to its HTTP Content-Type.
pub fn content_type_for(path: &str) -> &'static str {
    let ext = path.rsplit('.').next().unwrap_or("");
    match ext {
        "html" | "htmlx" => "text/html; charset=utf-8",
        "css" => "text/css; charset=utf-8",
        "js" => "application/javascript; charset=utf-8",
        "json" => "application/json; charset=utf-8",
        "png" => "image/png",
        "jpg" | "jpeg" => "image/jpeg",
        "gif" => "image/gif",
        "svg" => "image/svg+xml",
        "ico" => "image/x-icon",
        "woff" => "font/woff",
        "woff2" => "font/woff2",
        "ttf" => "font/ttf",
        "txt" => "text/plain; charset=utf-8",
        "xml" => "application/xml; charset=utf-8",
        _ => "application/octet-stream",
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn known_types() {
        assert_eq!(content_type_for("main.css"), "text/css; charset=utf-8");
        assert_eq!(content_type_for("logo.png"), "image/png");
        assert_eq!(content_type_for("app.js"), "application/javascript; charset=utf-8");
        assert_eq!(content_type_for("favicon.ico"), "image/x-icon");
    }

    #[test]
    fn unknown_type() {
        assert_eq!(content_type_for("file.xyz"), "application/octet-stream");
    }

    #[test]
    fn nested_path() {
        assert_eq!(content_type_for("styles/code-theme.css"), "text/css; charset=utf-8");
    }
}
