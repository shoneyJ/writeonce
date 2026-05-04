//! `wo-sub` — the live-subscription engine.
//!
//! **Status: placeholder.** Phase 4 (see
//! [04-client-api.md](../../../docs/runtime/database/04-client-api.md)).
//!
//! Responsibilities:
//!   * per-connection subscription registry — predicate + bound parameters
//!   * commit-path dispatch: each [`txn`](../txn/index.html) commit
//!     walks the registry, matches predicates against the delta set, and
//!     pushes `Insert` / `Update` / `Delete` frames into per-subscription
//!     ring buffers
//!   * back-pressure policy (drop-and-resync / coalesce / disconnect) —
//!     mirrors the client-side policy in [`wo-db`](../db/index.html)
//!     / the Go SDK
//!
//! Stage 2 stubs the `/api/<type>/live` endpoint to 501 in
//! [`rt::server`](../rt/server/index.html). Stage 3 wires this crate
//! behind that route and upgrades the connection to WebSocket.
//!
//! Not related to the v1 `reference/crates/wo-sub/` crate — that one did
//! polling-based diff delivery over SSE for the old flat-file blog.
