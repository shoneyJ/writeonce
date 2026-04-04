use std::collections::BTreeMap;

/// Template context value — the data that template bindings resolve against.
#[derive(Debug, Clone, PartialEq)]
pub enum Value {
    String(String),
    Number(f64),
    Bool(bool),
    List(Vec<Value>),
    Object(BTreeMap<String, Value>),
    Null,
}

impl Value {
    /// Resolve a dotted path like ["article", "title"] against this value.
    pub fn resolve(&self, path: &[String]) -> &Value {
        let mut current = self;
        for key in path {
            match current {
                Value::Object(map) => {
                    current = map.get(key.as_str()).unwrap_or(&Value::Null);
                }
                _ => return &Value::Null,
            }
        }
        current
    }

    /// Convert to a display string for template output.
    pub fn to_display(&self) -> String {
        match self {
            Value::String(s) => s.clone(),
            Value::Number(n) => {
                if *n == (*n as i64) as f64 {
                    format!("{}", *n as i64)
                } else {
                    format!("{}", n)
                }
            }
            Value::Bool(b) => b.to_string(),
            Value::Null => String::new(),
            Value::List(items) => {
                let parts: Vec<String> = items.iter().map(|v| v.to_display()).collect();
                parts.join(", ")
            }
            Value::Object(_) => "[object]".to_string(),
        }
    }

    /// Convert to a list for iteration (returns empty vec if not a list).
    pub fn as_list(&self) -> &[Value] {
        match self {
            Value::List(items) => items,
            _ => &[],
        }
    }

    /// Convert a serde_json::Value to a template Value.
    pub fn from_json(json: &serde_json::Value) -> Self {
        match json {
            serde_json::Value::Null => Value::Null,
            serde_json::Value::Bool(b) => Value::Bool(*b),
            serde_json::Value::Number(n) => Value::Number(n.as_f64().unwrap_or(0.0)),
            serde_json::Value::String(s) => Value::String(s.clone()),
            serde_json::Value::Array(arr) => {
                Value::List(arr.iter().map(Value::from_json).collect())
            }
            serde_json::Value::Object(map) => {
                let btree = map
                    .iter()
                    .map(|(k, v)| (k.clone(), Value::from_json(v)))
                    .collect();
                Value::Object(btree)
            }
        }
    }
}

/// Convert an Article to a template Value using serde_json as intermediary.
pub fn article_to_value(article: &wo_model::Article) -> Value {
    let json = serde_json::to_value(article).unwrap_or(serde_json::Value::Null);
    Value::from_json(&json)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn resolve_path() {
        let mut inner = BTreeMap::new();
        inner.insert("title".into(), Value::String("Hello".into()));
        let mut root = BTreeMap::new();
        root.insert("article".into(), Value::Object(inner));
        let val = Value::Object(root);

        let result = val.resolve(&["article".into(), "title".into()]);
        assert_eq!(result, &Value::String("Hello".into()));
    }

    #[test]
    fn resolve_missing() {
        let val = Value::Object(BTreeMap::new());
        assert_eq!(val.resolve(&["nope".into()]), &Value::Null);
    }

    #[test]
    fn from_json() {
        let json: serde_json::Value = serde_json::json!({
            "name": "test",
            "count": 42,
            "active": true,
            "tags": ["a", "b"]
        });
        let val = Value::from_json(&json);
        assert_eq!(
            val.resolve(&["name".into()]),
            &Value::String("test".into())
        );
        assert_eq!(val.resolve(&["count".into()]), &Value::Number(42.0));
    }
}
