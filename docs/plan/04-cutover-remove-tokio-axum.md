# 04 — Cutover: Remove tokio, axum, tower

**Context sources:** [`./02-event-loop-epoll.md`](./02-event-loop-epoll.md), [`./03-hand-rolled-http.md`](./03-hand-rolled-http.md), [`../01-problem.md`](../01-problem.md).

## Goal

Flip the `wo` binary off the tokio + axum stack and onto the phase-02 event loop + phase-03 HTTP server. Delete three dependencies from `crates/rt/Cargo.toml`. REST behaviour visible to [`reference/rest/blog.rest`](../../reference/rest/blog.rest) does not change — same status codes, same response bodies, same endpoint paths.

This is the first phase where the dependency count goes *down*. Phases 02 and 03 were additive; this one is the switch.

## Design decisions (locked)

1. **Atomic swap, single commit.** Don't run tokio and the new loop in parallel in production. Flip the binary's `main()` in one change. Phase 03 already gave us confidence the new stack works end-to-end via `http-smoke`.
2. **Preserve the `Engine` trait surface.** `Arc<Mutex<Engine>>` stays exactly as `crates/rt/src/engine.rs` has it today. The routing layer in `crates/rt/src/server.rs` — the function that maps `service rest` blocks to axum `MethodRouter` — gets rewritten to emit phase-03 `Router::route(...)` calls instead. Same data flow, different transport.
3. **No tokio — no async.** Handlers become synchronous `fn(&Request, &Engine) -> Response`. The single-threaded event loop [already assumes this](../runtime/database/02-wo-language.md#concurrency-model); removing `async fn` plumbing simplifies the code. `tokio::sync::Mutex` becomes `std::sync::Mutex` (fine in a single-threaded loop since lock contention is impossible).
4. **`signalfd` replaces `tokio::signal::ctrl_c()`.** Registered as another fd on the loop; reading a SIGINT cleanly exits the loop and closes outstanding connections.
5. **`WO_LISTEN` env var semantics unchanged.** The `127.0.0.1:8080` default + the `WO_LISTEN=...` override stays exactly as today. Operators don't notice the change.

## Scope

### Files rewritten inside `crates/rt/`

| File | Change | Notes |
| --- | --- | --- |
| `src/bin/wo.rs` | Replace `tokio::runtime::Builder::new_current_thread` + `axum::serve` with `EventLoop` + `Listener` + `Router` wiring | The `run()` / `serve()` fns fuse into a single synchronous `run()` that drives the loop |
| `src/server.rs` | Replace `axum::Router` construction + axum handlers (`async fn list_h(State(st): State<TypeState>) -> impl IntoResponse`) with phase-03 `Router::route(...)` + sync handlers | Handler bodies are otherwise untouched: `engine.lock().list(&ty).map(Json)` logic flows through |
| `src/engine.rs` | `tokio::sync::Mutex` → `std::sync::Mutex`; `.lock().await` → `.lock().unwrap()` | Only the wrapper changes; row logic intact |

### `Cargo.toml` delta

```diff
 [dependencies]
 anyhow     = "1"
 serde      = { version = "1", features = ["derive"] }
 serde_json = "1"
-tokio      = { version = "1", features = ["rt", "macros", "net", "signal", "sync", "time"] }
-axum       = "0.7"
-tower      = "0.4"
 libc       = "0.2"
```

Three deps gone. Four remaining: `anyhow`, `serde`, `serde_json`, `libc`.

### Files deleted

- None. The phase-02 `event/` module and phase-03 `http/` module stay in place and now become the primary code path.

## Handler signature change

**Before (axum + tokio):**

```rust
async fn list_h(State(st): State<TypeState>) -> impl IntoResponse {
    let eng = st.engine.lock().await;
    match eng.list(&st.ty) {
        Ok(rows) => (StatusCode::OK, Json(json!(rows))).into_response(),
        Err(e)   => (StatusCode::INTERNAL_SERVER_ERROR, e.to_string()).into_response(),
    }
}
```

**After (sync, event loop):**

```rust
fn list_h(_req: &Request, st: &TypeState) -> Response {
    let eng = st.engine.lock().unwrap();
    match eng.list(&st.ty) {
        Ok(rows) => Response::ok().json(&serde_json::json!(rows)),
        Err(e)   => Response::status(Status::INTERNAL_SERVER_ERROR).body(e.to_string()),
    }
}
```

Twelve handlers total — one pair per `{list, get, create, update, delete}` × four types. Mechanical rewrite.

## Exit criteria

1. **`cargo build`** at root — compiles with four deps (not seven).
2. **`cargo test --lib`** — all 14 existing `rt` unit tests still pass. A new test in `src/server.rs` exercises the router build from a compiled catalog (no HTTP, just static registration).
3. **End-to-end REST smoke — the 20-assertion battery from [`reference/rest/blog.rest`](../../reference/rest/blog.rest)** must pass byte-identical to Stage 2 today. Script:
   ```bash
   WO_LISTEN=127.0.0.1:8765 cargo run --bin wo -- run docs/examples/blog &
   # ... curl each block, check expected status
   ```
4. **Graceful shutdown.** SIGINT on the process exits cleanly (no panic, no orphan fds). Validate with `strace -f -e signalfd4,close` on shutdown.
5. **`cd reference/crates && cargo build && cargo test`** still green.
6. **Dep audit.** `cargo tree -p rt --depth 1` shows `libc` as the only non-transitive external dep beyond `anyhow`, `serde`, `serde_json`.

## Non-scope

- **No JSON replacement.** `serde` + `serde_json` are still imported and used. Phase 05 removes them.
- **No inotify / sendfile.** Stage 3 capabilities. Phases 07 and 08.
- **No `io_uring`.** The `EventLoop` keeps using `epoll` here; swapping is a later phase.
- **No crate extraction.** `event/` and `http/` stay inside `crates/rt/src/`. The empty `crates/event/` and `crates/http/` sibling crates wait for second consumers.

## Risk

The `.rest` files are the safety net — 20 assertions that every Stage 2 endpoint returns the expected status. If one breaks after the cutover, the fix is almost always in the handler rewrite (sync semantics + the new `Response::json(...)` helper). No transport-layer regression should survive phase 03's `http-smoke` passing.

## Verification

```bash
cargo build                                         # 4 deps, no tokio/axum/tower
cargo test --lib                                    # 14 + any new server.rs tests green

# end-to-end
WO_LISTEN=127.0.0.1:8765 cargo run --bin wo -- run docs/examples/blog &
PID=$!
sleep 2
# every block in reference/rest/blog.rest, via curl, checking %{http_code}
# (copy-paste the 20-assertion script from the Stage 2 turn that verified blog.rest)
kill $PID

cd reference/crates && cargo build && cargo test    # v1 untouched
```

## After this phase

Phase 05 removes `serde` + `serde_json` by writing a minimal JSON parser + emitter against the new `http::Response::json()` surface. At the end of phase 06, `crates/rt/Cargo.toml` is down to `libc` alone — the stated end goal.
