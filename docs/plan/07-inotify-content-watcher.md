# 07 — `inotify` Content Watcher

**Context sources:** [`./02-event-loop-epoll.md`](./02-event-loop-epoll.md), [`./linux/00-linux.md`](./linux/00-linux.md) § File Watching, [`../02-recovery.md`](../02-recovery.md) § No AWS Infrastructure.

## Goal

First Stage-3 capability. When a `.wo` source file under the active project directory changes, `inotify` fires on the phase-02 event loop and the runtime hot-reloads the affected schema — parser re-run, catalog refreshed, live routes updated in-place. Maps to [`00-linux.md`](./linux/00-linux.md)'s "watch the content directory for file creates, modifications, and deletes. Triggers re-indexing and subscriber notification when articles change. **Replaces the S3 + Lambda event pipeline entirely.**"

Also the first real second consumer of the phase-02 `EventLoop` beyond the HTTP listener — validates the abstraction under cross-feature load.

## Design decisions (locked)

1. **`inotify_init1(IN_CLOEXEC | IN_NONBLOCK)` + `inotify_add_watch`.** The fd is registered on the phase-02 event loop alongside the HTTP listener. No polling. No cross-platform fallback (`kqueue` on macOS, `ReadDirectoryChangesW` on Windows) — writeonce targets Linux only.
2. **Per-directory watches, not per-file.** `types/`, `ui/`, `logic/`, `tests/`, and `app.wo`'s parent get a watch each; individual files are resolved from the event's `wd` + `name` fields. Prevents fd exhaustion on large projects (the default `fs.inotify.max_user_watches` is 8192 on most distros, but we'd rather spend watches carefully).
3. **Debounce at 150 ms.** Editors issue multiple events per save (create tempfile → write → rename → delete old). A `TimerFd::oneshot(150ms)` per-watch absorbs the burst; only the final "settled" state triggers a recompile.
4. **Full recompile, not incremental.** A file change invalidates the full schema catalog — re-run `rt::discover()` → `rt::parser::parse()` → `rt::compile::Catalog::from_schemas()`. The sample projects are small (8 files for the blog); a full parse is < 50 ms. Incremental type-graph invalidation is a phase 09+ optimization.
5. **Atomic catalog swap.** The `Engine`'s catalog is behind an `Arc<ArcSwap<Catalog>>` or equivalent — a new catalog replaces the old under a single pointer write, and in-flight HTTP handlers finish with the old one while new ones see the new. Under single-threaded execution this is essentially free; under sharding it becomes the per-shard atomic.
6. **Module at `crates/rt/src/watch/`.** Same extraction-deferred rule as earlier modules.

## Scope

### New files inside `crates/rt/src/watch/`

| File | Responsibility | Port source |
| --- | --- | --- |
| `mod.rs` | Re-exports `Watcher`, `WatchEvent` | — |
| `inotify.rs` | Raw wrappers: `init()`, `add_watch(path, mask)`, `read_events() -> Vec<RawEvent>`. Registers on the `EventLoop`. | [`reference/crates/wo-watch/src/lib.rs`](../../reference/crates/wo-watch/src/lib.rs) (280 LOC) — v1 already does exactly this |
| `recursive.rs` | Walks the project root, calls `add_watch` for every directory matching `types/\|ui/\|logic/\|tests/` or containing `*.wo` | ~80 new LOC |
| `debounce.rs` | Coalesces bursts per-watch-descriptor, fires a `TimerFd` for the 150 ms settle window | ~100 new LOC |
| `reload.rs` | On debounced fire: re-discover, re-parse, re-compile, `ArcSwap::store(new_catalog)` | ~80 new LOC |

Total: ~540 LOC (280 ported + ~260 new).

### `Cargo.toml` change

None. `libc` already covers `inotify_init1` / `inotify_add_watch` / `inotify_rm_watch`.

### Routing change in `crates/rt/src/server.rs`

The router needs to re-resolve the catalog on each request rather than close over a snapshot at boot:

```rust
// before
let router = Router::new().route("/api/articles", list_h_bound_to_catalog_snapshot);

// after
let shared = Arc::new(ArcSwap::from_pointee(catalog));
let router = Router::new().route("/api/articles", move |req, st| {
    let cat = shared.load();
    list_h(req, &cat, st)
});
```

One-time rewrite of the 12 handlers (4 types × 3 ops). Mechanical.

## API shape (target)

```rust
use rt::event::EventLoop;
use rt::watch::Watcher;

let mut loop_ = EventLoop::new()?;
let mut watcher = Watcher::recursive(Path::new("docs/examples/blog"), Duration::from_millis(150))?;
watcher.register(&mut loop_)?;

for event in loop_.wait_once(None)? {
    if event.token() == watcher.token() {
        for change in watcher.drain() {
            eprintln!("[wo] content change: {} ({})", change.path.display(), change.kind);
            // reload pipeline fires here
        }
    }
}
```

## Exit criteria

1. **`cargo build`** green. No new deps.
2. **Unit test:** create a temp dir, write `a.wo`, spin up a `Watcher` on a loop in a test thread, modify `a.wo`, assert the debounced `WatchEvent::Modified(path)` arrives within 250 ms.
3. **End-to-end manual:**
   ```bash
   cargo run --bin wo -- run docs/examples/blog &
   # observe: `curl :8080/api/articles` returns [...]
   # edit docs/examples/blog/types/article.wo — add a `nickname: Text?` field
   # wait 200 ms
   # observe: `curl :8080/api/articles` response shape reflects new field (no restart)
   ```
4. **`[wo]` log lines** match the spec in [00-linux.md](./linux/00-linux.md) — one line per debounced change, showing the relative path and event kind.
5. **All 14 `rt` unit tests still pass.** `reference/rest/blog.rest` 20-assertion battery still green.
6. **No fd leak** — `ls -la /proc/$PID/fd` before and after ten consecutive edits shows the same count.

## Non-scope

- **No cross-platform fallback.** `kqueue` and `ReadDirectoryChangesW` are not on the roadmap. Linux only.
- **No `fanotify`.** [00-linux.md](./linux/00-linux.md) lists it as "useful if watching needs to span mount points" — writeonce projects live in one directory tree; `inotify` is enough.
- **No incremental reparse.** Full recompile per settled change. If a real project hits the full-recompile wall, phase 09+ can add a dependency-graph-aware rebuilder.
- **No subscription push.** Phase 07 only detects and reloads. Notifying connected clients (the `register! { #{blog-title} => notify(fd) }` model in [00-linux.md](./linux/00-linux.md)) is phase 09 once `sub` activates.

## Verification

```bash
cargo build
cargo test --lib watch
cargo test --lib                               # 14 existing + watcher tests green

# manual hot-reload check
cargo run --bin wo -- run docs/examples/blog &
PID=$!
sleep 2
curl -s http://127.0.0.1:8080/api/articles
echo '  policy read anyone' >> docs/examples/blog/types/article.wo
sleep 0.5
curl -s http://127.0.0.1:8080/api/articles     # server did not restart; catalog refreshed
kill $PID
git checkout docs/examples/blog/types/article.wo   # undo the edit

cd reference/crates && cargo build && cargo test   # v1 untouched
```

## After this phase

The runtime now does what `docs/02-recovery.md` originally promised: the binary watches its own content directory with `inotify` and re-indexes on change. The S3 + Lambda + sync-trigger pipeline is fully replaced by one fd on one event loop in one process.

Phase 08 adds the other half of the v1 kernel-primitive story — `sendfile` for zero-copy static serving.
