# 03 — Client runtime

**Context sources:** [`./00-overview.md`](./00-overview.md) §§ "`.htmlx` with live subscriptions — target format" (L127–166) and decisions 1–2, [`./01-htmlx-format-spec.md`](./01-htmlx-format-spec.md) (the manifest schema this runtime consumes), [`reference/crates/wo-sub/src/lib.rs`](../../../reference/crates/wo-sub/src/lib.rs) (the v1 frame model the wire format mirrors), [`docs/examples/ecommerce/shared/components/order-row.htmlx`](../../examples/ecommerce/shared/components/order-row.htmlx) (the live workload the runtime must update without reload).

## Goal

Ship a ~500-line vanilla-JS client at `crates/ui/assets/wo-runtime.js` that, on page load, reads the `<script data-wo-manifest>` JSON, opens one WebSocket back to the originating app, subscribes to every declared `LiveSubscription`, and patches DOM in place on `Insert`/`Update`/`Delete` frames — without a framework, without a build step, served as a static asset at `/_wo/runtime.js`.

## Design decisions (locked)

1. **Vanilla JS, no transpiler.** The file shipped is the file written. Anchored in [`./00-overview.md`](./00-overview.md) L25, L198–199.
2. **JSON over WebSocket.** Frame schema mirrors `reference/crates/wo-sub` semantics evolved into this phase's predicate-subscription model. `{ subscription_id, kind: "snapshot"|"insert"|"update"|"delete", key, row|fields }`.
3. **Targeted DOM patching, not virtual-DOM.** `update` ⇒ `document.querySelectorAll('[data-wo-subscription="<id>"] [data-key="<k>"] [wo\\:bind="<f>"]')` ⇒ `el.textContent = row[f]`. Matches the Zone-less Angular note in 00-overview decision 9.
4. **Reconnect = full snapshot resync.** On reconnect the runtime re-subscribes and replaces each `<wo:live>` body with the fresh snapshot. No diff, no replay buffer.
5. **Backpressure = drop all but latest update per `data-key`.** A coalescing queue keyed by `(subscription_id, key)` collapses queued `update` frames; the latest wins. New frames of other kinds (`insert`/`delete`) flush the queue.
6. **Asset baked into binary, served at `/_wo/runtime.js`.** `crates/ui/src/runtime/asset.rs` does `pub const RUNTIME_JS: &[u8] = include_bytes!("../../assets/wo-runtime.js");`. Phase 05 mounts the route in the per-app binary.

## Scope

### New files

| File | Responsibility | Port source |
| --- | --- | --- |
| `crates/ui/assets/wo-runtime.js` | The runtime — reads manifest, opens WS, dispatches frames, patches DOM | new (~500 LOC JS) |
| `crates/ui/src/runtime/mod.rs` | Re-exports `RUNTIME_JS`, `runtime_etag()`, `Frame` | new (~20 LOC) |
| `crates/ui/src/runtime/asset.rs` | `include_bytes!` of the JS + sha256 ETag | new (~30 LOC) |
| `crates/ui/src/runtime/frame.rs` | Wire-frame `enum Frame` mirroring the JS schema | new (~120 LOC) |

Total: ~170 LOC Rust + ~500 LOC JS.

### `Cargo.toml` change

```toml
[dependencies]
serde      = { version = "1", features = ["derive"] }
serde_json = "1"
sha2       = "0.10"   # ETag for the runtime asset
```

`serde` / `serde_json` were already pulled in by phase 01. `sha2` is new for the asset ETag.

### Wire frame schema (mirrored in JS and Rust)

```json
{ "kind": "snapshot", "subscription_id": "orders-live-0", "rows": [ {…}, … ] }
{ "kind": "insert",   "subscription_id": "orders-live-0", "key": "42", "row": {…} }
{ "kind": "update",   "subscription_id": "orders-live-0", "key": "42", "fields": { "status": "Paid" } }
{ "kind": "delete",   "subscription_id": "orders-live-0", "key": "42" }
```

## API shape (target)

```rust
use ui::runtime::{RUNTIME_JS, runtime_etag, Frame};

// Phase-05 router mounts:
router.route(Method::GET, "/_wo/runtime.js", |_req| {
    Response::ok()
        .header("Content-Type",  "application/javascript")
        .header("ETag",          runtime_etag())
        .body(RUNTIME_JS.to_vec())
});

// Phase-06 wire-protocol handler emits frames:
let frame = Frame::Update { subscription_id: id.into(), key: k.into(), fields: patch };
ws.send_text(serde_json::to_string(&frame)?)?;
```

## Exit criteria

1. `cargo build -p ui` green; `wc -c crates/ui/assets/wo-runtime.js` ≤ 25 600 bytes (25 KB cap, slack on the 20 KB target).
2. **Frame round-trip test.** A Rust unit test serialises one of each `Frame` variant; a Node-driven test (`node --test`) parses the same JSON, asserts shape.
3. **DOM-patch test (jsdom).** `node crates/ui/runtime-tests/run.mjs` loads a stub HTML containing one `<wo:live>` block and a manifest, fakes a WebSocket emitting `snapshot` → `insert` → `update` → `delete` frames, and asserts each patch hits the right element.
4. **Reconnect test.** Killing the fake WS triggers exponential backoff; on resume the runtime re-issues subscriptions and replaces the body with the new snapshot.
5. **Asset served.** Once phase 05 lands, `curl http://127.0.0.1:8080/_wo/runtime.js` returns the file with a stable `ETag` matching `sha256(RUNTIME_JS)`.
6. `cd reference/crates && cargo build && cargo test`.

## Non-scope

- **No browser test matrix.** jsdom is the only target; Playwright/headless-Chrome are deferred.
- **No optimistic UI.** Action buttons (`data-action="…"`) POST and wait — no client-side state mutation before the server confirms.
- **No client-side routing.** Page navigation is full-page reload.
- **No framework integration.** No React/Vue/Solid bindings.
- **No gzip / Brotli.** The JS ships uncompressed; HTTP-level compression is a phase-08-style concern.
- **No `<noscript>` fallback.** Pages with `<wo:live>` without JS show the SSR snapshot frozen.

## Verification

```bash
cargo build -p ui
cargo test  -p ui --test wire_frames

# JS unit test (jsdom) — repo will need node ≥ 20
node crates/ui/runtime-tests/run.mjs

# Size budget
wc -c crates/ui/assets/wo-runtime.js
test "$(wc -c < crates/ui/assets/wo-runtime.js)" -le 25600

# v1 regression
cargo run --bin wo -- run docs/examples/blog &
PID=$!; sleep 1; curl -fsS http://127.0.0.1:8080/ >/dev/null; kill $PID

cd reference/crates && cargo build && cargo test
```

## After this phase

Phases 01 + 02 + 03 together cover the prototype demo path: a `##ui` block compiles to `.htmlx`, the SSR pass renders it with a manifest, the runtime opens a WebSocket and patches the DOM on commit. Phase 05 (`05-per-app-binaries.md`) bakes `RUNTIME_JS` into each app binary; phase 06 (`06-shared-db-daemon.md`) is the WebSocket origin that emits the frames defined here.
