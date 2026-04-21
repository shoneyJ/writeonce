//! AST for `.wo` schema-layer declarations.
//!
//! Stage 2 scope: `type` declarations with fields and `service rest` blocks.
//! Policies, triggers, computed fields, link types, `##ui`, `##app`, `fn` bodies,
//! and `##sql/##doc/##graph` blocks are parsed-and-discarded for now — the AST
//! carries just enough to stand up a REST server that serves the declared types.

#[derive(Debug, Clone, Default)]
pub struct Schema {
    pub types: Vec<TypeDecl>,
}

#[derive(Debug, Clone)]
pub struct TypeDecl {
    pub name:     String,
    pub fields:   Vec<Field>,
    pub services: Vec<ServiceDecl>,
}

#[derive(Debug, Clone)]
pub struct Field {
    pub name:    String,
    pub ty:      FieldTy,
    pub nullable:bool,
    pub unique:  bool,
    pub default: Option<DefaultExpr>,
    /// True if this is a `ref T` / `multi T ...` / `backlink ...` field that doesn't
    /// correspond to a storage column in this type's table. The server ignores it
    /// for create/update/list scalar projection but exposes it via sub-endpoints.
    pub is_relation: bool,
}

#[derive(Debug, Clone)]
pub enum FieldTy {
    /// Plain scalar — one of the well-known names (`Id`, `Text`, `Int`, etc.) or
    /// an unrecognised identifier that's compiled as opaque text for Stage 2.
    Scalar(String),
    /// `[T]` — array of the inner type.
    Array(Box<FieldTy>),
    /// `{ k: T, ... }` — inline embedded-document struct.
    Struct(Vec<Field>),
    /// Tagged union: `A | B | C`. Stage 2 stores variants as strings.
    Union(Vec<String>),
    /// `ref TypeName` — scalar FK to another type.
    Ref(String),
    /// `multi TypeName @edge(:TAG)` — zero-prop graph edge.
    MultiEdge { target: String, tag: Option<String> },
    /// `multi TypeName via LinkType` — graph edge with properties.
    MultiVia  { target: String, link: String },
    /// `backlink TypeName.field` — inverse relation.
    Backlink  { target: String, field: String },
}

#[derive(Debug, Clone)]
pub enum DefaultExpr {
    Str(String),
    Int(i64),
    Bool(bool),
    Null,
    Now,
    Enum(String),        // a bare identifier — e.g. `Customer` in a union default
    Opaque(String),      // anything we didn't bother to evaluate (computed, etc.)
}

#[derive(Debug, Clone)]
pub struct ServiceDecl {
    pub kind:     ServiceKind,
    pub path:     String,
    pub expose:   Vec<Operation>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ServiceKind { Rest, Graphql, Native }

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Operation {
    List,
    Get,
    Create,
    Update,
    Delete,
    Subscribe,
    Me,
    Custom,          // anything we don't special-case — reserved
}

impl Operation {
    pub fn from_ident(s: &str) -> Operation {
        match s {
            "list"      => Operation::List,
            "get"       => Operation::Get,
            "create"    => Operation::Create,
            "update"    => Operation::Update,
            "delete"    => Operation::Delete,
            "subscribe" => Operation::Subscribe,
            "me"        => Operation::Me,
            _           => Operation::Custom,
        }
    }
}

impl Schema {
    pub fn merge(&mut self, other: Schema) {
        self.types.extend(other.types);
    }
}
