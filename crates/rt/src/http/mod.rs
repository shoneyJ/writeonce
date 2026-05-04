//! Hand-rolled HTTP/1.1 — phase 03 of the runtime plan.
//!
//! The transport layer for the `wo` binary after the phase-04 cutover.
//! Drives non-blocking accept + per-connection state machines off the
//! phase-02 [`EventLoop`](super::runtime::EventLoop). No `tokio`, no
//! `axum`, no `hyper`. Synchronous handlers; close-after-response (the
//! v1 model — keep-alive lands when a sample needs it).
//!
//! See `docs/plan/03-hand-rolled-http.md` and `docs/plan/04-cutover-remove-tokio-axum.md`.

mod connection;
mod listener;
mod request;
mod response;
mod route;

pub use connection::{Connection, ConnState};
pub use listener::Listener;
pub use request::{Method, Request};
pub use response::{Response, Status};
pub use route::{RouteParams, Router};
