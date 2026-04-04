use crate::ast::Node;

/// Parse a template string into a list of AST nodes.
pub fn parse(template: &str) -> Vec<Node> {
    let mut nodes = Vec::new();
    let mut rest = template;

    while !rest.is_empty() {
        if let Some(pos) = rest.find("{{") {
            // Literal before the opening `{{`.
            if pos > 0 {
                nodes.push(Node::Literal(rest[..pos].to_string()));
            }

            let after_open = &rest[pos + 2..];

            if let Some(close) = after_open.find("}}") {
                let expr = after_open[..close].trim();
                rest = &after_open[close + 2..];

                if let Some(each_path) = expr.strip_prefix("#each ") {
                    // Block: {{#each path}} ... {{/each}}
                    let path = parse_path(each_path.trim());
                    let (body, remaining) = parse_until_end_each(rest);
                    nodes.push(Node::Each { path, body });
                    rest = remaining;
                } else if let Some(partial_expr) = expr.strip_prefix("> ") {
                    // Partial: {{> name arg=value}}
                    let (name, args) = parse_partial_expr(partial_expr.trim());
                    nodes.push(Node::Partial { name, args });
                } else if !expr.starts_with('/') {
                    // Binding: {{path.to.value}}
                    let path = parse_path(expr);
                    nodes.push(Node::Binding(path));
                }
                // {{/each}} handled by parse_until_end_each
            } else {
                // No closing `}}` — treat rest as literal.
                nodes.push(Node::Literal(rest.to_string()));
                break;
            }
        } else {
            // No more `{{` — rest is literal.
            nodes.push(Node::Literal(rest.to_string()));
            break;
        }
    }

    nodes
}

fn parse_path(s: &str) -> Vec<String> {
    s.split('.').map(|p| p.trim().to_string()).collect()
}

fn parse_until_end_each(input: &str) -> (Vec<Node>, &str) {
    // Find the matching {{/each}}.
    let mut depth = 1;
    let mut pos = 0;
    let bytes = input.as_bytes();

    while pos < bytes.len() {
        if let Some(open) = input[pos..].find("{{") {
            let abs = pos + open;
            let after = &input[abs + 2..];
            if let Some(close) = after.find("}}") {
                let expr = after[..close].trim();
                if expr.starts_with("#each ") {
                    depth += 1;
                } else if expr == "/each" {
                    depth -= 1;
                    if depth == 0 {
                        let body_str = &input[..abs];
                        let rest = &after[close + 2..];
                        return (parse(body_str), rest);
                    }
                }
                pos = abs + 2 + close + 2;
            } else {
                break;
            }
        } else {
            break;
        }
    }

    // Unmatched {{#each}} — return rest as literal body.
    (vec![Node::Literal(input.to_string())], "")
}

fn parse_partial_expr(expr: &str) -> (String, Vec<(String, Vec<String>)>) {
    let parts: Vec<&str> = expr.splitn(2, ' ').collect();
    let name = parts[0].to_string();
    let mut args = Vec::new();

    if parts.len() > 1 {
        for arg in parts[1].split_whitespace() {
            if let Some((key, val)) = arg.split_once('=') {
                args.push((key.to_string(), parse_path(val)));
            }
        }
    }

    (name, args)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::ast::Node;

    #[test]
    fn parse_literal() {
        let nodes = parse("<h1>Hello</h1>");
        assert_eq!(nodes, vec![Node::Literal("<h1>Hello</h1>".into())]);
    }

    #[test]
    fn parse_binding() {
        let nodes = parse("{{article.title}}");
        assert_eq!(nodes, vec![Node::Binding(vec!["article".into(), "title".into()])]);
    }

    #[test]
    fn parse_mixed() {
        let nodes = parse("<h1>{{title}}</h1>");
        assert_eq!(nodes.len(), 3);
        assert_eq!(nodes[0], Node::Literal("<h1>".into()));
        assert_eq!(nodes[1], Node::Binding(vec!["title".into()]));
        assert_eq!(nodes[2], Node::Literal("</h1>".into()));
    }

    #[test]
    fn parse_each() {
        let nodes = parse("{{#each items}}<li>{{this}}</li>{{/each}}");
        assert_eq!(nodes.len(), 1);
        match &nodes[0] {
            Node::Each { path, body } => {
                assert_eq!(path, &vec!["items".to_string()]);
                assert_eq!(body.len(), 3);
            }
            _ => panic!("expected Each"),
        }
    }

    #[test]
    fn parse_partial() {
        let nodes = parse("{{> article-card article=this}}");
        assert_eq!(nodes.len(), 1);
        match &nodes[0] {
            Node::Partial { name, args } => {
                assert_eq!(name, "article-card");
                assert_eq!(args.len(), 1);
                assert_eq!(args[0].0, "article");
                assert_eq!(args[0].1, vec!["this".to_string()]);
            }
            _ => panic!("expected Partial"),
        }
    }

    #[test]
    fn parse_nested_each() {
        let nodes = parse("{{#each sections}}{{#each paragraphs}}{{this}}{{/each}}{{/each}}");
        assert_eq!(nodes.len(), 1);
        match &nodes[0] {
            Node::Each { body, .. } => {
                assert_eq!(body.len(), 1);
                assert!(matches!(&body[0], Node::Each { .. }));
            }
            _ => panic!("expected nested Each"),
        }
    }
}
