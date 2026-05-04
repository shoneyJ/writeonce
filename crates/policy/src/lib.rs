//! `wo-policy` — RBAC + row-level policies.
//!
//! **Status: placeholder.** Phase 6 (see
//! [06-lowcode-fullstack.md](../../../docs/runtime/database/06-lowcode-fullstack.md)).
//!
//! Compiles type-attached `policy` blocks (and standalone `##policy` blocks
//! for cross-entity rules) into predicates that [`engine`](../engine/index.html)
//! AND-s into every query at plan time. The rule is enforced once, at the
//! planner, so no caller can bypass it — not the REST handler, not the
//! `wo-db` SDK, not an admin script calling `fn` directly.
//!
//! Shape of a compiled policy:
//!
//! ```text
//! for type Article:
//!   read:   published == true     OR $session.role == Admin
//!                                  OR author == $session.user
//!   write:  $session.role == Admin OR ($session.role == Author && author == $session.user)
//!   delete: $session.role == Admin
//! ```
//!
//! The planner merges the read predicate as a `WHERE` conjunct, the write
//! predicate as an assertion inside UPDATE/INSERT codegen, and delete as a
//! DELETE guard. Same mechanism as Postgres RLS — just authored declaratively
//! in the `type` block instead of via SQL migrations.
