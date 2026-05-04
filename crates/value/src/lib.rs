//! `wo-value` — the runtime's tagged `Value` type plus dotted-path helpers.
//!
//! **Status: placeholder.** Phase 2 foundation.
//!
//! Carries the union of all scalar + document + array shapes the engines pass
//! around: `Null`, `Bool`, `Int`, `Str`, `Array`, `Object`. Plus `fetch_path`
//! / `assign_path` utilities that drive the dotted-access semantics shared by
//! SQL expressions, document `UPDATE SET meta.x.y = z`, and Cypher projections
//! (see [the two-layer language spec](
//! ../../../docs/runtime/database/02-wo-language.md)).
//!
//! Stage 2 keeps these inside [`rt`](../rt/index.html)'s `engine` module;
//! they extract here once a second consumer (the WAL serializer, the wire codec)
//! needs them.
