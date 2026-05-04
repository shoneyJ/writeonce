# 07 — Per-app policy composition

**Context sources:** [`./00-overview.md`](./00-overview.md) decision 7 (L36) and "Goal" (L26), [`./04-workspace-layout.md`](./04-workspace-layout.md) (where app-scope policies live), [`./06-shared-db-daemon.md`](./06-shared-db-daemon.md) (where composition is evaluated), [`docs/examples/blog/types/article.wo`](../../examples/blog/types/article.wo) and the other type files (existing global `policy read/write` blocks), [`docs/examples/ecommerce/apps/admin/ui/orders/orders.wo`](../../examples/ecommerce/apps/admin/ui/orders/orders.wo) L18 (a `role: Admin | Ops` set expression).

## Goal

Layer per-app policy scopes (declared in `apps/<X>/app.wo` and per-`##ui` `role:` clauses) on top of the global per-type policies that already live next to type declarations, enforcing `effective = global AND app_scope`. An app can narrow what its connection sees, never broaden. A static check at `wo build` time refuses any app-scope that names a role outside the type's own policy domain.

## Design decisions (locked)

1. **AND-only composition.** App-scope can narrow, never broaden. Anchored in [`./00-overview.md`](./00-overview.md) decision 7 (L36).
2. **Three carrier locations.** (a) Global `policy read/write` blocks beside `type` declarations in `shared/types/<type>.wo` — already in the language. (b) `policy` blocks inside `apps/<X>/app.wo` — new structured form parsed in this phase. (c) `role: <RoleSet>` clauses inside `##ui` blocks and `actions:` rows — already in samples (e.g. [`docs/examples/ecommerce/apps/admin/ui/orders/orders.wo`](../../examples/ecommerce/apps/admin/ui/orders/orders.wo) L18, L50–53). All three feed the same `EffectivePolicy` composer.
3. **Evaluation point = wire-protocol query handler in phase 06.** When a connection issues a query, the daemon reads `connection.principal`, fetches global + app-scope policies for the touched types, AND-composes, applies. No client-side enforcement; no compile-time inlining.
4. **`role:` is a set expression, not a single string.** `Admin | Ops` parses to `RoleSet::union(Admin, Ops)`. Anchored in [`docs/examples/ecommerce/apps/admin/ui/orders/orders.wo`](../../examples/ecommerce/apps/admin/ui/orders/orders.wo) L18.
5. **Build-time domain check.** `wo build apps/<X>` walks every `role: <set>` referenced by the app and verifies each role is one the global type policy actually defines for that type. An app cannot mention `role: Anonymous` against a type whose global policy never permits anonymous access — that's a build error, not a runtime one.

## Scope

### New files

| File | Responsibility | Port source |
| --- | --- | --- |
| `crates/policy/src/scope.rs` | `AppScope`, `EffectivePolicy`, AND composer | new (~150 LOC) |
| `crates/policy/src/parse.rs` | Parse `role: A \| B` set expressions and `policy` blocks in `app.wo` | new (~100 LOC) |
| `crates/policy/src/check.rs` | Static build-time domain check | new (~100 LOC) |
| `crates/policy/src/mod.rs` | Re-exports | new (~30 LOC) |

### Modified files

| File | Change |
| --- | --- |
| `crates/db/src/server.rs` | Plug `EffectivePolicy::compose(global, app_scope, type)` into the per-query path; pass `connection.principal` through. (+50 LOC) |
| `crates/app/src/build.rs` | Run `policy::check::domain_check(app)` before invoking cargo. (+30 LOC) |

Total: ~460 LOC (all new — no v1 precedent for app-scope policy composition).

### `Cargo.toml` change

None beyond what phases 01–06 already added.

## API shape (target)

```rust
use policy::{GlobalPolicy, AppScope, EffectivePolicy, RoleSet};

// Composition (called per query in the daemon)
let effective = EffectivePolicy::compose(&global_for(&order_type),
                                          &app_scope_for("admin"),
                                          &order_type);
let allowed_rows = effective.filter(rows, &principal);

// Build-time domain check (called by `wo build apps/X`)
policy::check::domain_check(&app)?;     // errors if any role: in app references undefined role

// Role set parser (used by both compiler and daemon)
let rs: RoleSet = "Admin | Ops".parse()?;
assert!(rs.contains(Role::Admin));
assert!(rs.contains(Role::Ops));
```

## Exit criteria

1. `cargo build -p policy -p db -p app` green.
2. **AND composition.** Unit test `compose(global = "published == true", app_scope = "owner == $session.id", t = Article)` returns an `EffectivePolicy` whose `allows_read` is true only when *both* clauses hold.
3. **Build-time domain check fires.** A test workspace where `apps/storefront/app.wo` declares `role: Anonymous` against a type whose global policy does not define `Anonymous` — `wo build apps/storefront` exits non-zero with `PolicyDomainError`.
4. **Cross-app integration.** Two storefront customers issue the same `GET /api/orders` against the daemon; each sees only their own rows (storefront app-scope narrows global). Admin sees both. Test runs against the phase-06 daemon.
5. **v1 regression.** `wo run docs/examples/blog` boots; the global `policy read for anyone when published == true` on the blog `Article` type continues to gate anonymous reads as it does today.
6. `cd reference/crates && cargo build && cargo test`.

## Non-scope

- **Row-level write policies.** Read only this phase. Phase-6 spec mentions write composition; defer.
- **Field-level (column-level) policies.** All-or-nothing per row.
- **Audit logging of policy decisions.** No structured emission in this phase.
- **Policy versioning, migration, schema changes.** Whatever the type currently declares is the one definition.
- **Dynamic role assignment.** Roles are static per session; promotion / impersonation flows are out.

## Verification

```bash
cargo build -p policy -p db -p app
cargo test  -p policy --test compose
cargo test  -p policy --test domain_check
cargo test  -p policy --test role_set_parse

# integration: two customers + one admin against the daemon
WO_DB_KEY_STOREFRONT=aaaa WO_DB_KEY_ADMIN=bbbb \
  cargo run --bin wo-db -- --listen 127.0.0.1:5555 --data-dir /tmp/wo-pol &
DB_PID=$!
cargo test --test cross_app_policy
kill $DB_PID

# v1 regression — blog policy unchanged
cargo run --bin wo -- run docs/examples/blog &
PID=$!; sleep 1
curl -fsS http://127.0.0.1:8080/api/articles                  # only published rows
test -z "$(curl -fsS http://127.0.0.1:8080/api/articles | grep '"published":false')"
kill $PID

cd reference/crates && cargo build && cargo test
```

## After this phase

The seven-doc UI track is complete. With phases 01–07 implemented end-to-end, the prototype demo path closes: `wo db serve` runs the shared daemon; `wo build apps/storefront` and `wo build apps/admin` produce two binaries with disjoint route tables and separate API keys; a checkout on the storefront fires a commit that the daemon broadcasts to admin's open `<wo:live>` subscription, the admin client runtime patches the orders table in place, and the same row never crosses storefront's narrower policy back to a different customer's session. After this phase, future hardening — TLS, key rotation, write-side composition, hot reload — gets its own track.
