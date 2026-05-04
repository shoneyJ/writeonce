//! `service` — endpoint registration and dispatch.
//!
//! **Status: placeholder.** Phase 6 (see
//! [06-lowcode-fullstack.md § `##service`](
//! ../../../docs/runtime/database/06-lowcode-fullstack.md)).
//!
//! Walks every `service rest "/api/..." expose ...` block declared on a type
//! (and every standalone `##service` bundle for multi-entity APIs) and binds
//! handlers on [`http`](../http/index.html)'s router. Each exposed operation
//! (`list` / `get` / `create` / `update` / `delete` / `subscribe` / `me`)
//! maps to a predetermined shape — declarative CRUD is the whole point.
//!
//! `fn` endpoints (`POST /api/fn/checkout`) are also dispatched from here —
//! the service declares the function-as-endpoint wiring, [`logic`](../logic/index.html)
//! executes.
//!
//! Stage 2 does this work inline inside [`rt::server`](../rt/server/index.html).
//! It extracts here when Phase 6 adds GraphQL + native-protocol surfaces that
//! share the same service-block source of truth.
