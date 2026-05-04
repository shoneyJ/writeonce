//! `wo-db` — the Rust-facing database SDK.
//!
//! **Status: placeholder.** Phases 2–4 integrator.
//!
//! The top-level crate that the rest of the workspace (`wo-rt`, `wo-http`,
//! future `wo-serve` integration glue) depends on. Composes:
//!
//! | | |
//! | --- | --- |
//! | [`ql`](../ql/index.html) | parse |
//! | [`value`](../value/index.html) | row representation |
//! | [`engine`](../engine/index.html) | storage + execution |
//! | [`txn`](../txn/index.html) | transaction coordinator |
//! | [`wal`](../wal/index.html) | durability (Phase 3) |
//! | [`sub`](../sub/index.html) | live subscriptions (Phase 4) |
//!
//! Public surface the rest of the workspace will call:
//!
//! ```text
//! let db = db::open(&catalog, &options)?;
//! db.wo(ctx, ".wo source", params).await?;   // raw DML
//! db.tx(ctx, |tx| async move { ... }).await?;
//! let sub = db.subscribe(ctx, "LIVE SELECT ...", params).await?;
//! ```
//!
//! See [05-go-sdk.md](../../../docs/runtime/database/05-go-sdk.md) — the Go
//! SDK's shape mirrors this one; both speak the same `.wo` wire protocol.
