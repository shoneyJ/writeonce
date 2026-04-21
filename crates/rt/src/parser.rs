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
                Kind::KwType => sch.types.push(self.parse_type()?),
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
                Kind::HashHash(_) if depth == 0 && self.pos > start => break,
                _ => { self.advance(); }
            }
        }
        Ok(())
    }

    // --- type declaration ---

    fn parse_type(&mut self) -> Result<TypeDecl> {
        self.expect(&Kind::KwType, "`type`")?;
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

        let mut decl = TypeDecl { name, fields: Vec::new(), services: Vec::new() };
        loop {
            self.skip_newlines();
            match self.peek() {
                Kind::RBrace => { self.advance(); break; }
                Kind::End    => bail!("unexpected end of input inside type body"),
                Kind::KwPolicy => self.skip_block_line()?,   // policy <read|write|...> ...
                Kind::KwOn     => self.skip_on_block()?,      // on update when ... do ...
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
                        | Kind::KwOn | Kind::End
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
}
