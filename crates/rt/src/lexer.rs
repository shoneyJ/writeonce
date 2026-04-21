//! Tokenizer for `.wo` source. Emits a sequential stream of [`Token`]s
//! suitable for the recursive-descent parser in [`crate::parser`].

use crate::token::{Kind, Token};
use anyhow::{bail, Result};

pub fn tokenize(src: &str) -> Result<Vec<Token>> {
    Lexer::new(src).lex()
}

struct Lexer<'a> {
    bytes: &'a [u8],
    pos:   usize,
    line:  u32,
    col:   u32,
}

impl<'a> Lexer<'a> {
    fn new(src: &'a str) -> Self {
        Self { bytes: src.as_bytes(), pos: 0, line: 1, col: 1 }
    }

    fn peek(&self) -> Option<u8>            { self.bytes.get(self.pos).copied() }
    fn peek_at(&self, n: usize) -> Option<u8> { self.bytes.get(self.pos + n).copied() }

    fn advance(&mut self) -> Option<u8> {
        let c = self.peek()?;
        self.pos += 1;
        if c == b'\n' { self.line += 1; self.col = 1; } else { self.col += 1; }
        Some(c)
    }

    fn lex(mut self) -> Result<Vec<Token>> {
        let mut out = Vec::new();
        while let Some(c) = self.peek() {
            let line = self.line;
            let col  = self.col;

            // line comment: -- ... EOL
            if c == b'-' && self.peek_at(1) == Some(b'-') {
                while let Some(c) = self.peek() {
                    if c == b'\n' { break; }
                    self.advance();
                }
                continue;
            }

            // newline → significant (ends trigger/policy lines, etc.)
            if c == b'\n' {
                self.advance();
                if out.last().map(|t: &Token| matches!(t.kind, Kind::Newline)) != Some(true) {
                    out.push(Token { kind: Kind::Newline, line, col });
                }
                continue;
            }

            // plain whitespace
            if c == b' ' || c == b'\t' || c == b'\r' {
                self.advance();
                continue;
            }

            // ## block marker
            if c == b'#' && self.peek_at(1) == Some(b'#') {
                self.advance(); self.advance();
                let name = self.read_ident_chars();
                out.push(Token { kind: Kind::HashHash(name), line, col });
                continue;
            }

            // # name
            if c == b'#' {
                self.advance();
                let name = self.read_ident_chars();
                out.push(Token { kind: Kind::Hash(name), line, col });
                continue;
            }

            // $name
            if c == b'$' {
                self.advance();
                let name = self.read_ident_chars();
                if name.is_empty() {
                    bail!("line {line}: expected parameter name after '$'");
                }
                out.push(Token { kind: Kind::Param(name), line, col });
                continue;
            }

            // string literal (single or double quote)
            if c == b'"' || c == b'\'' {
                let quote = c;
                self.advance();
                let mut s = String::new();
                while let Some(c) = self.peek() {
                    if c == quote { self.advance(); break; }
                    if c == b'\\' {
                        self.advance();
                        match self.advance() {
                            Some(b'n')  => s.push('\n'),
                            Some(b't')  => s.push('\t'),
                            Some(b'\\') => s.push('\\'),
                            Some(b'"')  => s.push('"'),
                            Some(b'\'') => s.push('\''),
                            Some(other) => s.push(other as char),
                            None        => bail!("line {line}: unterminated string escape"),
                        }
                        continue;
                    }
                    s.push(self.advance().unwrap() as char);
                }
                out.push(Token { kind: Kind::Str(s), line, col });
                continue;
            }

            // integer literal
            if c.is_ascii_digit() {
                let mut n: i64 = 0;
                while let Some(d) = self.peek() {
                    if !d.is_ascii_digit() { break; }
                    n = n.saturating_mul(10) + (d - b'0') as i64;
                    self.advance();
                }
                out.push(Token { kind: Kind::Int(n), line, col });
                continue;
            }

            // identifier / keyword
            if c.is_ascii_alphabetic() || c == b'_' {
                let name = self.read_ident_chars();
                let kind = match name.as_str() {
                    "type"       => Kind::KwType,
                    "ref"        => Kind::KwRef,
                    "multi"      => Kind::KwMulti,
                    "via"        => Kind::KwVia,
                    "backlink"   => Kind::KwBacklink,
                    "link"       => Kind::KwLink,
                    "service"    => Kind::KwService,
                    "rest"       => Kind::KwRest,
                    "graphql"    => Kind::KwGraphql,
                    "native"     => Kind::KwNative,
                    "expose"     => Kind::KwExpose,
                    "policy"     => Kind::KwPolicy,
                    "for"        => Kind::KwFor,
                    "role"       => Kind::KwRole,
                    "when"       => Kind::KwWhen,
                    "anyone"     => Kind::KwAnyone,
                    "on"         => Kind::KwOn,
                    "do"         => Kind::KwDo,
                    "set"        => Kind::KwSet,
                    "call"       => Kind::KwCall,
                    "emit"       => Kind::KwEmit,
                    "enqueue"    => Kind::KwEnqueue,
                    "assert"     => Kind::KwAssert,
                    "otherwise"  => Kind::KwOtherwise,
                    "abort"      => Kind::KwAbort,
                    "return"     => Kind::KwReturn,
                    "returning"  => Kind::KwReturning,
                    "RETURNING"  => Kind::KwReturning,
                    "as"         => Kind::KwAs,
                    "AS"         => Kind::KwAs,
                    "fn"         => Kind::KwFn,
                    "in"         => Kind::KwIn,
                    "txn"        => Kind::KwTxn,
                    "snapshot"   => Kind::KwSnapshot,
                    "serializable" => Kind::KwSerializable,
                    "BEGIN"      => Kind::KwBegin,
                    "COMMIT"     => Kind::KwCommit,
                    "ROLLBACK"   => Kind::KwRollback,
                    "SAVEPOINT"  => Kind::KwSavepoint,
                    "TO"         => Kind::KwTo,
                    "LIVE"       => Kind::KwLive,
                    "live"       => Kind::KwLive,     // lowercase `live` used in UI blocks
                    // `subscribe`, `receive`, `expect_abort` stay as plain idents so
                    // `expose ... subscribe` works in `service rest` blocks.
                    "INSERT"     => Kind::KwInsert,
                    "INTO"       => Kind::KwInto,
                    "VALUES"     => Kind::KwValues,
                    "UPDATE"     => Kind::KwUpdate,
                    "DELETE"     => Kind::KwDelete,
                    "SELECT"     => Kind::KwSelect,
                    "FROM"       => Kind::KwFrom,
                    "WHERE"      => Kind::KwWhere,
                    "MATCH"      => Kind::KwMatch,
                    "CREATE"     => Kind::KwCreate,
                    "SET"        => Kind::KwSet,
                    "let"        => Kind::KwLet,
                    "if"         => Kind::KwIf,
                    "else"       => Kind::KwElse,
                    "each"       => Kind::KwEach,
                    "contains"   => Kind::KwContains,
                    "and"        => Kind::KwAnd,
                    "AND"        => Kind::KwAnd,
                    "or"         => Kind::KwOr,
                    "OR"         => Kind::KwOr,
                    "not"        => Kind::KwNot,
                    "NOT"        => Kind::KwNot,
                    "true"       => Kind::KwTrue,
                    "false"      => Kind::KwFalse,
                    "null"       => Kind::KwNull,
                    "test"       => Kind::KwTest,
                    "main"       => Kind::KwMain,
                    "startup"    => Kind::KwStartup,
                    _            => Kind::Ident(name),
                };
                out.push(Token { kind, line, col });
                continue;
            }

            // punctuation & operators
            let kind = match c {
                b'{' => { self.advance(); Kind::LBrace }
                b'}' => { self.advance(); Kind::RBrace }
                b'(' => { self.advance(); Kind::LParen }
                b')' => { self.advance(); Kind::RParen }
                b'[' => { self.advance(); Kind::LBracket }
                b']' => { self.advance(); Kind::RBracket }
                b',' => { self.advance(); Kind::Comma }
                b';' => { self.advance(); Kind::Semicolon }
                b':' => { self.advance(); Kind::Colon }
                b'.' => {
                    self.advance();
                    match self.peek() {
                        Some(b'.') => { self.advance(); Kind::DotDot }
                        Some(b'*') => { self.advance(); Kind::DotStar }
                        _          => Kind::Dot,
                    }
                }
                b'?' => { self.advance(); Kind::Question }
                b'@' => { self.advance(); Kind::At }
                b'|' => { self.advance(); Kind::Pipe }
                b'-' => {
                    self.advance();
                    match self.peek() {
                        Some(b'>') => { self.advance(); Kind::Arrow }
                        Some(b'=') => { self.advance(); Kind::MinusEq }
                        _          => Kind::Dash,
                    }
                }
                b'+' => {
                    self.advance();
                    match self.peek() {
                        Some(b'=') => { self.advance(); Kind::PlusEq }
                        _          => Kind::Plus,
                    }
                }
                b'*' => { self.advance(); Kind::Star }
                b'/' => { self.advance(); Kind::Slash }
                b'%' => { self.advance(); Kind::Percent }
                b'=' => {
                    self.advance();
                    match self.peek() {
                        Some(b'=') => { self.advance(); Kind::EqEq }
                        Some(b'>') => { self.advance(); Kind::FatArrow }
                        _          => Kind::Eq,
                    }
                }
                b'!' => {
                    self.advance();
                    match self.peek() {
                        Some(b'=') => { self.advance(); Kind::NotEq }
                        _          => bail!("line {line}: expected '!=' got '!'"),
                    }
                }
                b'<' => {
                    self.advance();
                    match self.peek() {
                        Some(b'=') => { self.advance(); Kind::LtEq }
                        _          => Kind::Lt,
                    }
                }
                b'>' => {
                    self.advance();
                    match self.peek() {
                        Some(b'=') => { self.advance(); Kind::GtEq }
                        _          => Kind::Gt,
                    }
                }
                other => bail!("line {line}, col {col}: unexpected character {:?}", other as char),
            };
            out.push(Token { kind, line, col });
        }

        out.push(Token { kind: Kind::End, line: self.line, col: self.col });
        Ok(out)
    }

    fn read_ident_chars(&mut self) -> String {
        let start = self.pos;
        while let Some(c) = self.peek() {
            if c.is_ascii_alphanumeric() || c == b'_' || c == b'-' {
                self.advance();
            } else {
                break;
            }
        }
        String::from_utf8_lossy(&self.bytes[start..self.pos]).into_owned()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn lexes_type_header() {
        let toks = tokenize("type Article {").unwrap();
        let kinds: Vec<_> = toks.iter().map(|t| format!("{:?}", t.kind)).collect();
        assert_eq!(kinds, vec![
            "KwType".to_string(),
            "Ident(\"Article\")".to_string(),
            "LBrace".to_string(),
            "End".to_string(),
        ]);
    }

    #[test]
    fn lexes_string_and_int_and_param() {
        let toks = tokenize(r#"VALUES ($uid, 'hello', 42)"#).unwrap();
        let kinds: Vec<_> = toks.iter().map(|t| &t.kind).cloned().collect();
        assert!(kinds.contains(&Kind::KwValues));
        assert!(kinds.contains(&Kind::Param("uid".into())));
        assert!(kinds.contains(&Kind::Str("hello".into())));
        assert!(kinds.contains(&Kind::Int(42)));
    }

    #[test]
    fn skips_line_comments() {
        let toks = tokenize("-- comment\ntype X {}").unwrap();
        // First non-newline non-comment token should be `type`.
        let first_meaningful = toks.iter().find(|t| !matches!(t.kind, Kind::Newline)).unwrap();
        assert_eq!(first_meaningful.kind, Kind::KwType);
    }

    #[test]
    fn lexes_hash_markers() {
        let toks = tokenize("##ui\n#article-list").unwrap();
        assert!(matches!(toks[0].kind, Kind::HashHash(ref s) if s == "ui"));
        assert!(matches!(toks.iter().find(|t| matches!(t.kind, Kind::Hash(_))).unwrap().kind,
            Kind::Hash(ref s) if s == "article-list"));
    }
}
