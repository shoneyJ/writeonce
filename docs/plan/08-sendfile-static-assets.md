# 08 — `sendfile` Zero-Copy Static Serving

**Context sources:** [`./03-hand-rolled-http.md`](./03-hand-rolled-http.md), [`./linux/00-linux.md`](./linux/00-linux.md) § Efficient File Serving, [`../02-recovery.md`](../02-recovery.md).

## Goal

Serve static file bytes — eventually `##ui`-emitted HTML + CSS + JS bundle, today anything put under a project's `static/` directory — via `sendfile(sock_fd, file_fd, NULL, count)`. Zero userspace copy on the payload path: the kernel moves bytes from the page cache directly to the socket's send buffer. Completes the v1 kernel-primitive port started in phase 02.

## Design decisions (locked)

1. **`sendfile(2)` only.** Not `splice`, not `vmsplice`. `sendfile` handles "file fd → socket fd" exactly, which is 100% of the use case here. `splice`-through-pipe is ~30% more code for cases we don't have (non-regular-file sources).
2. **`GET /static/...` is the only route mounted.** Hard-coded for Stage 3. When `##ui` arrives in phase 6+ it'll emit bundles into this path; when typed SDK codegen arrives in phase 5 the generated JS client goes here too.
3. **No path traversal.** Canonicalise the requested path; reject anything that escapes the configured static root. Standard directory-traversal defence — `..` segments already stripped by the HTTP request parser from phase 03, but the static resolver double-checks with a realpath comparison.
4. **`open + fstat + sendfile` chain.** No `mmap`. `mmap` wins for repeated reads of the same file (where the page cache warming pays off), but `sendfile` is strictly faster for one-shot delivery since the kernel manages the page cache itself. [00-linux.md](./linux/00-linux.md) lists both; the runtime's static-asset pattern is one-shot, pick `sendfile`.
5. **`EAGAIN` backoff through the event loop.** If `sendfile` returns partial bytes (send buffer full), re-arm `EPOLLOUT` for the socket and resume when the kernel signals writable. Matches v1 wo-serve's flow.
6. **MIME by extension table.** Compact `match` on `.html`/`.css`/`.js`/`.json`/`.svg`/`.png`/`.jpg`/`.woff2`/`.wasm` covers every asset the SSR layer will emit. Unknown extensions default to `application/octet-stream`.

## Scope

### New files inside `crates/rt/src/static_files/`

| File | Responsibility | Port source |
| --- | --- | --- |
| `mod.rs` | Re-exports `StaticHandler`, `resolve` | — |
| `sendfile.rs` | Raw `sendfile(2)` wrapper + non-blocking `send_all` that co-operates with `EPOLLOUT` | [`reference/crates/wo-serve/src/sendfile.rs`](../../reference/crates/wo-serve/src/sendfile.rs) (109 LOC) |
| `resolve.rs` | Path canonicalisation + traversal defence + file existence check | [`reference/crates/wo-serve/src/resolve.rs`](../../reference/crates/wo-serve/src/resolve.rs) (80 LOC) |
| `mime.rs` | Extension → `Content-Type` table | [`reference/crates/wo-serve/src/mime.rs`](../../reference/crates/wo-serve/src/mime.rs) (44 LOC) |
| `handler.rs` | `StaticHandler` — integrates the three with phase-03's `Response` builder; returns 404 / 403 / 200 as appropriate | ~120 new LOC |

Total: ~350 LOC (233 ported + ~120 new).

### `Cargo.toml` change

None.

### Router change in `crates/rt/src/server.rs`

One new route per project:

```rust
let static_root = project_dir.join("static");
let handler     = StaticHandler::new(static_root);
router.route(Method::GET, "/static/*path", move |req, _| handler.serve(req));
```

`/static/*path` is a new wildcard pattern in the phase-03 router — add it to `route.rs` if not already supported.

## API shape (target)

```rust
use rt::static_files::StaticHandler;

let handler = StaticHandler::new("/app/static");
handler.serve(&request)?;   // returns a Response that streams via sendfile()
```

The `Response` returned by `handler.serve()` owns the open `File` fd. The phase-03 connection writer notices it's a `sendfile`-backed response and uses the non-blocking `send_all` path instead of `write`.

## Exit criteria

1. **`cargo build`** green. No new deps.
2. **Unit test:** place a 10 MB file under a temp static root, `StaticHandler::serve` on a mock request, assert the `Response` reports 200 / correct `Content-Length` / correct `Content-Type`. A separate integration test validates the actual `sendfile` path using an accepted socket.
3. **`strace` validation:**
   ```bash
   cargo run --bin wo -- run docs/examples/blog &
   PID=$!
   # ship a 10 MB file into docs/examples/blog/static/big.bin
   strace -p $PID -f -e sendfile,read,write 2>&1 | tee /tmp/strace.log &
   curl -s -o /dev/null http://127.0.0.1:8080/static/big.bin
   # assert /tmp/strace.log shows sendfile(...) calls and zero read/write
   # of the file content
   ```
4. **Path traversal attempts fail closed.** `curl :8080/static/../Cargo.toml` returns 403. `curl :8080/static/nonexistent.png` returns 404.
5. **`EAGAIN` handling.** A test that rate-limits the socket sendbuf to force a partial write exercises the `EPOLLOUT` re-arm path; the full payload still arrives.
6. **All 14 `rt` tests** + phase-02/03/04/05/06/07 additions pass. `reference/rest/blog.rest` 20 assertions still green (no regressions on the JSON endpoints).

## Non-scope

- **No `sendfile64`.** Modern glibc aliases `sendfile` to `sendfile64` transparently; explicit 64-bit selection isn't needed.
- **No range requests.** `GET /static/big.bin` with `Range: bytes=...` returns 200 + full payload in Stage 3; proper range-request handling is a follow-on. Nothing in the blog or ecommerce samples uses ranges.
- **No in-memory cache.** The kernel page cache is the only cache. Re-opening the file on every request is cheap; if future profiling says otherwise, add an LRU fd cache — but not pre-emptively.
- **No TLS.** `sendfile` over TLS requires kTLS (`setsockopt(TCP_ULP, "tls")` + kernel 4.13+ and the right cipher suites). Worth doing when TLS lands as its own phase; out of scope here.
- **No compression.** The HTTP response writer in phase 03 doesn't gzip; `sendfile` can't gzip on the fly either. Pre-compress (`.br` / `.gz` sibling files) is a future phase — for now the table lists `.br`/`.gz` extensions with the correct `Content-Encoding` but the caller has to produce the pre-compressed file itself.

## Verification

```bash
cargo build
cargo test --lib static_files
cargo test --lib                               # all existing tests green

# strace-backed zero-copy proof
cargo run --bin wo -- run docs/examples/blog &
# ... (full script from exit criterion 3)

# .rest smoke unchanged
# full 20-assertion battery against reference/rest/blog.rest

cd reference/crates && cargo build && cargo test   # v1 untouched
```

## After this phase

The runtime covers every kernel primitive listed in [`00-linux.md`](./linux/00-linux.md) except `io_uring`, `mmap`, `fallocate`, and `memfd_create` — which all belong to the storage engine (phase 3 of the database series), not the runtime per se.

Next natural phase: **`09-native-subscriptions.md`** — the `register! { #{blog-title} => notify(fd) }` model from [00-linux.md](./linux/00-linux.md). Takes the `inotify` watcher from phase 07 and wires it into a subscription table that dispatches delta writes directly to subscriber sockets over the phase-03 HTTP connection. That replaces the Stage-3 `501` stub the `/api/<type>/live` endpoint currently returns.

After that phase, `crates/rt/` is feature-complete for Stages 1–3 of the runtime, with exactly one external dependency.
