//! Compile parsed [`Schema`] objects into a runtime [`Catalog`] — the engine's
//! view of the declared types, their storage columns, and the REST routes we
//! need to expose.

use crate::ast::*;
use anyhow::{bail, Result};
use std::collections::HashMap;

/// A catalog of all compiled types, ready to feed to the engine and the server.
#[derive(Debug, Clone, Default)]
pub struct Catalog {
    pub types: HashMap<String, CompiledType>,
    /// Preserves declaration order so REST route registration is deterministic.
    pub order: Vec<String>,
}

#[derive(Debug, Clone)]
pub struct CompiledType {
    pub name:     String,
    /// Columns in source order. Includes relation fields; the server treats
    /// those specially but keeps them in the type's row object for echo.
    pub fields:   Vec<Field>,
    pub services: Vec<ServiceDecl>,
    /// True iff the type declared an `id: Id` column. Auto-populated on insert.
    pub has_id:   bool,
}

impl Catalog {
    pub fn from_schemas(schemas: Vec<Schema>) -> Result<Self> {
        let mut cat = Catalog::default();
        for s in schemas {
            for t in s.types {
                if cat.types.contains_key(&t.name) {
                    bail!("duplicate type declaration: {}", t.name);
                }
                let has_id = t.fields.iter().any(|f|
                    f.name == "id" &&
                    matches!(f.ty, FieldTy::Scalar(ref n) if n == "Id")
                );
                cat.order.push(t.name.clone());
                cat.types.insert(t.name.clone(), CompiledType {
                    name:     t.name.clone(),
                    fields:   t.fields,
                    services: t.services,
                    has_id,
                });
            }
        }
        Ok(cat)
    }

    pub fn get(&self, name: &str) -> Option<&CompiledType> {
        self.types.get(name)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::parser::parse;

    #[test]
    fn catalog_collects_types_and_detects_id() {
        let sch = parse(r#"
type Article { id: Id
               title: Text
               service rest "/api/articles" expose list, get }
type Tag { slug: Slug
           service rest "/api/tags" expose list }
"#).unwrap();
        let cat = Catalog::from_schemas(vec![sch]).unwrap();
        assert_eq!(cat.order, vec!["Article", "Tag"]);
        assert!(cat.types["Article"].has_id);
        assert!(!cat.types["Tag"].has_id);
    }
}
