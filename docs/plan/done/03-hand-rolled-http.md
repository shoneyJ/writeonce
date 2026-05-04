# 03 — Hand-Rolled HTTP/1.1

**Context sources:** [`./02-event-loop-epoll.md`](./02-event-loop-epoll.md), [`./linux/00-linux.md`](./linux/00-linux.md), [`../02-recovery.md`](../02-recovery.md).

## Goal

A non-blocking HTTP/1.1 server module that accepts connections, parses requests, and writes responses, **driven by the phase-02 `EventLoop`** — no axum, no hyper, no tokio. Still additive: the existing axum router keeps serving `wo run` until phase 04 cuts over.

## Design decisions (locked)

1. **HTTP/1.1 only, keep-alive supported.** HTTP/2 and HTTP/3 are not on the roadmap for Stage 2 — they need ALPN / TLS support we don't have a plan for yet. HTTP/1.1 covers every endpoint the blog + ecommerce samples exercise.
2. **Per-connection state machine.** Each accepted socket fd is registered on the event loop with its own `Connection { state: Reading | Writing | Idle, parser, pending_response }`. Edge-triggered `EPOLLIN`/`EPOLLOUT` drive state transitions. Matches [v1 wo-http](../../reference/crates/wo-http/src/connection.rs)'s model verbatim.
3. **Router is pattern-matched at registration.** `Router::new().route("/api/articles/:id", Method::GET, handler)` resolves to a trie at boot. Per-request dispatch is a single trie walk — no axum-style type-erased layers.
4. **Handlers are `fn(&Request, &Engine) -> Response`.** Synchronous. The single-threaded event loop means a handler blocking is a bug; each handler must be a pure transformation over engine state.
5. **Module, not crate (yet).** Lives at `crates/rt/src/http/` with the same "extract when a second consumer shows up" rule as phase 02. The eventual home is the empty [`crates/http/`](../../crates/http/) sibling — but not in this phase. Paired with [phase 02's `crates/rt/src/runtime/`](./02-event-loop-epoll.md) (Go-style naming — `netpoll_epoll.rs`, `eventfd.rs`, …) which this module depends on for the `EventLoop` + raw syscall shims. Go's `src/net/http/` and `src/runtime/` split is the layout precedent; see [`reference/go/src/net/http/`](../../reference/go/src/net/http/).

## Scope

### New files inside `crates/rt/src/http/`

| File | Responsibility | Port source |
| --- | --- | --- |
| `mod.rs` | Re-exports `Listener`, `Connection`, `Request`, `Response`, `Router`, `Method`, `Status` | [`reference/crates/wo-http/src/lib.rs`](../../reference/crates/wo-http/src/lib.rs) (4 LOC) |
| `listener.rs` | `Listener { fd }` wrapping `socket + bind + listen + accept4(SOCK_NONBLOCK \| SOCK_CLOEXEC)`; integrates with `EventLoop` | [`reference/crates/wo-http/src/listener.rs`](../../reference/crates/wo-http/src/listener.rs) (202 LOC) |
| `connection.rs` | Per-fd state machine: drain request bytes, parse, dispatch, drain response bytes, keep-alive or close | [`reference/crates/wo-http/src/connection.rs`](../../reference/crates/wo-http/src/connection.rs) (202 LOC) |
| `request.rs` | Incremental HTTP/1.1 request parser: request line, headers, optional body. `Content-Length` only (no chunked request bodies in Stage 2 — they don't appear in the samples) | [`reference/crates/wo-http/src/request.rs`](../../reference/crates/wo-http/src/request.rs) (158 LOC) |
| `response.rs` | Response builder + writer: status line, headers, body (fixed or chunked) | [`reference/crates/wo-http/src/response.rs`](../../reference/crates/wo-http/src/response.rs) (110 LOC) |
| `route.rs` | Trie-based router: static paths + `:param` segments. `Router::route(method, path, handler) -> Router` | [`reference/crates/wo-route/src/router.rs`](../../reference/crates/wo-route/src/router.rs) (127 LOC) + [`pattern.rs`](../../reference/crates/wo-route/src/pattern.rs) (146 LOC) |

Total: ~949 LOC ported. Most of it is mechanical adaptation from v1; the namespace + the `Interest` enum change from phase 02 are the only non-trivial edits.

### `Cargo.toml` change

None. `libc` already in from phase 02 covers the raw syscalls.

## API shape (target)

```rust
use rt::event::EventLoop;
use rt::http::{Listener, Router, Method, Status, Response};

let mut loop_ = EventLoop::new()?;
let router   = Router::new()
    .route(Method::GET, "/healthz",          |_req, _eng| Response::ok().body("ok"))
    .route(Method::GET, "/api/articles",     list_articles)
    .route(Method::GET, "/api/articles/:id", get_article)
    .route(Method::POST,"/api/articles",     create_article);

let listener = Listener::bind("127.0.0.1:8080")?;
loop_.register(listener.as_raw_fd(), Interest::READABLE, Token::LISTENER)?;

let mut conns: HashMap<RawFd, Connection> = HashMap::new();
loop {
    for ev in loop_.wait_once(None)? {
        match ev.token() {
            Token::LISTENER => {
                while let Some(stream) = listener.accept_nonblocking()? {
                    let fd = stream.as_raw_fd();
                    loop_.register(fd, Interest::READABLE, Token::CONN(fd))?;
                    conns.insert(fd, Connection::new(stream));
                }
            }
            Token::CONN(fd) => {
                conns.get_mut(&fd).unwrap().drive(ev, &router, &engine)?;
                if conns[&fd].is_closed() { conns.remove(&fd); }
            }
            _ => {}
        }
    }
}
```

## Exit criteria

1. A test binary `crates/rt/src/bin/http-smoke.rs` (`[[bin]] name = "http-smoke"` in `rt/Cargo.toml`) binds on `127.0.0.1:0` (auto-assigned port), registers routes for `/healthz`, `/echo/:name`, `/counter`, and services them via the phase-02 event loop.
2. An integration test (also in `crates/rt/tests/http_smoke.rs` or similar) spawns the binary, sends three `curl` equivalents using `std::net::TcpStream`, validates status codes and bodies.
3. All 14 existing `rt` tests still pass.
4. `wo run docs/examples/blog` unchanged — axum path still drives the real CLI.
5. `cargo build` at root; `cd reference/crates && cargo build` still green.

## Non-scope

- **No TLS.** Deferred. When it lands, it's a wrapper around `Connection` that `read`/`write`s through `rustls` or (ideally) kTLS. Not this phase.
- **No HTTP/2.** See design decision 1.
- **No chunked request bodies.** Every sample's `POST /api/X` uses `Content-Length`. If a future sample needs chunked, it's a small extension to `request.rs`.
- **No middleware.** axum's `tower::Layer` idiom has no direct analog. Cross-cutting concerns (logging, auth) live in the handler or in a wrapper fn — phase 04 re-integrates with the existing axum state handling when the cutover happens.
- **Not wired into the `wo` binary yet.** That's phase 04.

## Verification

```bash
cargo build                               # root workspace compiles
cargo test --bin http-smoke               # the bundled test binary
cargo test --lib                          # 14 existing rt tests still green
cargo run --bin wo -- run docs/examples/blog   # axum path unchanged
```

## After this phase

Phase 04 takes the same in-memory `Engine` that the axum router serves and points the phase-03 router at it instead. Removing `tokio`, `axum`, `tower` is a consequence; the behaviour visible to `reference/rest/blog.rest` does not change.
