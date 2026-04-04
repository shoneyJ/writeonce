/// Server-side syntax highlighting for code blocks.
///
/// Returns HTML with `<span class="...">` tokens. Pair with code-theme.css.
pub fn highlight(code: &str, language: &str) -> String {
    let keywords = keywords_for(language);
    let mut result = String::with_capacity(code.len() * 2);
    let chars: Vec<char> = code.chars().collect();
    let len = chars.len();
    let mut i = 0;

    while i < len {
        // String literals (double or single quotes).
        if chars[i] == '"' || chars[i] == '\'' {
            let quote = chars[i];
            let start = i;
            i += 1;
            while i < len && chars[i] != quote {
                if chars[i] == '\\' { i += 1; } // skip escaped char
                i += 1;
            }
            if i < len { i += 1; } // closing quote
            let s: String = chars[start..i].iter().collect();
            result.push_str(&format!("<span class=\"str\">{}</span>", escape(&s)));
            continue;
        }

        // Line comments.
        if i + 1 < len && chars[i] == '/' && chars[i + 1] == '/' {
            let start = i;
            while i < len && chars[i] != '\n' { i += 1; }
            let s: String = chars[start..i].iter().collect();
            result.push_str(&format!("<span class=\"cm\">{}</span>", escape(&s)));
            continue;
        }

        // Hash comments (bash, yaml, etc.).
        if chars[i] == '#' && (language == "bash" || language == "yaml" || language == "python") {
            let start = i;
            while i < len && chars[i] != '\n' { i += 1; }
            let s: String = chars[start..i].iter().collect();
            result.push_str(&format!("<span class=\"cm\">{}</span>", escape(&s)));
            continue;
        }

        // Numbers.
        if chars[i].is_ascii_digit() {
            let start = i;
            while i < len && (chars[i].is_ascii_alphanumeric() || chars[i] == '.') { i += 1; }
            let s: String = chars[start..i].iter().collect();
            result.push_str(&format!("<span class=\"num\">{}</span>", escape(&s)));
            continue;
        }

        // Identifiers / keywords.
        if chars[i].is_ascii_alphabetic() || chars[i] == '_' {
            let start = i;
            while i < len && (chars[i].is_ascii_alphanumeric() || chars[i] == '_') { i += 1; }
            let word: String = chars[start..i].iter().collect();
            if keywords.contains(&word.as_str()) {
                result.push_str(&format!("<span class=\"kw\">{}</span>", escape(&word)));
            } else {
                result.push_str(&escape(&word));
            }
            continue;
        }

        // Everything else.
        result.push_str(&escape(&chars[i].to_string()));
        i += 1;
    }

    result
}

fn escape(s: &str) -> String {
    s.replace('&', "&amp;")
        .replace('<', "&lt;")
        .replace('>', "&gt;")
}

fn keywords_for(language: &str) -> &'static [&'static str] {
    match language {
        "rust" => &[
            "fn", "let", "mut", "const", "struct", "enum", "impl", "trait", "pub", "use",
            "mod", "crate", "self", "super", "return", "if", "else", "match", "for", "while",
            "loop", "break", "continue", "where", "type", "as", "in", "ref", "move",
            "async", "await", "unsafe", "extern", "dyn", "true", "false",
        ],
        "go" => &[
            "func", "var", "const", "type", "struct", "interface", "map", "chan",
            "package", "import", "return", "if", "else", "for", "range", "switch",
            "case", "default", "go", "defer", "select", "true", "false", "nil",
        ],
        "bash" | "sh" => &[
            "if", "then", "else", "elif", "fi", "for", "while", "do", "done",
            "case", "esac", "function", "return", "export", "local", "echo",
            "in", "true", "false",
        ],
        "yaml" => &["true", "false", "null", "yes", "no"],
        "json" => &["true", "false", "null"],
        "python" => &[
            "def", "class", "if", "elif", "else", "for", "while", "return",
            "import", "from", "as", "with", "try", "except", "finally",
            "raise", "pass", "lambda", "yield", "True", "False", "None", "in",
            "not", "and", "or", "is", "async", "await",
        ],
        _ => &[],
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn highlight_rust() {
        let code = "fn main() {\n    let x = 42;\n}";
        let html = highlight(code, "rust");
        assert!(html.contains("<span class=\"kw\">fn</span>"));
        assert!(html.contains("<span class=\"kw\">let</span>"));
        assert!(html.contains("<span class=\"num\">42</span>"));
    }

    #[test]
    fn highlight_string() {
        let code = r#"let s = "hello";"#;
        let html = highlight(code, "rust");
        assert!(html.contains("<span class=\"str\">\"hello\"</span>"));
    }

    #[test]
    fn highlight_comment() {
        let code = "// this is a comment\nlet x = 1;";
        let html = highlight(code, "rust");
        assert!(html.contains("<span class=\"cm\">// this is a comment</span>"));
    }

    #[test]
    fn highlight_bash() {
        let code = "# comment\nexport PATH=/usr/bin";
        let html = highlight(code, "bash");
        assert!(html.contains("<span class=\"cm\"># comment</span>"));
        assert!(html.contains("<span class=\"kw\">export</span>"));
    }

    #[test]
    fn escapes_html() {
        let code = "fn compare<T>(a: T, b: T) {}";
        let html = highlight(code, "rust");
        assert!(html.contains("&lt;"));
        assert!(html.contains("&gt;"));
        assert!(!html.contains("<T>"));
    }
}
