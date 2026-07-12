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
    /// Declared with `class` instead of `type`. Storage and REST are
    /// class-blind (plan 13 decision 5); the flag gates method parsing —
    /// `fn` members of a `class` compile into [`MethodDecl`]s (13b), while
    /// `fn` inside a plain `type` keeps the 13a parse-and-discard behaviour.
    pub is_class: bool,
    /// Row-scoped methods (plan 13b). Only populated for classes.
    pub methods:  Vec<MethodDecl>,
}

/// `fn name(args) -> Ret [in txn [snapshot]] { body }` — a row-scoped
/// transactional function with an implicit `self` receiver (plan 13
/// decision 2). Served over RPC as `POST <service-path>/:id/<name>`.
#[derive(Debug, Clone)]
pub struct MethodDecl {
    pub name:   String,
    /// `(name, declared type)` — the type is diagnostic-only in Stage 2.
    pub params: Vec<(String, String)>,
    pub ret:    Option<String>,
    pub txn:    TxnMode,
    pub body:   Vec<Stmt>,
}

/// Transaction annotation on a method. The Stage-2 engine is single-threaded
/// per shard, so every method already executes atomically and in isolation;
/// the mode is recorded for diagnostics and for the future MVCC coordinator.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TxnMode { None, Txn, Snapshot, Serializable }

/// Method-body statement — the schema-layer DML subset of
/// `02-wo-language.md § Schema-Layer DML` that 13b executes.
#[derive(Debug, Clone)]
pub enum Stmt {
    /// `let name = expr` — binding is a snapshot (spec rule).
    Let    { name: String, expr: Expr },
    /// `insert Type { field: expr, ... }` — construction form.
    Insert { ty: String, fields: Vec<(String, Expr)> },
    /// `return [expr]`
    Return { expr: Option<Expr> },
    /// `assert expr [otherwise abort ["msg"]]` — false aborts the txn.
    Assert { cond: Expr, msg: Option<String> },
    /// `if cond { ... } [else { ... }]` (else-if chains nest in `otherwise`).
    If     { cond: Expr, then: Vec<Stmt>, otherwise: Vec<Stmt> },
}

#[derive(Debug, Clone)]
pub enum Expr {
    Int(i64),
    Str(String),
    Bool(bool),
    Null,
    /// Argument, `let` binding, or `self` (bound positionally — `self` stays
    /// a plain identifier in the lexer, plan 13 decision 4).
    Ident(String),
    /// `base.field` — plain object access, or relation resolution when the
    /// base is `self` and the field is a `multi`/`backlink` relation.
    Field(Box<Expr>, String),
    /// `name(args)` — builtins: `latest`, `count`, `now`.
    Call(String, Vec<Expr>),
    Unary(UnOp, Box<Expr>),
    Binary(BinOp, Box<Expr>, Box<Expr>),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum UnOp { Neg, Not }

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BinOp {
    Add, Sub, Mul, Div, Mod,
    Eq, Ne, Lt, Le, Gt, Ge,
    And, Or,
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
