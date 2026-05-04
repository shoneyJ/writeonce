//! `ui` — `##ui` screens → SSR HTML + client-side delta runtime.
//!
//! **Status: placeholder.** Phase 6 (see
//! [06-lowcode-fullstack.md § `##ui`](
//! ../../../docs/runtime/database/06-lowcode-fullstack.md)).
//!
//! Declarative screens compile to:
//!   * a **server-rendered HTML** tree — a compact template + data-binding
//!     form computed from the screen's `source:` projection and `columns:` /
//!     `sections:` declarations
//!   * a **client-side runtime** (~50 KB vanilla JS) — opens a WebSocket to
//!     the engine's [`sub`](../sub/index.html) endpoint, binds incoming delta
//!     frames to DOM fragments by row key, handles sort / filter / paginate
//!     without refetching
//!   * an **auto-admin fallback** — any declared `type` without an explicit
//!     `##ui` gets a generic CRUD screen
//!
//! `live: true` on a screen auto-generates a matching `LIVE SELECT` that the
//! client runtime subscribes to — no hand-written subscription code on the UI
//! side.
//!
//! The [`docs/examples/blog/ui/`](../../../docs/examples/blog/ui/) and
//! [`docs/examples/ecommerce/ui/`](../../../docs/examples/ecommerce/ui/)
//! directories are the reference shapes this crate has to handle.
