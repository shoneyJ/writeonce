# 04 — Workspace layout + `wo.toml` grammar

**Context sources:** [`./00-overview.md`](./00-overview.md) §§ "Target layout" (L73–117), "Design decisions" 4–5 (L33–34), [`docs/examples/ecommerce/wo.toml`](../../examples/ecommerce/wo.toml) (the workspace manifest already in the repo), [`docs/examples/ecommerce/apps/storefront/wo.toml`](../../examples/ecommerce/apps/storefront/wo.toml) (the per-app manifest already in the repo), [`docs/examples/blog/`](../../examples/blog/) (the degenerate single-app form).

## Goal

Lock the `wo.toml` grammar at both workspace and per-app scope, fill any structural gaps in `docs/examples/ecommerce/` so its layout matches 00-overview's "Target layout" exactly, and ship a Rust workspace loader (`crates/app/src/workspace.rs` + sibling) that walks a workspace root, reads each app's manifest, and resolves `{{> partial}}` lookups across `shared = […]` directories first-match-wins.

## Design decisions (locked)

1. **`wo.toml` is TOML.** Not `.wo`. The workspace + app manifests already exist in the repo using TOML; this phase formalises the schema and adds a parser. Anchored in [`docs/examples/ecommerce/wo.toml`](../../examples/ecommerce/wo.toml).
2. **Path-based shared dependencies, no registry.** `shared = ["../../shared/types", …]` resolves at parse time relative to the per-app `wo.toml`. No semver, no fetch. Anchored in [`./00-overview.md`](./00-overview.md) decision 4.
3. **Workspace root vs. single app, by `kind`/`app_kind` field.** A `wo.toml` with `kind = "workspace"` triggers workspace loading and reads `[workspace]`. A `wo.toml` with `app_kind = "app"` is a leaf app. A `wo.toml` with neither is a degenerate single-app workspace (the blog example) — loaded as if it were `apps/<itself>`.
4. **One screen per directory.** `apps/<X>/ui/<screen>/{<screen>.wo, <screen>.htmlx, <screen>.css}` is locked layout. The loader walks `apps/<X>/ui/*/` and registers each subdirectory as a screen. Anchored in [`./00-overview.md`](./00-overview.md) decision 5 + Target Layout (L98–103).
5. **Component resolution: app-local first, then shared in declared order.** `{{> money amount=x}}` looks in `apps/<X>/ui/components/` first, then each path in `[dependencies] shared = […]` in order. First match wins. Errors at compile time if the partial cannot be resolved.

## Scope

### New files inside `crates/app/src/`

| File | Responsibility | Port source |
| --- | --- | --- |
| `workspace.rs` | Parse workspace `wo.toml`, walk and load each app | new (~200 LOC) |
| `manifest.rs` | Per-app `wo.toml` schema (`AppManifest`) | new (~150 LOC) |
| `resolver.rs` | Component / type / logic path resolution across `shared = […]` | new (~120 LOC) |
| `mod.rs` | Re-exports `Workspace`, `App`, `AppManifest`, `WorkspaceError`, `ResolverError` | new (~30 LOC) |

Total: ~500 LOC.

### Existing files filled in (not new code)

`docs/examples/ecommerce/` — gap-fill any directories named in 00-overview's "Target layout" that don't yet exist (e.g. `shared/components/header.htmlx`, `apps/storefront/types/cart.wo`). Pure structural work; no runtime change.

### `Cargo.toml` change

```toml
[dependencies]
toml = "0.8"
```

`toml` is the explicit cost of skipping a hand-rolled TOML parser at this phase. The dependency-removal track will revisit when relevant; for prototype, parsing TOML by hand isn't worth the LOC.

### Workspace `wo.toml` schema

```toml
name        = "ecommerce-workspace"
version     = "0.1.0"
kind        = "workspace"

[runtime]
wo = ">= 0.1"

[workspace]
apps   = ["apps/storefront", "apps/admin"]
shared = ["shared/types", "shared/logic", "shared/components"]

[database]
listen    = "127.0.0.1:5555"
data_dir  = "./data"
isolation = "snapshot"
```

### Per-app `wo.toml` schema

```toml
name        = "storefront"
version     = "0.1.0"
app_kind    = "app"

[dependencies]
shared = ["../../shared/types", "../../shared/logic", "../../shared/components"]

[server]
listen = ":8080"

[database]
url         = "wo://127.0.0.1:5555"
api_key_env = "STOREFRONT_DB_KEY"
```

## API shape (target)

```rust
use app::{Workspace, App};

let ws = Workspace::load(Path::new("docs/examples/ecommerce"))?;
assert_eq!(ws.apps().len(), 2);

let storefront = ws.app("storefront").unwrap();
let money_partial = storefront.resolve_component("money")?;        // shared/components/money.htmlx
let cart_type     = storefront.resolve_type("Cart")?;              // apps/storefront/types/cart.wo

// Degenerate form:
let blog = Workspace::load(Path::new("docs/examples/blog"))?;       // single-app workspace
assert_eq!(blog.apps().len(), 1);
```

## Exit criteria

1. `cargo build -p app` green; the new `toml` dep compiles.
2. **Workspace load.** `Workspace::load(docs/examples/ecommerce)` returns 2 apps + 3 shared dirs and no errors.
3. **Component resolution.** `storefront.resolve_component("money")` returns the path to `shared/components/money.htmlx`. `storefront.resolve_component("nonsense")` errors as `ResolverError::NotFound`.
4. **App-local override.** Adding `apps/storefront/ui/components/money.htmlx` makes `resolve_component("money")` return the app-local path; removing it falls back to the shared one.
5. **Degenerate form.** `Workspace::load(docs/examples/blog)` loads as a one-app workspace; `wo run docs/examples/blog` continues to start unchanged.
6. `cd reference/crates && cargo build && cargo test`.

## Non-scope

- **No semver, no registry, no lockfile.** Path references only.
- **No `wo dev` hot-reload.** File watching against `apps/*/ui/` is deferred (would consume `reference/crates/wo-watch/`).
- **No cross-workspace symlinks.** `shared = […]` paths must resolve under the workspace root.
- **No build-time enforcement that an app touches only its declared shared dirs.** That's an integrity check for a later hardening phase.
- **No env-var interpolation in `wo.toml`.** `${VAR}` syntax stays out; runtime config comes through env vars at startup, not manifest time.

## Verification

```bash
cargo build -p app
cargo test  -p app --test workspace_load
cargo test  -p app --test resolver

# end-to-end inspection
cargo run --bin wo -- ls-apps docs/examples/ecommerce
# storefront    apps/storefront
# admin         apps/admin

cargo run --bin wo -- ls-apps docs/examples/blog
# blog          .

# v1 regression
cargo run --bin wo -- run docs/examples/blog &
PID=$!; sleep 1; curl -fsS http://127.0.0.1:8080/ >/dev/null; kill $PID

cd reference/crates && cargo build && cargo test
```

## After this phase

The workspace layout is the input that phase 05 (`05-per-app-binaries.md`) compiles into a binary per app, and the namespace within which phase 06 (`06-shared-db-daemon.md`) issues per-app API keys. Phase 07 (`07-per-app-policies.md`) reads `apps/<X>/app.wo` for app-scope policy declarations whose location this phase locks.
