use crate::highlight::highlight;
use crate::inline::markdown_to_html;

/// Parse a full markdown document into HTML.
///
/// Handles block-level elements (headings, paragraphs, code fences, lists,
/// blockquotes, images) and delegates inline formatting to `markdown_to_html`.
pub fn markdown_to_html_block(md: &str) -> String {
    let lines: Vec<&str> = md.lines().collect();
    let len = lines.len();
    let mut html = String::with_capacity(md.len() * 2);
    let mut i = 0;

    while i < len {
        let line = lines[i];
        let trimmed = line.trim();

        // Skip blank lines.
        if trimmed.is_empty() {
            i += 1;
            continue;
        }

        // Fenced code block: ```lang ... ```
        if trimmed.starts_with("```") {
            let lang = trimmed[3..].trim();
            i += 1;
            let mut code = String::new();
            while i < len && !lines[i].trim().starts_with("```") {
                if !code.is_empty() {
                    code.push('\n');
                }
                code.push_str(lines[i]);
                i += 1;
            }
            if i < len {
                i += 1; // skip closing ```
            }

            if lang.is_empty() {
                html.push_str("<pre><code>");
                html.push_str(&escape_html(&code));
                html.push_str("</code></pre>\n");
            } else {
                html.push_str(&format!("<pre><code class=\"language-{}\">", escape_html(lang)));
                html.push_str(&highlight(&code, lang));
                html.push_str("</code></pre>\n");
            }
            continue;
        }

        // Heading: # ... ######
        if trimmed.starts_with('#') {
            let level = trimmed.chars().take_while(|&c| c == '#').count().min(6);
            let text = trimmed[level..].trim();
            html.push_str(&format!(
                "<h{0}>{1}</h{0}>\n",
                level,
                markdown_to_html(text)
            ));
            i += 1;
            continue;
        }

        // Blockquote: > text
        if trimmed.starts_with('>') {
            let mut quote_lines = Vec::new();
            while i < len && lines[i].trim().starts_with('>') {
                let content = lines[i].trim().strip_prefix('>').unwrap_or("").trim();
                quote_lines.push(content);
                i += 1;
            }
            html.push_str("<blockquote>");
            html.push_str(&markdown_to_html(&quote_lines.join(" ")));
            html.push_str("</blockquote>\n");
            continue;
        }

        // Unordered list: - item or * item
        if (trimmed.starts_with("- ") || trimmed.starts_with("* "))
            && !trimmed.starts_with("---")
        {
            html.push_str("<ul>\n");
            while i < len {
                let lt = lines[i].trim();
                if lt.starts_with("- ") || lt.starts_with("* ") {
                    let text = &lt[2..];
                    html.push_str(&format!("<li>{}</li>\n", markdown_to_html(text)));
                    i += 1;
                } else if lt.is_empty() {
                    i += 1;
                    break;
                } else {
                    break;
                }
            }
            html.push_str("</ul>\n");
            continue;
        }

        // Ordered list: 1. item
        if trimmed.len() > 2 && trimmed.as_bytes()[0].is_ascii_digit() {
            if let Some(rest) = strip_ordered_prefix(trimmed) {
                html.push_str("<ol>\n");
                html.push_str(&format!("<li>{}</li>\n", markdown_to_html(rest)));
                i += 1;
                while i < len {
                    let lt = lines[i].trim();
                    if let Some(rest) = strip_ordered_prefix(lt) {
                        html.push_str(&format!("<li>{}</li>\n", markdown_to_html(rest)));
                        i += 1;
                    } else if lt.is_empty() {
                        i += 1;
                        break;
                    } else {
                        break;
                    }
                }
                html.push_str("</ol>\n");
                continue;
            }
        }

        // Image: ![alt](url)
        if trimmed.starts_with("![") {
            if let Some((alt, url)) = parse_image(trimmed) {
                html.push_str(&format!(
                    "<img src=\"{}\" alt=\"{}\">\n",
                    escape_html(url),
                    escape_html(alt)
                ));
                i += 1;
                continue;
            }
        }

        // Horizontal rule: --- or ***
        if trimmed == "---" || trimmed == "***" || trimmed == "___" {
            html.push_str("<hr>\n");
            i += 1;
            continue;
        }

        // Paragraph: collect consecutive non-blank, non-block lines.
        let mut para_lines = Vec::new();
        while i < len {
            let lt = lines[i].trim();
            if lt.is_empty()
                || lt.starts_with('#')
                || lt.starts_with("```")
                || lt.starts_with('>')
                || lt == "---"
                || lt == "***"
                || lt == "___"
            {
                break;
            }
            // Check if next line starts a list
            if (lt.starts_with("- ") || lt.starts_with("* ")) && !lt.starts_with("---") {
                break;
            }
            if strip_ordered_prefix(lt).is_some() && para_lines.is_empty() {
                break;
            }
            para_lines.push(lt);
            i += 1;
        }
        if !para_lines.is_empty() {
            html.push_str("<p>");
            html.push_str(&markdown_to_html(&para_lines.join(" ")));
            html.push_str("</p>\n");
        }
    }

    html
}

fn escape_html(s: &str) -> String {
    s.replace('&', "&amp;")
        .replace('<', "&lt;")
        .replace('>', "&gt;")
        .replace('"', "&quot;")
}

fn strip_ordered_prefix(s: &str) -> Option<&str> {
    let dot_pos = s.find(". ")?;
    if dot_pos > 0 && s[..dot_pos].chars().all(|c| c.is_ascii_digit()) {
        Some(&s[dot_pos + 2..])
    } else {
        None
    }
}

fn parse_image(s: &str) -> Option<(&str, &str)> {
    // ![alt](url)
    let alt_start = s.find("![")? + 2;
    let alt_end = s[alt_start..].find(']')? + alt_start;
    let url_start = s[alt_end..].find('(')? + alt_end + 1;
    let url_end = s[url_start..].find(')')? + url_start;
    Some((&s[alt_start..alt_end], &s[url_start..url_end]))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn headings() {
        let md = "# Title\n\n## Subtitle\n\n### Third";
        let html = markdown_to_html_block(md);
        assert!(html.contains("<h1>Title</h1>"));
        assert!(html.contains("<h2>Subtitle</h2>"));
        assert!(html.contains("<h3>Third</h3>"));
    }

    #[test]
    fn paragraphs() {
        let md = "First paragraph.\n\nSecond paragraph.";
        let html = markdown_to_html_block(md);
        assert!(html.contains("<p>First paragraph.</p>"));
        assert!(html.contains("<p>Second paragraph.</p>"));
    }

    #[test]
    fn code_fence_with_language() {
        let md = "```rust\nfn main() {\n    let x = 42;\n}\n```";
        let html = markdown_to_html_block(md);
        assert!(html.contains("<pre><code class=\"language-rust\">"));
        assert!(html.contains("<span class=\"kw\">fn</span>"));
        assert!(html.contains("<span class=\"num\">42</span>"));
        assert!(html.contains("</code></pre>"));
    }

    #[test]
    fn code_fence_no_language() {
        let md = "```\nplain code\n```";
        let html = markdown_to_html_block(md);
        assert!(html.contains("<pre><code>plain code</code></pre>"));
    }

    #[test]
    fn unordered_list() {
        let md = "- Item one\n- Item two\n- Item three";
        let html = markdown_to_html_block(md);
        assert!(html.contains("<ul>"));
        assert!(html.contains("<li>Item one</li>"));
        assert!(html.contains("<li>Item two</li>"));
        assert!(html.contains("</ul>"));
    }

    #[test]
    fn ordered_list() {
        let md = "1. First\n2. Second\n3. Third";
        let html = markdown_to_html_block(md);
        assert!(html.contains("<ol>"));
        assert!(html.contains("<li>First</li>"));
        assert!(html.contains("<li>Third</li>"));
        assert!(html.contains("</ol>"));
    }

    #[test]
    fn blockquote() {
        let md = "> This is a quote\n> spanning two lines";
        let html = markdown_to_html_block(md);
        assert!(html.contains("<blockquote>This is a quote spanning two lines</blockquote>"));
    }

    #[test]
    fn image() {
        let md = "![Alt text](https://example.com/img.png)";
        let html = markdown_to_html_block(md);
        assert!(html.contains("<img src=\"https://example.com/img.png\" alt=\"Alt text\">"));
    }

    #[test]
    fn horizontal_rule() {
        let md = "Before\n\n---\n\nAfter";
        let html = markdown_to_html_block(md);
        assert!(html.contains("<hr>"));
    }

    #[test]
    fn inline_formatting_in_paragraphs() {
        let md = "Use **bold** and `code` in a paragraph.";
        let html = markdown_to_html_block(md);
        assert!(html.contains("<strong>bold</strong>"));
        assert!(html.contains("<code>code</code>"));
    }

    #[test]
    fn full_article() {
        let md = "\
# Getting Started

Welcome to the guide.

## Installation

Install with cargo:

```bash
cargo install writeonce
```

## Features

- Fast rendering
- Zero dependencies
- **Bold** feature

> Note: this is a quote.

![Logo](logo.png)
";
        let html = markdown_to_html_block(md);
        assert!(html.contains("<h1>Getting Started</h1>"));
        assert!(html.contains("<h2>Installation</h2>"));
        assert!(html.contains("<p>Install with cargo:</p>"));
        assert!(html.contains("<pre><code class=\"language-bash\">"));
        assert!(html.contains("<li>Fast rendering</li>"));
        assert!(html.contains("<blockquote>"));
        assert!(html.contains("<img src=\"logo.png\""));
    }
}
