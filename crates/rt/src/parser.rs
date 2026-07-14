//! Recursive-descent parser for `.wo` source.
//!
//! Stage 2 scope:
//!   * `type Name { ... }` declarations with fields and `service rest` blocks
//!   * graceful skip of constructs we don't yet execute: `policy`, `on <event>`,
//!     computed field defaults, `fn`, `main`, `##ui`/`##app`/`##sql`/`##doc`/
//!     `##graph`/`##policy`/`##service`/`##logic`/`##logic` blocks.
//!
//! "Skip" means: consume until the matching close brace / next top-level start,
//! so the parser survives and later phases can do nothing.

use crate::ast::*;
use crate::lexer::tokenize;
use crate::token::{Kind, Token};

use anyhow::{bail, Context, Result};

pub fn parse(src: &str) -> Result<Schema> {
    let toks = tokenize(src).context("tokenize")?;
    Parser::new(toks).parse_schema()
}

struct Parser {
    toks: Vec<Token>,
    pos:  usize,
}

impl Parser {
    fn new(toks: Vec<Token>) -> Self { Self { toks, pos: 0 } }

    // --- primitives ---

    fn peek(&self) -> &Kind { &self.toks[self.pos.min(self.toks.len() - 1)].kind }
    fn peek_line(&self) -> u32 { self.toks[self.pos.min(self.toks.len() - 1)].line }

    fn advance(&mut self) -> &Token {
        let t = &self.toks[self.pos];
        if !matches!(t.kind, Kind::End) { self.pos += 1; }
        &self.toks[self.pos.saturating_sub(1)]
    }

    fn skip_newlines(&mut self) {
        while matches!(self.peek(), Kind::Newline) { self.advance(); }
    }

    fn accept(&mut self, want: &Kind) -> bool {
        if std::mem::discriminant(self.peek()) == std::mem::discriminant(want) {
            self.advance();
            true
        } else { false }
    }

    fn expect(&mut self, want: &Kind, what: &str) -> Result<&Token> {
        if std::mem::discriminant(self.peek()) == std::mem::discriminant(want) {
            Ok(self.advance())
        } else {
            bail!("line {}: expected {what}, got {}", self.peek_line(), self.peek())
        }
    }

    fn expect_ident(&mut self, what: &str) -> Result<String> {
        match self.peek().clone() {
            Kind::Ident(s) => { self.advance(); Ok(s) }
            k => bail!("line {}: expected {what}, got {k}", self.peek_line()),
        }
    }

    fn at_end(&self) -> bool { matches!(self.peek(), Kind::End) }

    // --- top-level ---

    fn parse_schema(&mut self) -> Result<Schema> {
        let mut sch = Schema::default();
        loop {
            self.skip_newlines();
            if self.at_end() { break; }
            match self.peek() {
                Kind::KwType | Kind::KwClass =>
                    sch.types.push(self.parse_type(TableCfg::default())?),
                // Type-level annotation: `@table(...)` configures the
                // declaration that follows; unknown names skip silently
                // (the field-annotation precedent).
                Kind::At => {
                    if let Some(table) = self.parse_type_annotations()? {
                        self.skip_newlines();
                        if !matches!(self.peek(), Kind::KwType | Kind::KwClass) {
                            bail!("line {}: expected `type` or `class` after @table, got {}",
                                  self.peek_line(), self.peek());
                        }
                        sch.types.push(self.parse_type(table)?);
                    }
                }
                // Skip constructs we don't execute yet.
                Kind::HashHash(_)
                | Kind::KwFn
                | Kind::KwMain
                | Kind::KwOn
                | Kind::KwPolicy
                | Kind::KwTest
                | Kind::KwLet        // stray `let` at top level (in `main { ... }` probably)
                | Kind::Hash(_)      // ##sql #table etc.
                => self.skip_top_level_chunk()?,
                _ => self.skip_top_level_chunk()?,
            }
        }
        Ok(sch)
    }

    /// Parse the type-level annotation list ahead of a `type`/`class`.
    /// Returns `Some(cfg)` when a recognised `@table` was consumed, `None`
    /// when the annotation was unknown and skipped (caller resumes the loop).
    fn parse_type_annotations(&mut self) -> Result<Option<TableCfg>> {
        self.expect(&Kind::At, "'@'")?;
        let name = self.expect_ident("annotation name")?;
        if name != "table" {
            // Unknown type-level annotation: consume an optional (...) block
            // and let the schema loop decide what the next token means.
            if matches!(self.peek(), Kind::LParen) {
                let mut depth = 1i32;
                self.advance();
                while depth > 0 && !self.at_end() {
                    match self.peek() {
                        Kind::LParen => { depth += 1; self.advance(); }
                        Kind::RParen => { depth -= 1; self.advance(); }
                        _ => { self.advance(); }
                    }
                }
            }
            return Ok(None);
        }

        let mut cfg = TableCfg::default();
        if !self.accept(&Kind::LParen) {
            return Ok(Some(cfg));            // bare `@table` — legal no-op
        }
        loop {
            self.skip_newlines();
            if self.accept(&Kind::RParen) { break; }
            let key = self.expect_ident("@table argument")?;
            self.expect(&Kind::Colon, "':'")?;
            match key.as_str() {
                "name" => {
                    if cfg.name.is_some() {
                        bail!("line {}: @table(name: ...) given twice", self.peek_line());
                    }
                    match self.peek().clone() {
                        Kind::Str(s) => { self.advance(); cfg.name = Some(s); }
                        other => bail!("line {}: @table name must be a string, got {other}",
                                       self.peek_line()),
                    }
                }
                "index" => {
                    self.expect(&Kind::LBracket, "'['")?;
                    let mut cols = Vec::new();
                    loop {
                        cols.push(self.expect_ident("index column")?);
                        if !self.accept(&Kind::Comma) { break; }
                    }
                    self.expect(&Kind::RBracket, "']'")?;
                    if cols.is_empty() {
                        bail!("line {}: @table index needs at least one column", self.peek_line());
                    }
                    cfg.indexes.push(cols);
                }
                // `shard_key`/`retention` are reserved for later phases —
                // reject loudly rather than silently ignoring (no silent
                // passthrough on surface we own).
                other => bail!(
                    "line {}: unknown @table argument `{other}` \
                     (supported: name, index)", self.peek_line()),
            }
            self.skip_newlines();
            if !self.accept(&Kind::Comma) {
                self.skip_newlines();
                self.expect(&Kind::RParen, "')' or ','")?;
                break;
            }
        }
        Ok(Some(cfg))
    }

    /// Walk forward until we reach the start of the next top-level construct
    /// (another `type`, `##…`, or EOF), balancing braces in between.
    fn skip_top_level_chunk(&mut self) -> Result<()> {
        // Always consume at least one token so we don't loop forever.
        let mut depth = 0i32;
        let start = self.pos;
        loop {
            match self.peek() {
                Kind::End => break,
                Kind::LBrace   => { depth += 1; self.advance(); }
                Kind::RBrace   => { depth -= 1; self.advance(); if depth <= 0 { break; } }
                Kind::LBracket => { depth += 1; self.advance(); }
                Kind::RBracket => { depth -= 1; self.advance(); }
                Kind::LParen   => { depth += 1; self.advance(); }
                Kind::RParen   => { depth -= 1; self.advance(); }
                Kind::KwType if depth == 0 && self.pos > start => break,
                Kind::KwClass if depth == 0 && self.pos > start => break,
                Kind::HashHash(_) if depth == 0 && self.pos > start => break,
                _ => { self.advance(); }
            }
        }
        Ok(())
    }

    // --- type declaration ---

    fn parse_type(&mut self, table: TableCfg) -> Result<TypeDecl> {
        // `class` is the behavior-bearing sibling of `type` — identical field
        // grammar plus `fn` methods (plan 13a). Storage/REST are class-blind.
        let is_class = matches!(self.peek(), Kind::KwClass);
        if is_class {
            self.advance();
        } else {
            self.expect(&Kind::KwType, "`type` or `class`")?;
        }
        let name = self.expect_ident("type name")?;

        // Link types have a different header: `type Purchase link Customer -> Product { ... }`.
        // For Stage 2 we don't bind link-type behaviour, so parse-and-discard the body.
        let is_link = matches!(self.peek(), Kind::KwLink);
        if is_link {
            // consume link A -> B
            while !matches!(self.peek(), Kind::LBrace | Kind::End) {
                self.advance();
            }
        }

        self.expect(&Kind::LBrace, "'{'")?;

        let mut decl = TypeDecl {
            name, fields: Vec::new(), services: Vec::new(), is_class,
            methods: Vec::new(), table,
        };
        loop {
            self.skip_newlines();
            match self.peek() {
                Kind::RBrace => { self.advance(); break; }
                Kind::End    => bail!("unexpected end of input inside type body"),
                Kind::KwPolicy => self.skip_block_line()?,   // policy <read|write|...> ...
                Kind::KwOn     => self.skip_on_block()?,      // on update when ... do ...
                Kind::KwFn if is_class => {
                    // 13b: class methods parse for real — signature + DML body.
                    decl.methods.push(self.parse_method()?);
                }
                Kind::KwFn     => self.skip_block_line()?,    // fn inside a plain `type`:
                                                              // parse-and-discard (13a);
                                                              // brace depth keeps the body's
                                                              // `}` from closing the type
                Kind::KwService => decl.services.push(self.parse_service()?),
                Kind::Ident(_) => {
                    if is_link {
                        // Stage 2: absorb link-type bodies without interpreting them.
                        self.skip_block_line()?;
                    } else {
                        // Distinguish a field from a trailing junk line. Fields look like
                        // `ident : type ...`. Anything else → skip.
                        if self.looks_like_field() {
                            decl.fields.push(self.parse_field()?);
                        } else {
                            self.skip_block_line()?;
                        }
                    }
                }
                _ => self.skip_block_line()?,
            }
        }
        Ok(decl)
    }

    fn looks_like_field(&self) -> bool {
        // Lookahead: Ident followed by Colon (possibly after a hyphenated ident).
        let mut i = self.pos;
        match self.toks.get(i).map(|t| &t.kind) {
            Some(Kind::Ident(_)) => {}
            _ => return false,
        }
        i += 1;
        matches!(self.toks.get(i).map(|t| &t.kind), Some(Kind::Colon))
    }

    /// Skip tokens until the end of the current logical line (up to Newline or
    /// the outer RBrace). Used for policies, triggers, etc., whose full grammar
    /// is out of Stage 2 scope.
    fn skip_block_line(&mut self) -> Result<()> {
        let mut depth = 0i32;
        loop {
            match self.peek() {
                Kind::End => break,
                Kind::Newline if depth == 0 => { self.advance(); break; }
                Kind::RBrace if depth == 0 => break,  // stop before the outer `}` — caller handles it
                Kind::LBrace | Kind::LBracket | Kind::LParen => { depth += 1; self.advance(); }
                Kind::RBrace | Kind::RBracket | Kind::RParen => { depth -= 1; self.advance(); }
                _ => { self.advance(); }
            }
        }
        Ok(())
    }

    /// `on <event> [when ...] do <action>` possibly spanning many lines. Skip
    /// until the next top-level keyword inside the type body. Trigger bodies
    /// commonly contain `{ k: v, ... }` object literals and `( ... )` calls,
    /// so we track brace depth — RBrace only terminates when we're at the
    /// outermost level of the `on` block.
    fn skip_on_block(&mut self) -> Result<()> {
        self.advance(); // consume `on`
        let mut depth = 0i32;
        loop {
            match self.peek() {
                Kind::End => break,
                Kind::RBrace if depth == 0 => break,   // closes the surrounding type body
                Kind::LBrace | Kind::LBracket | Kind::LParen => {
                    depth += 1;
                    self.advance();
                }
                Kind::RBrace | Kind::RBracket | Kind::RParen => {
                    depth -= 1;
                    self.advance();
                }
                Kind::Newline if depth == 0 => {
                    // At depth 0, a newline may end the `on` block if the next
                    // meaningful token starts a new type-body item.
                    while matches!(self.peek(), Kind::Newline) { self.advance(); }
                    if matches!(self.peek(),
                        Kind::RBrace | Kind::KwPolicy | Kind::KwService
                        | Kind::KwOn | Kind::KwFn | Kind::End
                    ) { return Ok(()); }
                    if matches!(self.peek(), Kind::Ident(_)) && self.looks_like_field() {
                        return Ok(());
                    }
                }
                _ => { self.advance(); }
            }
        }
        Ok(())
    }

    // --- field parsing ---

    fn parse_field(&mut self) -> Result<Field> {
        let name = self.expect_ident("field name")?;
        self.expect(&Kind::Colon, "':'")?;
        let (ty, is_relation) = self.parse_field_ty()?;

        let mut nullable = false;
        if matches!(self.peek(), Kind::Question) {
            self.advance();
            nullable = true;
        }

        let mut unique = false;
        let mut default = None;
        loop {
            match self.peek() {
                Kind::At => {
                    self.advance();
                    let name = self.expect_ident("annotation name")?;
                    // Consume optional (...) argument block without interpreting it.
                    if matches!(self.peek(), Kind::LParen) {
                        let mut depth = 1i32;
                        self.advance();
                        while depth > 0 {
                            match self.peek() {
                                Kind::End => break,
                                Kind::LParen => { depth += 1; self.advance(); }
                                Kind::RParen => { depth -= 1; self.advance(); }
                                _ => { self.advance(); }
                            }
                        }
                    }
                    if name == "unique" { unique = true; }
                }
                Kind::Eq => {
                    self.advance();
                    default = Some(self.parse_default_expr()?);
                }
                Kind::Newline | Kind::RBrace | Kind::End => break,
                _ => {
                    // Skip any stray tokens until end-of-line — resilient to unhandled
                    // annotation forms like `@check(between 1 and 5)`.
                    self.advance();
                }
            }
        }

        Ok(Field { name, ty, nullable, unique, default, is_relation })
    }

    /// Returns (type, is_relation) — `ref`/`multi`/`backlink` are relations
    /// and don't carry a stored scalar column in this type.
    fn parse_field_ty(&mut self) -> Result<(FieldTy, bool)> {
        match self.peek() {
            Kind::KwRef => {
                self.advance();
                let target = self.expect_ident("ref target type")?;
                Ok((FieldTy::Ref(target), true))
            }
            Kind::KwMulti => {
                self.advance();
                let target = self.expect_ident("multi target type")?;
                // Either `@edge(:TAG)` or `via LinkType` or nothing (= `@edge(:target_upper)`).
                match self.peek() {
                    Kind::At => {
                        self.advance();
                        let ann = self.expect_ident("annotation name")?;
                        if ann != "edge" {
                            bail!("line {}: expected @edge, got @{ann}", self.peek_line());
                        }
                        self.expect(&Kind::LParen, "'('")?;
                        self.expect(&Kind::Colon, "':'")?;
                        let tag = self.expect_ident("edge tag")?;
                        self.expect(&Kind::RParen, "')'")?;
                        Ok((FieldTy::MultiEdge { target, tag: Some(tag) }, true))
                    }
                    Kind::KwVia => {
                        self.advance();
                        let link = self.expect_ident("link type name")?;
                        Ok((FieldTy::MultiVia { target, link }, true))
                    }
                    _ => Ok((FieldTy::MultiEdge { target, tag: None }, true))
                }
            }
            Kind::KwBacklink => {
                self.advance();
                let target = self.expect_ident("backlink target type")?;
                self.expect(&Kind::Dot, "'.'")?;
                let field = self.expect_ident("backlink field")?;
                Ok((FieldTy::Backlink { target, field }, true))
            }
            Kind::LBracket => {
                self.advance();
                let (inner, _) = self.parse_field_ty()?;
                self.expect(&Kind::RBracket, "']'")?;
                Ok((FieldTy::Array(Box::new(inner)), false))
            }
            Kind::LBrace => {
                self.advance();
                let mut fields = Vec::new();
                loop {
                    self.skip_newlines();
                    if matches!(self.peek(), Kind::RBrace) { self.advance(); break; }
                    if matches!(self.peek(), Kind::End)    { bail!("unexpected end in struct type"); }
                    fields.push(self.parse_field()?);
                    // Field end can be comma or just newline
                    self.accept(&Kind::Comma);
                }
                Ok((FieldTy::Struct(fields), false))
            }
            Kind::Ident(_) => {
                let first = self.expect_ident("type name")?;
                // Try tagged union: IDENT | IDENT | IDENT
                if matches!(self.peek(), Kind::Pipe) {
                    let mut variants = vec![first];
                    while matches!(self.peek(), Kind::Pipe) {
                        self.advance();
                        let next = self.expect_ident("union variant")?;
                        variants.push(next);
                    }
                    Ok((FieldTy::Union(variants), false))
                } else {
                    Ok((FieldTy::Scalar(first), false))
                }
            }
            other => bail!("line {}: expected type, got {other}", self.peek_line()),
        }
    }

    fn parse_default_expr(&mut self) -> Result<DefaultExpr> {
        // Fast path: a single literal followed by newline / `}` / `,`.
        if let Some(lit) = self.try_standalone_literal() {
            return Ok(lit);
        }

        // `now` or `now()` — recognise before falling into opaque slurp.
        if matches!(self.peek(), Kind::Ident(ref s) if s == "now") {
            self.advance();
            if matches!(self.peek(), Kind::LParen) {
                self.advance();
                if matches!(self.peek(), Kind::RParen) { self.advance(); }
            }
            // Only honour if the expression ends here; otherwise fall through
            // to the opaque path — `now + 5` etc. becomes opaque.
            if matches!(self.peek(), Kind::Newline | Kind::Comma | Kind::RBrace | Kind::End) {
                return Ok(DefaultExpr::Now);
            }
        }

        // Otherwise slurp a balanced expression until newline at depth 0.
        // This handles `now()`, `count(...)`, `words(meta.body_md)`, `self.xxx`,
        // and anything else we haven't modelled explicitly. Stage 2 treats the
        // result as opaque for execution, but the parser survives intact.
        let mut buf = String::new();
        let mut depth = 0i32;
        loop {
            match self.peek() {
                Kind::End => break,
                Kind::Newline | Kind::Comma if depth == 0 => break,
                Kind::RBrace if depth == 0 => break,
                Kind::LBrace | Kind::LBracket | Kind::LParen => {
                    depth += 1;
                    let t = self.advance();
                    buf.push_str(&format!("{} ", t.kind));
                }
                Kind::RBrace | Kind::RBracket | Kind::RParen => {
                    depth -= 1;
                    let t = self.advance();
                    buf.push_str(&format!("{} ", t.kind));
                }
                _ => {
                    let t = self.advance();
                    buf.push_str(&format!("{} ", t.kind));
                }
            }
        }
        let trimmed = buf.trim().to_string();
        // Friendly recognition of common forms.
        if trimmed.starts_with("now ") || trimmed == "now" {
            return Ok(DefaultExpr::Now);
        }
        Ok(DefaultExpr::Opaque(trimmed))
    }

    /// If the next token is a self-contained literal (not followed by more
    /// expression tokens), consume and return it. Otherwise leave the cursor
    /// alone and return None so the opaque path takes over.
    fn try_standalone_literal(&mut self) -> Option<DefaultExpr> {
        // Peek one ahead to decide if the literal is the whole expression.
        let follower_ends_expr = match self.toks.get(self.pos + 1).map(|t| &t.kind) {
            Some(Kind::Newline) | Some(Kind::End) | Some(Kind::RBrace) | Some(Kind::Comma) => true,
            _ => false,
        };
        if !follower_ends_expr { return None; }
        match self.peek().clone() {
            Kind::Str(s)   => { self.advance(); Some(DefaultExpr::Str(s)) }
            Kind::Int(n)   => { self.advance(); Some(DefaultExpr::Int(n)) }
            Kind::KwTrue   => { self.advance(); Some(DefaultExpr::Bool(true)) }
            Kind::KwFalse  => { self.advance(); Some(DefaultExpr::Bool(false)) }
            Kind::KwNull   => { self.advance(); Some(DefaultExpr::Null) }
            Kind::Ident(s) => { self.advance(); Some(DefaultExpr::Enum(s)) }
            _ => None,
        }
    }

    // --- service ---

    fn parse_service(&mut self) -> Result<ServiceDecl> {
        self.expect(&Kind::KwService, "`service`")?;
        let kind = match self.peek() {
            Kind::KwRest    => { self.advance(); ServiceKind::Rest }
            Kind::KwGraphql => { self.advance(); ServiceKind::Graphql }
            Kind::KwNative  => { self.advance(); ServiceKind::Native }
            Kind::Ident(s) if s == "rest"    => { self.advance(); ServiceKind::Rest }
            Kind::Ident(s) if s == "graphql" => { self.advance(); ServiceKind::Graphql }
            Kind::Ident(s) if s == "native"  => { self.advance(); ServiceKind::Native }
            other => bail!("line {}: expected `rest`/`graphql`/`native`, got {other}", self.peek_line()),
        };

        let path = match self.peek().clone() {
            Kind::Str(s) => { self.advance(); s }
            other => bail!("line {}: expected service path string, got {other}", self.peek_line()),
        };

        self.skip_newlines();
        self.expect(&Kind::KwExpose, "`expose`")?;
        let mut expose = Vec::new();
        loop {
            let name = self.expect_ident("operation name")?;
            expose.push(Operation::from_ident(&name));
            if !self.accept(&Kind::Comma) { break; }
        }
        Ok(ServiceDecl { kind, path, expose })
    }

    // --- class methods (plan 13b) ---

    /// `fn name([p: T, ...]) [-> Ret] [in txn [snapshot|serializable]] { body }`
    fn parse_method(&mut self) -> Result<MethodDecl> {
        self.expect(&Kind::KwFn, "`fn`")?;
        let name = self.expect_ident("method name")?;

        self.expect(&Kind::LParen, "'('")?;
        let mut params = Vec::new();
        self.skip_newlines();
        while !matches!(self.peek(), Kind::RParen) {
            let pname = self.expect_ident("parameter name")?;
            self.expect(&Kind::Colon, "':'")?;
            let pty = self.expect_ident("parameter type")?;
            params.push((pname, pty));
            self.skip_newlines();
            if !self.accept(&Kind::Comma) { break; }
            self.skip_newlines();
        }
        self.expect(&Kind::RParen, "')'")?;

        let mut ret = None;
        if self.accept(&Kind::Arrow) {
            // `-> Money` or `-> [Price]` — diagnostic-only in Stage 2.
            if self.accept(&Kind::LBracket) {
                let inner = self.expect_ident("return type")?;
                self.expect(&Kind::RBracket, "']'")?;
                ret = Some(format!("[{inner}]"));
            } else {
                ret = Some(self.expect_ident("return type")?);
            }
        }

        let mut txn = TxnMode::None;
        if self.accept(&Kind::KwIn) {
            self.expect(&Kind::KwTxn, "`txn`")?;
            txn = match self.peek() {
                Kind::KwSnapshot     => { self.advance(); TxnMode::Snapshot }
                Kind::KwSerializable => { self.advance(); TxnMode::Serializable }
                _ => TxnMode::Txn,
            };
        }

        self.skip_newlines();
        self.expect(&Kind::LBrace, "'{' to open method body")?;
        let body = self.parse_stmt_block()?;
        Ok(MethodDecl { name, params, ret, txn, body })
    }

    /// Statements until the matching `}` (consumed).
    fn parse_stmt_block(&mut self) -> Result<Vec<Stmt>> {
        let mut stmts = Vec::new();
        loop {
            self.skip_newlines();
            while self.accept(&Kind::Semicolon) { self.skip_newlines(); }
            match self.peek() {
                Kind::RBrace => { self.advance(); return Ok(stmts); }
                Kind::End    => bail!("unexpected end of input inside method body"),
                _ => stmts.push(self.parse_stmt()?),
            }
        }
    }

    fn parse_stmt(&mut self) -> Result<Stmt> {
        let stmt = match self.peek() {
            Kind::KwLet => {
                self.advance();
                let name = self.expect_ident("binding name")?;
                self.expect(&Kind::Eq, "'='")?;
                let expr = self.parse_expr()?;
                Stmt::Let { name, expr }
            }
            // Schema-layer DML `insert` is lowercase and deliberately NOT a
            // lexer keyword (only SQL-layer `INSERT` is) — same rule that
            // keeps `subscribe`/`me`/`self` usable as plain identifiers.
            Kind::Ident(s) if s == "insert" => {
                self.advance();
                let ty = self.expect_ident("type name after `insert`")?;
                self.expect(&Kind::LBrace, "'{'")?;
                let mut fields = Vec::new();
                loop {
                    self.skip_newlines();
                    if self.accept(&Kind::RBrace) { break; }
                    let fname = self.expect_ident("field name")?;
                    self.expect(&Kind::Colon, "':'")?;
                    let expr = self.parse_expr()?;
                    fields.push((fname, expr));
                    self.skip_newlines();
                    self.accept(&Kind::Comma);
                }
                Stmt::Insert { ty, fields }
            }
            Kind::KwReturn => {
                self.advance();
                let expr = if matches!(self.peek(),
                    Kind::Semicolon | Kind::Newline | Kind::RBrace | Kind::End)
                { None } else { Some(self.parse_expr()?) };
                Stmt::Return { expr }
            }
            Kind::KwAssert => {
                self.advance();
                let cond = self.parse_expr()?;
                let mut msg = None;
                if self.accept(&Kind::KwOtherwise) {
                    self.expect(&Kind::KwAbort, "`abort`")?;
                    if let Kind::Str(s) = self.peek().clone() {
                        self.advance();
                        msg = Some(s);
                    }
                }
                Stmt::Assert { cond, msg }
            }
            Kind::KwIf => {
                self.advance();
                let cond = self.parse_expr()?;
                self.skip_newlines();
                self.expect(&Kind::LBrace, "'{' after if condition")?;
                let then = self.parse_stmt_block()?;
                let mut otherwise = Vec::new();
                // `else` may sit on the next line.
                let mark = self.pos;
                self.skip_newlines();
                if self.accept(&Kind::KwElse) {
                    self.skip_newlines();
                    if matches!(self.peek(), Kind::KwIf) {
                        otherwise.push(self.parse_stmt()?);   // else-if chain
                    } else {
                        self.expect(&Kind::LBrace, "'{' after else")?;
                        otherwise = self.parse_stmt_block()?;
                    }
                } else {
                    self.pos = mark;   // no else — restore consumed newlines
                }
                return Ok(Stmt::If { cond, then, otherwise });
            }
            other => bail!(
                "line {}: unsupported statement in method body: {other} \
                 (13b executes `let`/`insert`/`return`/`assert`/`if`)",
                self.peek_line()
            ),
        };
        // Statement terminator: `;`, newline, or the closing `}`.
        match self.peek() {
            Kind::Semicolon | Kind::Newline => { self.advance(); }
            Kind::RBrace | Kind::End => {}
            other => bail!("line {}: expected end of statement, got {other}", self.peek_line()),
        }
        Ok(stmt)
    }

    // --- expressions (precedence climbing: or < and < cmp < add < mul < unary < postfix) ---

    fn parse_expr(&mut self) -> Result<Expr> {
        self.parse_or()
    }

    fn parse_or(&mut self) -> Result<Expr> {
        let mut lhs = self.parse_and()?;
        while self.accept(&Kind::KwOr) {
            let rhs = self.parse_and()?;
            lhs = Expr::Binary(BinOp::Or, Box::new(lhs), Box::new(rhs));
        }
        Ok(lhs)
    }

    fn parse_and(&mut self) -> Result<Expr> {
        let mut lhs = self.parse_cmp()?;
        while self.accept(&Kind::KwAnd) {
            let rhs = self.parse_cmp()?;
            lhs = Expr::Binary(BinOp::And, Box::new(lhs), Box::new(rhs));
        }
        Ok(lhs)
    }

    fn parse_cmp(&mut self) -> Result<Expr> {
        let lhs = self.parse_add()?;
        let op = match self.peek() {
            Kind::EqEq  => BinOp::Eq,
            Kind::NotEq => BinOp::Ne,
            Kind::Lt    => BinOp::Lt,
            Kind::LtEq  => BinOp::Le,
            Kind::Gt    => BinOp::Gt,
            Kind::GtEq  => BinOp::Ge,
            _ => return Ok(lhs),
        };
        self.advance();
        let rhs = self.parse_add()?;
        Ok(Expr::Binary(op, Box::new(lhs), Box::new(rhs)))
    }

    fn parse_add(&mut self) -> Result<Expr> {
        let mut lhs = self.parse_mul()?;
        loop {
            let op = match self.peek() {
                Kind::Plus => BinOp::Add,
                Kind::Dash => BinOp::Sub,
                _ => return Ok(lhs),
            };
            self.advance();
            let rhs = self.parse_mul()?;
            lhs = Expr::Binary(op, Box::new(lhs), Box::new(rhs));
        }
    }

    fn parse_mul(&mut self) -> Result<Expr> {
        let mut lhs = self.parse_unary()?;
        loop {
            let op = match self.peek() {
                Kind::Star    => BinOp::Mul,
                Kind::Slash   => BinOp::Div,
                Kind::Percent => BinOp::Mod,
                _ => return Ok(lhs),
            };
            self.advance();
            let rhs = self.parse_unary()?;
            lhs = Expr::Binary(op, Box::new(lhs), Box::new(rhs));
        }
    }

    fn parse_unary(&mut self) -> Result<Expr> {
        match self.peek() {
            Kind::Dash  => { self.advance(); Ok(Expr::Unary(UnOp::Neg, Box::new(self.parse_unary()?))) }
            Kind::KwNot => { self.advance(); Ok(Expr::Unary(UnOp::Not, Box::new(self.parse_unary()?))) }
            _ => self.parse_postfix(),
        }
    }

    /// Primary followed by `.field` chains.
    fn parse_postfix(&mut self) -> Result<Expr> {
        let mut e = self.parse_primary()?;
        while self.accept(&Kind::Dot) {
            let field = self.expect_ident("field name after '.'")?;
            e = Expr::Field(Box::new(e), field);
        }
        Ok(e)
    }

    fn parse_primary(&mut self) -> Result<Expr> {
        match self.peek().clone() {
            Kind::Int(n)  => { self.advance(); Ok(Expr::Int(n)) }
            Kind::Str(s)  => { self.advance(); Ok(Expr::Str(s)) }
            Kind::KwTrue  => { self.advance(); Ok(Expr::Bool(true)) }
            Kind::KwFalse => { self.advance(); Ok(Expr::Bool(false)) }
            Kind::KwNull  => { self.advance(); Ok(Expr::Null) }
            Kind::LParen  => {
                self.advance();
                let e = self.parse_expr()?;
                self.expect(&Kind::RParen, "')'")?;
                Ok(e)
            }
            Kind::Ident(name) => {
                self.advance();
                // `select Type{ ... }` — schema-layer select expression.
                // Lowercase `select` is an ident (only SQL `SELECT` is a
                // keyword); the two-token shape Ident + LBrace disambiguates
                // it from a variable named `select`.
                if name == "select"
                    && matches!(self.peek(), Kind::Ident(_))
                    && matches!(self.toks.get(self.pos + 1).map(|t| &t.kind), Some(Kind::LBrace))
                {
                    return self.parse_select_expr();
                }
                // `name(args)` — call form.
                if self.accept(&Kind::LParen) {
                    let mut args = Vec::new();
                    if !matches!(self.peek(), Kind::RParen) {
                        loop {
                            args.push(self.parse_expr()?);
                            if !self.accept(&Kind::Comma) { break; }
                        }
                    }
                    self.expect(&Kind::RParen, "')'")?;
                    Ok(Expr::Call(name, args))
                } else {
                    Ok(Expr::Ident(name))
                }
            }
            other => bail!("line {}: expected expression, got {other}", self.peek_line()),
        }
    }

    /// `select Type{ entries }` — per § Brace Disambiguation: an entry with a
    /// comparison operator is a predicate; a bare identifier is a projection.
    /// (`select` and the type name are already consumed up to the ident.)
    fn parse_select_expr(&mut self) -> Result<Expr> {
        let ty = self.expect_ident("type name after `select`")?;
        self.expect(&Kind::LBrace, "'{'")?;
        let mut predicates = Vec::new();
        let mut projection = Vec::new();
        loop {
            self.skip_newlines();
            if self.accept(&Kind::RBrace) { break; }
            let field = self.expect_ident("field name")?;
            let op = match self.peek() {
                Kind::EqEq  => Some(BinOp::Eq),
                Kind::NotEq => Some(BinOp::Ne),
                Kind::Lt    => Some(BinOp::Lt),
                Kind::LtEq  => Some(BinOp::Le),
                Kind::Gt    => Some(BinOp::Gt),
                Kind::GtEq  => Some(BinOp::Ge),
                _ => None,
            };
            match op {
                Some(op) => {
                    self.advance();
                    // RHS is an additive expression — comparisons don't chain.
                    let rhs = self.parse_add()?;
                    predicates.push((field, op, Box::new(rhs)));
                }
                None => projection.push(field),
            }
            self.skip_newlines();
            self.accept(&Kind::Comma);
        }
        Ok(Expr::Select { ty, predicates, projection })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_simple_type() {
        let src = r#"
type Article {
  id:    Id
  title: Text
  service rest "/api/articles" expose list, get, create
}
"#;
        let sch = parse(src).unwrap();
        assert_eq!(sch.types.len(), 1);
        let t = &sch.types[0];
        assert_eq!(t.name, "Article");
        assert_eq!(t.fields.len(), 2);
        assert_eq!(t.fields[0].name, "id");
        assert_eq!(t.fields[1].name, "title");
        assert_eq!(t.services.len(), 1);
        assert_eq!(t.services[0].path, "/api/articles");
        assert_eq!(t.services[0].expose, vec![
            Operation::List, Operation::Get, Operation::Create,
        ]);
    }

    #[test]
    fn parses_nullable_and_unique_and_default() {
        let src = r#"
type User {
  email:  Email @unique
  name:   Text
  avatar: Url?
  joined: Timestamp = now()
  admin:  Bool = false
}
"#;
        let sch = parse(src).unwrap();
        let t = &sch.types[0];
        assert!(t.fields[0].unique);
        assert!(t.fields[2].nullable);
        assert!(matches!(t.fields[3].default, Some(DefaultExpr::Now)));
        assert!(matches!(t.fields[4].default, Some(DefaultExpr::Bool(false))));
    }

    #[test]
    fn parses_relations() {
        let src = r#"
type Article {
  author:  ref User
  tags:    multi Tag @edge(:TAGGED_AS)
  related: multi Article @edge(:RELATED_TO)
}
"#;
        let sch = parse(src).unwrap();
        let t = &sch.types[0];
        assert!(matches!(t.fields[0].ty, FieldTy::Ref(ref n) if n == "User"));
        assert!(matches!(t.fields[1].ty, FieldTy::MultiEdge { ref target, .. } if target == "Tag"));
    }

    #[test]
    fn skips_policy_and_on_trigger() {
        let src = r#"
type Article {
  id: Id
  title: Text

  policy read  anyone
  policy write for role Admin

  on update when old.published == false and new.published == true
    do set self.published_at = now()

  service rest "/api/articles" expose list, get
}
"#;
        let sch = parse(src).unwrap();
        let t = &sch.types[0];
        assert_eq!(t.name, "Article");
        assert_eq!(t.fields.len(), 2);
        assert_eq!(t.services.len(), 1);
    }

    #[test]
    fn parses_inline_struct() {
        let src = r#"
type Product {
  meta: {
    title: Text
    tags:  [Text]
  }
}
"#;
        let sch = parse(src).unwrap();
        let t = &sch.types[0];
        if let FieldTy::Struct(fs) = &t.fields[0].ty {
            assert_eq!(fs.len(), 2);
            assert!(matches!(fs[1].ty, FieldTy::Array(_)));
        } else { panic!("expected struct") }
    }

    #[test]
    fn parses_union() {
        let src = r#"
type Order { status: Pending | Paid | Shipped }
"#;
        let sch = parse(src).unwrap();
        if let FieldTy::Union(v) = &sch.types[0].fields[0].ty {
            assert_eq!(v, &vec!["Pending".to_string(), "Paid".into(), "Shipped".into()]);
        } else { panic!("expected union") }
    }

    #[test]
    fn article_debug() {
        let src = std::fs::read_to_string(concat!(
            env!("CARGO_MANIFEST_DIR"), "/../../docs/examples/blog/types/article.wo"
        )).unwrap();
        let sch = parse(&src).unwrap();
        eprintln!("types: {}", sch.types.len());
        for t in &sch.types {
            eprintln!("  type {} — {} fields, {} services", t.name, t.fields.len(), t.services.len());
            for f in &t.fields { eprintln!("    field: {}", f.name); }
            for svc in &t.services { eprintln!("    service {:?} {} expose {:?}", svc.kind, svc.path, svc.expose); }
        }
        assert!(sch.types.iter().any(|t| t.name == "Article" && !t.services.is_empty()),
            "Article should have a service");
    }

    #[test]
    fn parses_class_with_methods() {
        let src = r#"
class Product {
  id:     Id
  sku:    SKU @unique
  name:   Text
  prices: multi Price

  fn current_price() -> Money in txn {
    return latest(self.prices).amount;
  }

  fn set_price(amount: Money) in txn {
    insert Price { product: self.id, amount: amount };
  }

  service rest "/api/products"
    expose list, get, create, update, delete, subscribe
}
"#;
        let sch = parse(src).unwrap();
        assert_eq!(sch.types.len(), 1);
        let t = &sch.types[0];
        assert!(t.is_class);
        assert_eq!(t.name, "Product");
        assert_eq!(t.fields.len(), 4);
        assert!(matches!(t.fields[3].ty, FieldTy::MultiEdge { ref target, .. } if target == "Price"));
        assert_eq!(t.services.len(), 1);
        assert_eq!(t.services[0].path, "/api/products");
        assert_eq!(t.services[0].expose.len(), 6);

        // 13b: methods parse into real AST.
        assert_eq!(t.methods.len(), 2);
        let cp = &t.methods[0];
        assert_eq!(cp.name, "current_price");
        assert!(cp.params.is_empty());
        assert_eq!(cp.ret.as_deref(), Some("Money"));
        assert_eq!(cp.txn, TxnMode::Txn);
        assert_eq!(cp.body.len(), 1);
        assert!(matches!(cp.body[0], Stmt::Return { expr: Some(_) }));

        let sp = &t.methods[1];
        assert_eq!(sp.name, "set_price");
        assert_eq!(sp.params, vec![("amount".to_string(), "Money".to_string())]);
        assert_eq!(sp.txn, TxnMode::Txn);
        assert!(matches!(&sp.body[0],
            Stmt::Insert { ty, fields } if ty == "Price" && fields.len() == 2));
    }

    #[test]
    fn parses_table_annotation() {
        let src = r#"
@table(name: "prices", index: [product, at], index: [sku])
class Price {
  id:      Id
  product: ref Product
  amount:  Money
  at:      Timestamp = now()
  sku:     SKU
}

@table
type Note { id: Id }

type Plain { id: Id }
"#;
        let sch = parse(src).unwrap();
        assert_eq!(sch.types.len(), 3);
        let p = &sch.types[0];
        assert_eq!(p.table.name.as_deref(), Some("prices"));
        assert_eq!(p.table.indexes, vec![
            vec!["product".to_string(), "at".to_string()],
            vec!["sku".to_string()],
        ]);
        // bare @table = legal no-op config
        let n = &sch.types[1];
        assert!(n.table.name.is_none() && n.table.indexes.is_empty());
        assert!(sch.types[2].table.indexes.is_empty());
    }

    #[test]
    fn table_annotation_error_cases() {
        // Reserved-for-later keys error loudly — no silent passthrough.
        let err = parse("@table(shard_key: sku)\ntype T { id: Id }").unwrap_err().to_string();
        assert!(err.contains("unknown @table argument `shard_key`"), "{err}");

        // @table must be followed by a type/class.
        assert!(parse("@table(name: \"x\")\nfn stray() {}").is_err());

        // Unknown annotation NAMES skip silently (field-annotation precedent).
        let sch = parse("@experimental(anything, at: all)\ntype T { id: Id }").unwrap();
        assert_eq!(sch.types.len(), 1);
        assert_eq!(sch.types[0].name, "T");
    }

    #[test]
    fn parses_select_expression() {
        let src = r#"
class Product {
  id: Id
  fn history() -> [Price] in txn {
    return select Price{ product == self.id, amount, at };
  }
}
"#;
        let m = &parse(src).unwrap().types[0].methods[0];
        let Stmt::Return { expr: Some(Expr::Select { ty, predicates, projection }) } = &m.body[0]
            else { panic!("expected return select, got {:?}", m.body[0]) };
        assert_eq!(ty, "Price");
        assert_eq!(predicates.len(), 1);
        assert_eq!(predicates[0].0, "product");
        assert!(matches!(predicates[0].1, BinOp::Eq));
        assert!(matches!(&*predicates[0].2, Expr::Field(b, f) if f == "id"
            && matches!(&**b, Expr::Ident(s) if s == "self")));
        assert_eq!(projection, &vec!["amount".to_string(), "at".to_string()]);
    }

    #[test]
    fn method_body_expression_ast() {
        let src = r#"
class Price {
  id:     Id
  amount: Money

  fn discounted(pct: Int) -> Money {
    return self.amount * (100 - pct) / 100;
  }
}
"#;
        let sch = parse(src).unwrap();
        let m = &sch.types[0].methods[0];
        assert_eq!(m.txn, TxnMode::None);
        let Stmt::Return { expr: Some(e) } = &m.body[0] else { panic!("expected return") };
        // ((self.amount * (100 - pct)) / 100) — mul level is left-associative.
        let Expr::Binary(BinOp::Div, lhs, rhs) = e else { panic!("expected /: {e:?}") };
        assert!(matches!(**rhs, Expr::Int(100)));
        let Expr::Binary(BinOp::Mul, base, paren) = &**lhs else { panic!("expected *") };
        assert!(matches!(&**base, Expr::Field(b, f) if f == "amount"
            && matches!(&**b, Expr::Ident(s) if s == "self")));
        assert!(matches!(&**paren, Expr::Binary(BinOp::Sub, _, _)));
    }

    #[test]
    fn class_method_braces_do_not_truncate_body() {
        // The method body's `}` and nested `{ ... }` literals must not be
        // mistaken for the class's closing brace — fields AFTER the methods
        // must still parse, and a following declaration must be seen.
        let src = r#"
class Price {
  id:     Id

  fn discounted(pct: Int) -> Money {
    if pct > 0 {
      return self.amount * (100 - pct) / 100;
    }
    return self.amount;
  }

  amount:   Money
  currency: Text = "EUR"
}

type Audit { id: Id }
"#;
        let sch = parse(src).unwrap();
        assert_eq!(sch.types.len(), 2);
        let p = &sch.types[0];
        assert!(p.is_class);
        assert_eq!(p.fields.len(), 3, "fields after the method must parse");
        assert_eq!(p.fields[1].name, "amount");
        assert!(!sch.types[1].is_class);
        assert_eq!(sch.types[1].name, "Audit");
    }
}
