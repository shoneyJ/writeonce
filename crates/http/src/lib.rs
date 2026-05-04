//! `wo-http` — HTTP + WebSocket + native wire protocol for the `.wo` runtime.
//!
//! **Status: placeholder.** Phase 4 (see
//! [04-client-api.md](../../../docs/runtime/database/04-client-api.md)).
//!
//! Three protocol surfaces, one dispatch:
//!   * **REST** — JSON in/out, one route per `service rest` operation.
//!   * **GraphQL over WebSocket** — SDL auto-generated from the schema; query,
//!     mutation, `subscription` all routed through the same planner.
//!   * **Native binary** — typed wire codec for `wo-db` and the Go SDK.
//!
//! Authentication middleware (`WithAPIKey`, `WithJWT`, `WithMTLS`) lives in
//! this crate — cross-cutting over every protocol surface.
//!
//! Stage 2 ships a minimal axum-based REST server inside
//! [`rt::server`](../rt/server/index.html). It migrates here when
//! Phase 4 activates and GraphQL / native surfaces join REST.
//!
//! Not the v1 `reference/crates/wo-http/` — that was a from-scratch HTTP
//! parser for the old single-threaded event loop.
