# 06 — Shared database daemon (`wo db serve`)

**Context sources:** [`./00-overview.md`](./00-overview.md) §§ "Goal" (L23), "Design decisions" 3 (L32), "Non-scope" (L201–203), [`./03-client-runtime.md`](./03-client-runtime.md) (the wire frames this daemon emits), [`./05-per-app-binaries.md`](./05-per-app-binaries.md) (the apps that connect), [`reference/crates/wo-sub/src/lib.rs`](../../../reference/crates/wo-sub/src/lib.rs) (the v1 subscription registry, 470 LOC, that needs generalising past `ByTitle`/`ByTag`/`All`), [`../../runtime/database/04-client-api.md`](../../runtime/database/04-client-api.md) (the wire-protocol owner).

## Goal

Stand up a headless daemon — `wo db serve` — that runs the engine + WAL + subscription registry behind the Phase-4 native wire protocol on `127.0.0.1:5555`, with no HTTP and no template rendering. Each connection presents an API key from a static table, binds a `Principal { app, roles }` for downstream policy evaluation, and can register `Subscription::ByPredicate` against any type — a generalisation of v1's article-only subscription model that this phase ports and broadens.

## Design decisions (locked)

1. **Daemon = `crates/db` thin entrypoint + `crates/engine` + the wire acceptor.** No HTTP, no `.htmlx`, no `##ui`. The shared DB process knows nothing about the UI layer.
2. **API-key table is in-memory, env-seeded.** On startup the daemon reads `WO_DB_KEY_<APP>=<hex>` for each app declared in the workspace and builds an `AuthTable: HashMap<ApiKey, Principal>`. A `--keys <file>` flag is accepted but treated as a future hook.
3. **Generalise `wo-sub`** from `Subscription::ByTitle/ByTag/All` to `Subscription::ByPredicate(TypeRef, Expr, SortKey)`. The v1 variants stay as legacy aliases (`ByTitle(t)` ⇒ `ByPredicate(Article, sys_title == t, _)`) for the blog regression test. Anchored in [`reference/crates/wo-sub/src/lib.rs`](../../../reference/crates/wo-sub/src/lib.rs) L8–17.
4. **Connection scope = `Principal { app, roles }` stored on the connection.** Every query evaluator reads it; phase 07 wires it into policy AND-composition.
5. **One data dir, one engine, many connections.** Snapshot isolation by default (per `[database].isolation = "snapshot"` in the workspace `wo.toml`).
6. **Foreground-only this phase.** No daemonisation, no PID file, no signal handling beyond `SIGTERM` graceful shutdown. A future ops doc can add `wo db daemonize`.

## Scope

### New files

| File | Responsibility | Port source |
| --- | --- | --- |
| `crates/db/src/main.rs` | Entrypoint, arg parsing, env-key loading | new (~100 LOC) |
| `crates/db/src/server.rs` | Wire-protocol acceptor (TCP listener + per-conn handler) | new (~250 LOC) |
| `crates/db/src/auth.rs` | `AuthTable`, `Principal`, key handshake | new (~120 LOC) |
| `crates/sub/src/lib.rs` | Generalised subscription manager | port [`reference/crates/wo-sub/src/lib.rs`](../../../reference/crates/wo-sub/src/lib.rs) (470 LOC) + ~150 new |
| `crates/sub/src/predicate.rs` | Predicate evaluation against a row (uses `crates/ql` if available, else minimal subset) | new (~150 LOC) |

Total: ~1240 LOC (470 ported + ~770 new).

### `Cargo.toml` change

`crates/db` becomes a binary target:

```toml
[[bin]]
name = "wo-db"
path = "src/main.rs"
```

Plus deps already in the workspace: `serde`, `serde_json`, optionally `libc` for the listener (matching `crates/rt`'s direction).

### Wire handshake (added to Phase 4 protocol)

```
client → server:  HELLO  app="storefront"  api_key="<hex>"
server → client:  WELCOME principal={ app, roles }   |   ERROR "unauthorised"
```

After `WELCOME`, frames follow the Phase-4 native protocol. Subscription-registration frames carry `ByPredicate(TypeRef, Expr, SortKey)`.

## API shape (target)

```rust
use db::{DbServer, AuthTable, Principal};
use sub::{SubscriptionManager, Subscription};

let mut auth = AuthTable::new();
auth.insert(ApiKey::from_env("WO_DB_KEY_STOREFRONT")?,
            Principal { app: "storefront".into(), roles: roles!("Customer") });
auth.insert(ApiKey::from_env("WO_DB_KEY_ADMIN")?,
            Principal { app: "admin".into(), roles: roles!("Admin", "Ops") });

let server = DbServer::bind("127.0.0.1:5555", Path::new("./data"), auth)?;
server.run()?;          // foreground; SIGTERM exits cleanly

// inside a connection handler:
let sub = Subscription::ByPredicate(
    TypeRef::new("Order"),
    parse_expr("status != Cancelled")?,
    SortKey::new("placed_at", SortDir::Desc),
);
let id = subs.register(conn_fd, sub)?;
```

## Exit criteria

1. `cargo build -p db -p sub` green; `wo-db` binary produced under `target/release/`.
2. **Daemon starts.** `WO_DB_KEY_STOREFRONT=aaaa WO_DB_KEY_ADMIN=bbbb cargo run --bin wo-db -- --listen 127.0.0.1:5555 --data-dir /tmp/wo-test` runs foreground and accepts `SIGTERM`.
3. **Two principals.** Two clients connect, one with each API key; each receives a distinct `Principal` in the `WELCOME` frame.
4. **Predicate subscription.** Client registers `Subscription::ByPredicate(Order, "status != Cancelled", "placed_at desc")`; the manager returns a fresh `subscription_id`; on a stub `Order` insert, the matching client receives an `insert` frame.
5. **v1 regression.** A connection running the legacy `Subscription::ByTitle("hello-world")` against the blog corpus still produces notifications via the legacy alias.
6. `cd reference/crates && cargo build && cargo test`.

## Non-scope

- **TLS.** `wo://` is plaintext this phase. TLS is its own phase later.
- **API-key rotation, revocation, expiry.** Static map only. JWT, mTLS, etc. — out of scope.
- **Multi-data-dir, replication, sharding.** One process, one data dir.
- **WAL changes.** Engine + WAL semantics inherit from the Stage-2 in-memory engine; persistent storage and crash recovery belong to the database series, not this phase.
- **Daemonisation, PID file, systemd integration.** Foreground-only.
- **Metric / structured-log emission.** Plain `eprintln!` traces only.

### Risk to flag in the doc

If `crates/ql` is too thin to evaluate `status != Cancelled` end-to-end at the time this phase lands, lock a minimal predicate subset — `==`, `!=`, `>`, `<`, `&&`, `||` against scalar fields — and document the gap explicitly. Phases that need richer predicates (graph traversals, computed fields) wait for `ql` to mature.

## Verification

```bash
cargo build -p db -p sub

# foreground daemon + two connections
WO_DB_KEY_STOREFRONT=aaaa WO_DB_KEY_ADMIN=bbbb \
  cargo run --bin wo-db -- --listen 127.0.0.1:5555 --data-dir /tmp/wo-test &
DB_PID=$!
cargo test -p db --test multi_app_principals
cargo test -p sub --test predicate_subscription
kill $DB_PID

# legacy v1 path
cargo test -p sub --test legacy_by_title

cd reference/crates && cargo build && cargo test
```

## After this phase

The wire URL contract is now stable, which unblocks phase 05 (per-app binaries) connecting to `wo://127.0.0.1:5555`. Phase 07 (`07-per-app-policies.md`) hooks its `EffectivePolicy` resolver into the query path inside this daemon — every query the daemon executes carries the connection's `Principal`, which is exactly what 07's AND-composition needs.
