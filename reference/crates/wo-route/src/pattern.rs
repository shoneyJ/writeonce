/// A compiled URL pattern segment.
#[derive(Debug, Clone, PartialEq)]
pub enum Segment {
    /// Exact literal match (e.g., "blog").
    Literal(String),
    /// Named parameter (e.g., ":sys_title").
    Param(String),
    /// Wildcard matching the rest of the path (e.g., "*path").
    Wildcard(String),
}

/// A compiled URL pattern like "/blog/:sys_title" or "/static/*path".
#[derive(Debug, Clone)]
pub struct Pattern {
    pub segments: Vec<Segment>,
}

impl Pattern {
    /// Compile a pattern string into segments.
    ///
    /// - `/blog/:sys_title` → `[Literal("blog"), Param("sys_title")]`
    /// - `/static/*path` → `[Literal("static"), Wildcard("path")]`
    /// - `/` → `[]`
    pub fn compile(pattern: &str) -> Self {
        let segments = pattern
            .trim_start_matches('/')
            .split('/')
            .filter(|s| !s.is_empty())
            .map(|s| {
                if let Some(name) = s.strip_prefix(':') {
                    Segment::Param(name.to_string())
                } else if let Some(name) = s.strip_prefix('*') {
                    Segment::Wildcard(name.to_string())
                } else {
                    Segment::Literal(s.to_string())
                }
            })
            .collect();

        Self { segments }
    }

    /// Try to match a URL path against this pattern.
    ///
    /// Returns `Some(params)` if the path matches, where params is a list
    /// of `(name, value)` pairs for any `:param` or `*wildcard` segments.
    pub fn matches(&self, path: &str) -> Option<Vec<(String, String)>> {
        let path_segments: Vec<&str> = path
            .trim_start_matches('/')
            .split('/')
            .filter(|s| !s.is_empty())
            .collect();

        let mut params = Vec::new();
        let mut pi = 0; // path segment index

        for seg in &self.segments {
            match seg {
                Segment::Literal(lit) => {
                    if pi >= path_segments.len() || path_segments[pi] != lit.as_str() {
                        return None;
                    }
                    pi += 1;
                }
                Segment::Param(name) => {
                    if pi >= path_segments.len() {
                        return None;
                    }
                    params.push((name.clone(), path_segments[pi].to_string()));
                    pi += 1;
                }
                Segment::Wildcard(name) => {
                    if pi >= path_segments.len() {
                        return None;
                    }
                    let rest = path_segments[pi..].join("/");
                    params.push((name.clone(), rest));
                    return Some(params);
                }
            }
        }

        // All pattern segments consumed; path must also be fully consumed.
        if pi == path_segments.len() {
            Some(params)
        } else {
            None
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn root_path() {
        let p = Pattern::compile("/");
        assert!(p.matches("/").is_some());
        assert!(p.matches("/blog").is_none());
    }

    #[test]
    fn literal_path() {
        let p = Pattern::compile("/about");
        assert!(p.matches("/about").is_some());
        assert!(p.matches("/contact").is_none());
        assert!(p.matches("/about/extra").is_none());
    }

    #[test]
    fn param_extraction() {
        let p = Pattern::compile("/blog/:sys_title");
        let params = p.matches("/blog/linux-misc").unwrap();
        assert_eq!(params, vec![("sys_title".into(), "linux-misc".into())]);

        assert!(p.matches("/blog").is_none());
        assert!(p.matches("/blog/linux-misc/extra").is_none());
    }

    #[test]
    fn wildcard() {
        let p = Pattern::compile("/static/*path");
        let params = p.matches("/static/styles/main.css").unwrap();
        assert_eq!(params, vec![("path".into(), "styles/main.css".into())]);

        let params = p.matches("/static/logo.png").unwrap();
        assert_eq!(params, vec![("path".into(), "logo.png".into())]);

        assert!(p.matches("/static").is_none());
    }

    #[test]
    fn multi_segment() {
        let p = Pattern::compile("/tag/:tag");
        let params = p.matches("/tag/rust").unwrap();
        assert_eq!(params, vec![("tag".into(), "rust".into())]);
    }

    #[test]
    fn no_match() {
        let p = Pattern::compile("/blog/:sys_title");
        assert!(p.matches("/about").is_none());
        assert!(p.matches("/").is_none());
    }
}
