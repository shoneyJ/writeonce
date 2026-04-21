use std::collections::BTreeMap;

use crate::ast::Node;
use crate::value::Value;

/// Render a list of AST nodes against a context value.
///
/// `partials` maps partial names to their parsed node lists.
pub fn render(
    nodes: &[Node],
    context: &Value,
    partials: &std::collections::HashMap<String, Vec<Node>>,
) -> String {
    let mut output = String::new();

    for node in nodes {
        match node {
            Node::Literal(text) => {
                output.push_str(text);
            }
            Node::Binding(path) => {
                if path.len() == 1 && path[0] == "this" {
                    output.push_str(&context.to_display());
                } else {
                    let val = context.resolve(path);
                    output.push_str(&val.to_display());
                }
            }
            Node::Each { path, body } => {
                let list_val = if path.len() == 1 && path[0] == "this" {
                    context
                } else {
                    context.resolve(path)
                };

                for item in list_val.as_list() {
                    output.push_str(&render(body, item, partials));
                }
            }
            Node::Partial { name, args } => {
                if let Some(partial_nodes) = partials.get(name.as_str()) {
                    // Build the partial context from args.
                    let partial_ctx = if args.is_empty() {
                        context.clone()
                    } else {
                        let mut map = BTreeMap::new();
                        for (key, path) in args {
                            let val = if path.len() == 1 && path[0] == "this" {
                                context.clone()
                            } else {
                                context.resolve(path).clone()
                            };
                            map.insert(key.clone(), val);
                        }
                        Value::Object(map)
                    };
                    output.push_str(&render(partial_nodes, &partial_ctx, partials));
                }
            }
        }
    }

    output
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::parser;
    use std::collections::HashMap;

    fn ctx(json: serde_json::Value) -> Value {
        Value::from_json(&json)
    }

    #[test]
    fn render_literal() {
        let nodes = parser::parse("<h1>Hello</h1>");
        let result = render(&nodes, &Value::Null, &HashMap::new());
        assert_eq!(result, "<h1>Hello</h1>");
    }

    #[test]
    fn render_binding() {
        let nodes = parser::parse("<h1>{{title}}</h1>");
        let context = ctx(serde_json::json!({"title": "My Article"}));
        let result = render(&nodes, &context, &HashMap::new());
        assert_eq!(result, "<h1>My Article</h1>");
    }

    #[test]
    fn render_nested_binding() {
        let nodes = parser::parse("by {{article.author}}");
        let context = ctx(serde_json::json!({"article": {"author": "Shoney"}}));
        let result = render(&nodes, &context, &HashMap::new());
        assert_eq!(result, "by Shoney");
    }

    #[test]
    fn render_each() {
        let nodes = parser::parse("<ul>{{#each tags}}<li>{{this}}</li>{{/each}}</ul>");
        let context = ctx(serde_json::json!({"tags": ["rust", "linux"]}));
        let result = render(&nodes, &context, &HashMap::new());
        assert_eq!(result, "<ul><li>rust</li><li>linux</li></ul>");
    }

    #[test]
    fn render_each_objects() {
        let nodes = parser::parse("{{#each sections}}<h2>{{heading}}</h2>{{/each}}");
        let context = ctx(serde_json::json!({
            "sections": [
                {"heading": "Intro"},
                {"heading": "Details"}
            ]
        }));
        let result = render(&nodes, &context, &HashMap::new());
        assert_eq!(result, "<h2>Intro</h2><h2>Details</h2>");
    }

    #[test]
    fn render_partial() {
        let nodes = parser::parse("{{> card item=this}}");
        let mut partials = HashMap::new();
        partials.insert(
            "card".to_string(),
            parser::parse("<div>{{item.name}}</div>"),
        );
        let context = ctx(serde_json::json!({"name": "Test"}));
        let result = render(&nodes, &context, &partials);
        assert_eq!(result, "<div>Test</div>");
    }

    #[test]
    fn render_missing_binding() {
        let nodes = parser::parse("{{nonexistent}}");
        let context = ctx(serde_json::json!({}));
        let result = render(&nodes, &context, &HashMap::new());
        assert_eq!(result, "");
    }
}
