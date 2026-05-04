//! `wo-gen` — client-SDK codegen.
//!
//! **Status: placeholder.** Phase 5 (see
//! [05-go-sdk.md § Typed SDK via `.wo` Schema Codegen](
//! ../../../docs/runtime/database/05-go-sdk.md)).
//!
//! Reads a compiled catalog (the same output [`ql`](../ql/index.html) +
//! [`engine`](../engine/index.html) produce from a `.wo` source tree)
//! and emits typed client code:
//!
//!   * **Go** — structs with `wo:"column"` tags, `*Client`,
//!     `TypedSubscription[T]` generics over channels
//!   * **TypeScript** — types + `fetch` + WebSocket subscriptions
//!   * **Rust** — `#[derive(Deserialize)]` structs + `impl Stream<Item = Delta>`
//!   * **Python** — dataclasses + `async for delta in sub`
//!   * **OpenAPI / GraphQL SDL** — machine-generated contracts served by
//!     [`http`](../http/index.html)
//!
//! Invoked as `wo gen sdk --lang go --out ./client` via the toolchain; a bin
//! target will be added when the crate has real behaviour. Until then this is
//! an empty library scaffold.
