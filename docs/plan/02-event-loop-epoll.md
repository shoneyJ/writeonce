# 02 — Event Loop on `epoll`

**Context sources:** [`../01-problem.md`](../01-problem.md), [`../02-recovery.md`](../02-recovery.md), [`./linux/00-linux.md`](./linux/00-linux.md), [`./done/01-scafolding-crates.md`](./done/01-scafolding-crates.md).

## Goal

Land a hand-rolled single-threaded event loop inside `crates/rt/` that wraps Linux's file-descriptor primitives directly — **without** touching `tokio`, `axum`, or any other async runtime crate. This is the foundation every later phase builds on: phase 03 puts an HTTP server on top of it, phase 04 retires tokio + axum, phase 07 registers `inotify` watches on it, phase 08 drives `sendfile` through it.

Nothing is removed in this phase. The module sits alongside the tokio-backed axum server, unused by the `wo` binary until phase 04 flips the switch.

## Design decisions (locked)

1. **`epoll`, not `io_uring`, on day one.** `epoll` is ubiquitous (Linux 2.6+), well-understood, and every primitive we need (eventfd, timerfd, signalfd, inotify, accepted sockets) already integrates with it via `epoll_ctl`. `io_uring` is a natural follow-on phase once the event-loop abstraction exists — [`00-linux.md`](./linux/00-linux.md) calls it out for that role.
2. **Single-threaded, edge-triggered.** Matches [02-wo-language.md § Concurrency Model](../runtime/database/02-wo-language.md#concurrency-model). Every fd registered with `EPOLLET`; the loop reads until `EAGAIN`. No worker pool, no cross-thread state.
3. **`libc` is the only new dependency.** `libc = "0.2"` added to `crates/rt/Cargo.toml`. No `nix`, no `mio`. Direct `unsafe extern "C"` calls against the kernel surface.
4. **Module, not crate (yet).** Lives at `crates/rt/src/runtime/` so phase 03 can call into it cheaply. Extraction to the empty `crates/event/` sibling is deferred until a second caller appears outside `rt` — likely when [`sub`](../../crates/sub/) starts consuming the loop for subscription delivery.

## Scope

### New files inside `crates/rt/src/runtime/`

| File | Responsibility | Port source |
| --- | --- | --- |
| `mod.rs` | Re-exports `EventLoop`, `Event`, `Interest`, `Token`, `EventFd`, `TimerFd`, `SignalFd` | [`reference/crates/wo-event/src/lib.rs`](../../reference/crates/wo-event/src/lib.rs) (9 LOC) |
| `netpoll_epoll.rs` | `EventLoop { fd, events }` — `new()`, `register(raw_fd, interest, token)`, `wait_once(timeout) -> &[Event]`, `deregister(raw_fd)` | [`reference/crates/wo-event/src/epoll.rs`](../../reference/crates/wo-event/src/epoll.rs) (183 LOC); [`reference/go/src/runtime/netpoll_epoll.go`](../../reference/go/src/runtime/netpoll_epoll.go) for idiom |
| `eventfd.rs` | `EventFd { fd }` — counter semaphore for cross-fd wake-up (subscription dispatch, shutdown signal) | [`reference/crates/wo-event/src/eventfd.rs`](../../reference/crates/wo-event/src/eventfd.rs) (66 LOC) |
| `timerfd.rs` | `TimerFd { fd }` — oneshot + periodic timers as fds for the loop | [`reference/crates/wo-event/src/timerfd.rs`](../../reference/crates/wo-event/src/timerfd.rs) (91 LOC) |
| `signalfd.rs` | `SignalFd { fd }` — SIGINT / SIGTERM / SIGHUP delivered as fd reads for graceful shutdown without a tokio signal handler | [`reference/crates/wo-event/src/signalfd.rs`](../../reference/crates/wo-event/src/signalfd.rs) (62 LOC) |

Total: ~410 LOC lifted and adapted. The v1 code already compiles standalone in `reference/crates/wo-event/` and has unit tests; the port is near-verbatim plus namespace cleanups.

### Why `runtime/` not `event/`

Go's equivalent code lives at [`reference/go/src/runtime/netpoll_epoll.go`](../../reference/go/src/runtime/netpoll_epoll.go) alongside siblings like `netpoll_kqueue.go` (macOS/BSD), `netpoll_io_uring.go` (if/when Go adds it), and the shared `netpoll.go` interface. The directory name "runtime" signals that this is the layer beneath user code — scheduler / netpoll / syscall shims — and the filename prefix `netpoll_<flavour>` makes each implementation alternative visible at a glance. Adopting the same convention in writeonce makes porting ideas bidirectional: a reader who knows Go's layout can find the writeonce equivalent by trimming the `.go` extension and swapping it for `.rs`. When Phase 3's io_uring arrives it'll land as `netpoll_io_uring.rs` next to the epoll one; a cross-platform stub would be `netpoll.rs`. Module boundary and naming both match. See [`docs/plan/assembly/00-overview.md`](./assembly/00-overview.md) for why we stop short of mirroring Go's assembly conventions.

### `Cargo.toml` change

```toml
[dependencies]
anyhow     = "1"
serde      = { version = "1", features = ["derive"] }
serde_json = "1"
tokio      = { version = "1", features = ["rt", "macros", "net", "signal", "sync", "time"] }
axum       = "0.7"
tower      = "0.4"
libc       = "0.2"   # NEW — see docs/plan/02-event-loop-epoll.md
```

## API shape (target — validate against v1 when porting)

```rust
use rt::runtime::{EventLoop, EventFd, Interest, Token};

let mut loop_ = EventLoop::new()?;
let ev = EventFd::new()?;
loop_.register(ev.as_raw_fd(), Interest::READABLE, Token(0))?;
ev.write(1)?;                       // wake the loop from another flow
for event in loop_.wait_once(Some(Duration::from_millis(100)))? {
    match event.token() {
        Token(0) => { let n = ev.read()?; /* ... */ }
        _ => unreachable!(),
    }
}
```

## Exit criteria

1. `cargo build` at root compiles cleanly.
2. A new unit test in `crates/rt/src/runtime/netpoll_epoll.rs`:
   - create an `EventLoop`,
   - register an `EventFd`,
   - `write(1)` to the eventfd from the same thread,
   - `wait_once(timeout)` returns an `Event` for the correct token,
   - `read()` on the eventfd returns `1`.
3. A second unit test validates `TimerFd::oneshot(100ms)` fires within a `wait_once(500ms)` window.
4. All 14 existing `rt` tests still pass. `cargo run --bin wo -- run docs/examples/blog` still serves (tokio path unchanged).
5. `cd reference/crates && cargo build && cargo test` still green (nothing touched).

## Non-scope

- **No cutover.** The `wo` binary keeps calling `tokio::runtime::Builder::new_current_thread()`. That happens in phase 04.
- **No HTTP.** Accepting connections is phase 03's problem. This phase is pure kernel-primitive plumbing.
- **No subscription dispatch.** The `sub` crate doesn't exist yet as real code; phase 07 (inotify) is the first real loop consumer after phase 03.
- **No crate extraction.** Stays at `crates/rt/src/runtime/`. Pulling to `crates/event/` waits for a second consumer.
- **No `io_uring`.** Separate follow-on once the abstraction solidifies.

## Verification

```bash
cargo build
cargo test --lib runtime         # new tests in crates/rt/src/runtime/
cargo test --lib                 # all 14 existing + new epoll/eventfd/timerfd tests green
cargo run --bin wo -- run docs/examples/blog   # axum path unchanged, still serves
cd reference/crates && cargo build && cargo test   # v1 untouched
```

## After this phase

Phase 03 puts a non-blocking HTTP/1.1 listener on top of the `EventLoop` and proves end-to-end I/O without tokio. The two phases together give phase 04 everything it needs to delete the tokio + axum dependencies.
