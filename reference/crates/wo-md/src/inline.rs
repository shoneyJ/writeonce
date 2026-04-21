/// Convert inline markdown to HTML.
///
/// Handles: **bold**, *italic*, `code`, [text](url), and HTML entity escaping.
/// Does not handle block-level elements (headers, lists, etc.) — those are
/// already structured in the article JSON.
pub fn markdown_to_html(text: &str) -> String {
    let mut result = String::with_capacity(text.len() * 2);
    let chars: Vec<char> = text.chars().collect();
    let len = chars.len();
    let mut i = 0;

    while i < len {
        match chars[i] {
            // HTML entity escaping.
            '&' => { result.push_str("&amp;"); i += 1; }
            '<' => { result.push_str("&lt;"); i += 1; }
            '>' => { result.push_str("&gt;"); i += 1; }

            // **bold** or *italic*
            '*' => {
                if i + 1 < len && chars[i + 1] == '*' {
                    // **bold**
                    if let Some(end) = find_closing(&chars, i + 2, "**") {
                        result.push_str("<strong>");
                        let inner: String = chars[i + 2..end].iter().collect();
                        result.push_str(&escape_html(&inner));
                        result.push_str("</strong>");
                        i = end + 2;
                    } else {
                        result.push('*');
                        i += 1;
                    }
                } else {
                    // *italic*
                    if let Some(end) = find_closing_char(&chars, i + 1, '*') {
                        result.push_str("<em>");
                        let inner: String = chars[i + 1..end].iter().collect();
                        result.push_str(&escape_html(&inner));
                        result.push_str("</em>");
                        i = end + 1;
                    } else {
                        result.push('*');
                        i += 1;
                    }
                }
            }

            // `inline code`
            '`' => {
                if let Some(end) = find_closing_char(&chars, i + 1, '`') {
                    result.push_str("<code>");
                    let inner: String = chars[i + 1..end].iter().collect();
                    result.push_str(&escape_html(&inner));
                    result.push_str("</code>");
                    i = end + 1;
                } else {
                    result.push('`');
                    i += 1;
                }
            }

            // [text](url)
            '[' => {
                if let Some((text_end, url_start, url_end)) = parse_link(&chars, i) {
                    let link_text: String = chars[i + 1..text_end].iter().collect();
                    let url: String = chars[url_start..url_end].iter().collect();
                    result.push_str(&format!(
                        "<a href=\"{}\">{}</a>",
                        escape_html(&url),
                        escape_html(&link_text)
                    ));
                    i = url_end + 1;
                } else {
                    result.push('[');
                    i += 1;
                }
            }

            c => {
                result.push(c);
                i += 1;
            }
        }
    }

    result
}

fn escape_html(s: &str) -> String {
    s.replace('&', "&amp;")
        .replace('<', "&lt;")
        .replace('>', "&gt;")
        .replace('"', "&quot;")
}

fn find_closing(chars: &[char], start: usize, marker: &str) -> Option<usize> {
    let marker_chars: Vec<char> = marker.chars().collect();
    let mlen = marker_chars.len();
    for i in start..chars.len().saturating_sub(mlen - 1) {
        if chars[i..i + mlen] == marker_chars[..] {
            return Some(i);
        }
    }
    None
}

fn find_closing_char(chars: &[char], start: usize, marker: char) -> Option<usize> {
    for i in start..chars.len() {
        if chars[i] == marker {
            return Some(i);
        }
    }
    None
}

fn parse_link(chars: &[char], start: usize) -> Option<(usize, usize, usize)> {
    // [text](url)
    let text_end = find_closing_char(chars, start + 1, ']')?;
    if text_end + 1 >= chars.len() || chars[text_end + 1] != '(' {
        return None;
    }
    let url_start = text_end + 2;
    let url_end = find_closing_char(chars, url_start, ')')?;
    Some((text_end, url_start, url_end))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn plain_text() {
        assert_eq!(markdown_to_html("hello world"), "hello world");
    }

    #[test]
    fn bold() {
        assert_eq!(markdown_to_html("**bold**"), "<strong>bold</strong>");
    }

    #[test]
    fn italic() {
        assert_eq!(markdown_to_html("*italic*"), "<em>italic</em>");
    }

    #[test]
    fn inline_code() {
        assert_eq!(markdown_to_html("`code`"), "<code>code</code>");
    }

    #[test]
    fn link() {
        assert_eq!(
            markdown_to_html("[click](https://example.com)"),
            "<a href=\"https://example.com\">click</a>"
        );
    }

    #[test]
    fn mixed() {
        assert_eq!(
            markdown_to_html("Use **Rust** with `cargo` for [docs](https://doc.rust-lang.org)"),
            "Use <strong>Rust</strong> with <code>cargo</code> for <a href=\"https://doc.rust-lang.org\">docs</a>"
        );
    }

    #[test]
    fn html_escaping() {
        assert_eq!(markdown_to_html("<script>alert('xss')</script>"), "&lt;script&gt;alert('xss')&lt;/script&gt;");
    }
}
