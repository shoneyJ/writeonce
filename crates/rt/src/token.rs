//! Token definitions for the `.wo` lexer.

use std::fmt;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Kind {
    // literals
    Ident(String),
    Str(String),
    Int(i64),
    Param(String),          // $name

    // keywords (schema + query layer)
    KwType,
    KwRef,
    KwMulti,
    KwVia,
    KwBacklink,
    KwLink,
    KwService,
    KwRest,
    KwGraphql,
    KwNative,
    KwExpose,
    KwPolicy,
    KwFor,
    KwRole,
    KwWhen,
    KwAnyone,
    KwOn,
    KwDo,
    KwSet,
    KwCall,
    KwEmit,
    KwEnqueue,
    KwAssert,
    KwOtherwise,
    KwAbort,
    KwReturn,
    KwReturning,
    KwAs,
    KwFn,
    KwIn,
    KwTxn,
    KwSnapshot,
    KwSerializable,
    KwBegin,
    KwCommit,
    KwRollback,
    KwSavepoint,
    KwTo,
    KwLive,
    KwInsert,
    KwInto,
    KwValues,
    KwUpdate,
    KwDelete,
    KwSelect,
    KwFrom,
    KwWhere,
    KwMatch,
    KwCreate,
    KwLet,
    KwIf,
    KwElse,
    KwFor1,            // the other `for` — for/each loop (disambiguated at parse time)
    KwEach,
    KwContains,
    KwAnd,
    KwOr,
    KwNot,
    KwTrue,
    KwFalse,
    KwNull,
    KwTest,
    KwMain,
    KwApp,
    KwStartup,

    // block markers
    HashHash(String),  // `##sql`, `##doc`, `##graph`, `##ui`, `##app`, `##policy`, `##service`, `##logic`
    Hash(String),      // `#table-name`

    // punctuation
    LBrace,            // {
    RBrace,            // }
    LParen,            // (
    RParen,            // )
    LBracket,          // [
    RBracket,          // ]
    Comma,
    Semicolon,
    Colon,
    Dot,
    DotDot,            // ..
    DotStar,           // .*   used in `line_items.*.qty`
    Question,          // ?
    At,                // @
    Pipe,              // |
    Arrow,             // ->
    FatArrow,          // =>
    Dash,              // -
    Plus,
    Star,
    Slash,
    Percent,
    Eq,                // =
    EqEq,              // ==
    NotEq,             // !=
    Lt, LtEq,
    Gt, GtEq,
    PlusEq, MinusEq,

    // meta
    Newline,
    End,
}

#[derive(Debug, Clone)]
pub struct Token {
    pub kind: Kind,
    pub line: u32,
    pub col:  u32,
}

impl fmt::Display for Kind {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Kind::Ident(s)    => write!(f, "ident({s})"),
            Kind::Str(s)      => write!(f, "\"{s}\""),
            Kind::Int(i)      => write!(f, "{i}"),
            Kind::Param(s)    => write!(f, "${s}"),
            Kind::HashHash(s) => write!(f, "##{s}"),
            Kind::Hash(s)     => write!(f, "#{s}"),
            k                 => write!(f, "{k:?}"),
        }
    }
}
