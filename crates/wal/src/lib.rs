//! `wo-wal` — the write-ahead log.
//!
//! **Status: placeholder.** Phase 3 (see
//! [03-inmemory-engine.md](../../../docs/runtime/database/03-inmemory-engine.md)).
//!
//! Responsibilities:
//!   * append committed txn records to an on-disk ring buffer via `io_uring`
//!   * issue `IORING_OP_FSYNC` with a linked SQE for durability
//!   * batch concurrent commits into one fsync per tick (group commit)
//!   * drive crash recovery by replaying the log from the last checkpoint
//!
//! Under the single-threaded event-loop design, the WAL is owned directly by
//! the main loop — no separate WAL-writer thread. `io_uring`'s kernel-owned
//! SQPOLL handler does the draining; userland just submits and parks on CQEs.
//!
//! This is the crate that turns the RAM-primary engine from "cache" into
//! "database": commits aren't acked until their WAL record is on the SSD.
