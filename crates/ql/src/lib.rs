//! `wo-ql` — the `.wo` grammar: lexer, parser, AST.
//!
//! **Status: placeholder.** Phase 2 of the [`.wo` runtime design](
//! ../../../docs/runtime/database/02-wo-language.md).
//!
//! Stage 2 of the runtime (shipped) carries the lexer/parser/AST inside
//! [`rt`](../rt/index.html) as part of its monolithic cut. This crate
//! is where those modules move to when Stage 3+ needs the grammar from
//! multiple places — the HTTP raw-query handler, `wo-gen`, the server-side
//! `fn` interpreter.
//!
//! Reference implementation: the C++ prototype at
//! [`prototypes/wo-db/`](../../../prototypes/wo-db/).
