//! `wo-txn` — the cross-paradigm transaction coordinator.
//!
//! **Status: placeholder.** Phase 2 milestone 3 (see
//! [02-wo-language.md § Cross-Paradigm Transaction Coordinator](
//! ../../../docs/runtime/database/02-wo-language.md)).
//!
//! Responsibilities:
//!   * `txn_id` allocation + snapshot timestamps
//!   * ordering commits across `wo-engine`'s three storage paradigms
//!   * the transaction-scoped **`RETURNING` alias table** — the state that
//!     lets `INSERT … RETURNING id AS oid` thread `$oid` into a later
//!     `CREATE (u)-[:PURCHASED {order_id: $oid}]->(p)` in the same
//!     `BEGIN … COMMIT` block
//!   * driving the in-process 2PC between the three engines on commit
//!
//! Under the single-threaded event-loop design (see
//! [§ Concurrency Model](../../../docs/runtime/database/02-wo-language.md#concurrency-model))
//! this reduces to a sequential counter + a per-txn name table. When the
//! runtime grows past one core via sharding, this crate owns cross-shard 2PC.
