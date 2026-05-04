//! `wo-logic` — triggers and stored-function interpreter.
//!
//! **Status: placeholder.** Phase 6 (see
//! [06-lowcode-fullstack.md § `##logic`](
//! ../../../docs/runtime/database/06-lowcode-fullstack.md)).
//!
//! Two sources of runtime code:
//!   * **Type-attached triggers** — `on insert | update | delete [when <pred>]
//!     do <action>` declared inside a `type` body. Fires inside the committing
//!     transaction so timestamp stamps (`paid_at`, `shipped_at`) are atomic
//!     with the state change, never observable half-way.
//!   * **Standalone `##logic`** — cross-entity workflows like the ecommerce
//!     on-order-placed hook that decrements inventory on every line item.
//!   * **`fn name(args) in txn ... { ... }`** — transactional stored
//!     functions (the `checkout` in `docs/examples/ecommerce/logic/`).
//!
//! All three compile to the same intermediate form that [`txn`](../txn/index.html)
//! invokes during commit (for triggers) or directly on the main loop (for
//! `fn` calls from REST/native wire).
