//! `app` — `##app` manifest: routes, theme, i18n, startup hooks.
//!
//! **Status: placeholder.** Phase 6 (see
//! [06-lowcode-fullstack.md § `##app`](
//! ../../../docs/runtime/database/06-lowcode-fullstack.md)).
//!
//! One `##app` block per project. Declares:
//!   * the static route table (URL path → `ui.<screen>` binding, possibly
//!     parameterised by dynamic segments like `/article/:slug`)
//!   * cross-entity policies ("Admin bypasses row-level filters on every type")
//!   * `on startup do …` hooks — idempotent seed code that runs once before
//!     [`http`](../http/index.html) binds a listening socket
//!   * theme tokens, i18n locale set, project metadata
//!
//! Consumed by [`ui`](../ui/index.html) for route rendering and by
//! [`policy`](../policy/index.html) for the cross-entity rules block.
//!
//! The [`docs/examples/blog/app.wo`](../../../docs/examples/blog/app.wo) and
//! [`docs/examples/ecommerce/app.wo`](../../../docs/examples/ecommerce/app.wo)
//! files are the reference shapes.
