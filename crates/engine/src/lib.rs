//! `wo-engine` — the in-memory executor.
//!
//! **Status: placeholder.** Phase 2 (see
//! [02-wo-language.md](../../../docs/runtime/database/02-wo-language.md))
//! designs the three storage paradigms; Phase 3
//! ([03-inmemory-engine.md](../../../docs/runtime/database/03-inmemory-engine.md))
//! details the RAM-primary layout, arenas, MVCC version chains.
//!
//! The engine owns:
//!   * relational tables — per-type `BTreeMap<id, Row>` today, B+ tree when
//!     sized appropriately
//!   * document collections — shape-free `Value::Object` rows; LSM when Phase 3
//!     activates
//!   * graph nodes + edges — adjacency over the type catalog
//!   * a schema catalog compiled from `wo-ql` output
//!
//! Stage 2 ships this as a module inside [`rt`](../rt/index.html).
//! It extracts here when `wo-wal` and `wo-txn` need to reach into the same
//! structures.
