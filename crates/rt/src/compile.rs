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
    /// Row-scoped methods (plan 13b) — populated for `class` declarations,
    /// empty for plain `type`s. Storage stays class-blind; methods only add
    /// RPC routes on top.
    pub methods:  Vec<MethodDecl>,
    /// True iff the type declared an `id: Id` column. Auto-populated on insert.
    pub has_id:   bool,
    /// Storage/table name — `@table(name: "...")` override or the type name.
    /// Metadata for the SQL layer and plans 10–12; the engine and WAL key
    /// everything by type name (the stable identifier).
    pub storage_name: String,
    /// Composite secondary indexes from `@table(index: [...])`, validated
    /// against the fields. The engine maintains these on every mutation.
    pub indexes:  Vec<Vec<String>>,
}

impl Catalog {
    pub fn from_schemas(schemas: Vec<Schema>) -> Result<Self> {
        let mut cat = Catalog::default();
        let mut storage_names = std::collections::HashSet::new();
        for s in schemas {
            for t in s.types {
                if cat.types.contains_key(&t.name) {
                    bail!("duplicate type declaration: {}", t.name);
                }
                let has_id = t.fields.iter().any(|f|
                    f.name == "id" &&
                    matches!(f.ty, FieldTy::Scalar(ref n) if n == "Id")
                );

                // @table validation (plan 13 follow-up): the name must be
                // catalog-unique; index columns must be stored scalar
                // columns (scalars, unions, and `ref` FKs — not relations
                // without a column, not arrays/structs).
                let storage_name = t.table.name.clone().unwrap_or_else(|| t.name.clone());
                if !storage_names.insert(storage_name.clone()) {
                    bail!("{}: @table name \"{storage_name}\" is already used by another type",
                          t.name);
                }
                for cols in &t.table.indexes {
                    for col in cols {
                        let Some(f) = t.fields.iter().find(|f| &f.name == col) else {
                            bail!("{}: @table index names unknown field `{col}`", t.name);
                        };
                        match &f.ty {
                            FieldTy::Scalar(_) | FieldTy::Union(_) | FieldTy::Ref(_) => {}
                            FieldTy::MultiEdge { .. } | FieldTy::MultiVia { .. }
                            | FieldTy::Backlink { .. } => bail!(
                                "{}: @table index field `{col}` is a relation without a \
                                 stored column — index the `ref` side instead", t.name),
                            FieldTy::Array(_) | FieldTy::Struct(_) => bail!(
                                "{}: @table index field `{col}` is not a scalar column", t.name),
                        }
                    }
                }

                cat.order.push(t.name.clone());
                cat.types.insert(t.name.clone(), CompiledType {
                    name:     t.name.clone(),
                    fields:   t.fields,
                    services: t.services,
                    methods:  t.methods,
                    has_id,
                    storage_name,
                    indexes:  t.table.indexes,
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
    fn table_annotation_validation() {
        // Duplicate storage names collide across types.
        let sch = parse("@table(name: \"t\")\ntype A { id: Id }\n@table(name: \"t\")\ntype B { id: Id }").unwrap();
        let err = Catalog::from_schemas(vec![sch]).unwrap_err().to_string();
        assert!(err.contains("already used"), "{err}");

        // A name override colliding with another type's default name.
        let sch = parse("@table(name: \"B\")\ntype A { id: Id }\ntype B { id: Id }").unwrap();
        assert!(Catalog::from_schemas(vec![sch]).is_err());

        // Index on a missing field.
        let sch = parse("@table(index: [nope])\ntype C { id: Id }").unwrap();
        let err = Catalog::from_schemas(vec![sch]).unwrap_err().to_string();
        assert!(err.contains("unknown field `nope`"), "{err}");

        // Index on a relation without a stored column.
        let sch = parse("@table(index: [prices])\nclass P { id: Id\n  prices: multi Price }").unwrap();
        let err = Catalog::from_schemas(vec![sch]).unwrap_err().to_string();
        assert!(err.contains("relation without a stored column"), "{err}");

        // Valid: ref FK + scalar composite; storage_name defaults to type name.
        let sch = parse(
            "@table(name: \"prices\", index: [product, at])\nclass Price { id: Id\n  product: ref Product\n  at: Text }"
        ).unwrap();
        let cat = Catalog::from_schemas(vec![sch]).unwrap();
        let t = cat.get("Price").unwrap();
        assert_eq!(t.storage_name, "prices");
        assert_eq!(t.indexes, vec![vec!["product".to_string(), "at".to_string()]]);
    }

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
