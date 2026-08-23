# writeonce-framework

A web framework **written in writeonce**, consumed as a `[deps]` dependency
(iteration 15). Spec: `docs/superpowers/specs/2026-08-18-web-framework-design.md` §B.

```toml
[deps]
writeonce-framework = { git = "https://github.com/shoneyj/writeonce-framework", rev = "v0.1.0" }
```

## What it is

- **HTTP/1.1** server core: request parsing (`Content-Length`
  bodies, %-decoded paths and query strings), response serialization, a
  blocking serve loop that answers 400 to malformed requests, 500 to
  trapping handlers (and survives), closes every fd, and honors SIGTERM.
  Connection policy: **pipelined requests are served on one connection;
  idle connections close after the response** — on a single-threaded server
  a parked keep-alive connection would block `accept` and starve every
  other client, so closing is the correct shape until fiber-per-connection
  serving lands (the arc — 8/11 — landed 2026-08-21; the serve-loop slice
  that consumes it is iteration 24's).
  A proxy in front simply reconnects.
- **Router** (`router/`): method + path table with `:param` captures into
  `req.params`; first match wins; a known path with the wrong method is
  **405 with the `Allow` header** (registration order); no matching path is
  the framework's 404. **HEAD is served free**: routed as GET, body
  suppressed, `Content-Length` still names the body a GET would carry.
- **Registration helpers**: `app.get/post/put/delete_(pattern, handler)`
  push the route for you (`delete_` because `delete` is the query
  keyword); `app.add(Route { ... })` stays for anything else. A
  request-line `Logging` middleware ships in `router/`, and
  `set_header(resp, name, value)` is the escape hatch for headers the
  builders don't set.
- **Handlers without closures**: the language has no function values by
  doctrine, so a route handler is a class satisfying the `Handler` interface
  (`fn handle(req: Req) -> Resp`), dispatched structurally — a
  non-conforming handler is a compile error (WO-E205). Middleware is its own
  interface (`fn before(req: Req) -> ?Resp`; nil = continue, a `Resp`
  short-circuits).
- **Auth mechanism in core** (`http/auth.wo`): `Authorization` header
  parsing (scheme split, case-insensitive), pure-`.wo` base64, a
  constant-time comparator (`ct_eq`, no early exit), and the blessed
  principal slot — `req.principal` is `""` until an auth middleware
  authenticates, then downstream handlers read who it is. `BearerAuth`
  and `BasicAuth` (with the `WWW-Authenticate` challenge) ship as
  middlewares; POLICY — which routes, which users, where secrets live —
  stays in the app, on top of `bearer_token`/`basic_credentials`/`ct_eq`.
- **Data layer for free**: handlers use `@table` + the query surface
  directly — durable, compiler-checked persistence in the same binary. No
  ORM, no database server.

## Honest limits (v1, all deliberate)

- **Concurrency is the APP's ten lines** (iteration 35's serving slice):
  the framework ships `serve_conn` — the keep-alive loop with read/idle
  deadlines — and the app owns accept + one spawned ConnWorker actor per
  connection (web-app's pattern; `spawn` takes a class literal, so this
  cannot live in the library). Parallel requests, stalled-client
  eviction and parked idle keep-alive are gate-proven. The plain
  `serve()` stays single-threaded for simple apps.
- **TLS: none, anywhere.** Deploy behind nginx/caddy; the proxy terminates
  TLS+ALPN and gives browsers HTTP/2 while this backend speaks HTTP/1.1
  keep-alive. See the web-app sample's README for the nginx sketch.
- `Content-Length` bodies only (no chunked encoding), no WebSockets/SSE,
  JSON-first (no templates). Form-encoded bodies parse through
  `form_values(req)` (`+` and `%XX` decoded, nil on any other
  content-type); multipart/form-data through `multipart_parts(req)`
  (whole-body, bounded by BODY_MAX — the arc landed 2026-08-21;
  streaming uploads stay parked until their own slice) with
  `part_named` for fields; `media_type(req)` names
  the body's media type for content negotiation.

## The v1 surface — status ledger (2026-08-22)

The target surface of **framework v1**, tracked per item. The memory-rich
features (TTL cache, feature flags, durable job queue, `transaction { }`)
are **framework v2** — iteration 18, spec written, NOT part of v1.
Legend: ✅ shipped · 🔶 partial (gap named) · ⬜ candidate slice ·
⏸ parked behind a runtime iteration · 🔧 needs a runtime/compiler seam
first (pure `.wo` cannot express it yet).

### Transport

| Item | State |
| --- | --- |
| HTTP/1.1 parsing | ✅ parses + 400-and-survive; duplicate `Content-Length` rejected outright (RFC 9112 §6.3, slice 2); BODY_MAX bounds headers and body |
| Keep-alive | ✅ RETIRED close-when-idle (iteration 35's serving slice): under the app-owned fiber-per-connection pattern, idle connections PARK until the idle deadline; the sequential `serve()` keeps the old policy for simple apps |
| Read/write/idle timeouts | ✅ iteration 35: per-call deadlines (`net.read_dl`/`accept_dl`/`write_dl`, nil/false = the expected timeout); `serve_conn(read_ms, idle_ms)` bounds slow-loris AND idle keep-alive |
| Request size limits | ✅ BODY_MAX bounds headers AND body |
| Unix socket binding | ✅ `net.listen_unix(path)` (iteration 35) — stale sockets unlinked before bind, same accept/read/write after |
| Graceful SIGTERM | ✅ in-flight request completes (blocking model), listener + fds closed, storage is per-commit durable (WAL fdatasync — nothing to checkpoint) |

### Routing

| Item | State |
| --- | --- |
| Path matching | 🔶 linear scan, first-match-wins; a radix tree waits on a MEASUREMENT first — 22's harness landed (benched the DB, not the router); needs a perf-targets register entry |
| Method dispatch · path params · 404 · 405+`Allow` | ✅ |
| Wildcards | ✅ `*rest` as the LAST pattern segment captures the joined tail (empty rest matches) — slice 2 |
| Precedence rules | ✅ registration order IS the rule; wildcards capture only in last position, so order stays the whole story |
| Route groups | ✅ `Group { prefix }` + per-group before-middleware, mounted in one move — slice 2 |

### Request/response

| Item | State |
| --- | --- |
| Case-insensitive headers · query parsing | ✅ (names lowercased on read) |
| JSON · form-urlencoded · multipart | ✅ all three hooks (`json.decode`, `form_values`, `multipart_parts`) |
| Content negotiation | ✅ `media_type(req)` request-side; `accepts(req, mtype)` response-side (exact, type/*, */*; q-values stripped not ranked — ranking waits for an app serving alternates) — slice 2 |
| Trusted-proxy client IP | 🔶 `client_ip(req)` parses X-Forwarded-For; `net.peer(fd)` (iteration 35) exposes the peer — the verify middleware is now a pure-`.wo` candidate slice |
| Status/header setting · redirects | ✅ builders + `set_header` |
| Lazy body streaming + backpressure · streaming responses · explicit commit point | ⏸ UNBLOCKED by the arc (8/11 landed 2026-08-21) — stays parked until its own slice |
| ETag + conditional requests | ✅ `etag_for` (quoted base64 SHA-256) + `with_etag` (If-None-Match → 304) over iteration 34's digest builtins — slice 2 |

### Context & middleware

| Item | State |
| --- | --- |
| Ordered middleware chain | ✅ registration order, `?Resp` short-circuits |
| Request-scoped context | ✅ `req.ctx` map (slice 2): middleware writes, handlers read; identity stays in `principal` |
| Guaranteed teardown | 🔶 every fd closes on every path (gate-proven); no user teardown hooks yet |
| Cancellation into pending storage ops | ⏸ UNBLOCKED by the arc (8/11 landed 2026-08-21) — stays parked until its own slice |
| Panic recovery | 🔶 trap = 500 and the server survives ✅; "rolls back the transaction" is framework v2 (needs `transaction { }`, iteration 18) |

### Storage integration (the differentiator — framework v2 territory)

| Item | State |
| --- | --- |
| Transaction-per-request middleware (commit on 2xx, roll back otherwise) | ⏸ **v2** — needs iteration 18's `transaction { }` |
| Cancellation → rollback | ⏸ arc landed; still needs v2's `transaction { }` (iteration 18) |
| Migration generation + review workflow | ⬜ recorded future story (script-based destructive migrations) |
| Eager-loading API (N+1) | ⬜ query-surface work (9-series), not framework code |
| Tenant-scoped query roots | ⬜ future; wants the query surface to grow scoped roots first |

### Security

| Item | State |
| --- | --- |
| Constant-time comparison · Authorization parsing · Basic auth · principal | ✅ `http/auth.wo`, `req.principal` |
| CORS | ✅ `Cors { allow_origin }` — preflight 204 (before) + origin stamp on every response (after) — slice 2 |
| Security headers | ✅ `SecurityHeaders` after-middleware (nosniff, DENY, referrer-policy); HSTS stays at the TLS proxy by design — slice 2 |
| Host validation | ✅ `HostAllow { host }` answers 421 before any route — slice 2 |
| Strict parsing | ✅ same item as Transport's row: duplicate Content-Length is a 400 |

### Crypto (self-written, hard-stop after JWT HS256)

| Item | State |
| --- | --- |
| base64 | ✅ pure `.wo` (`http/auth.wo`) |
| SHA-1 · SHA-256 · HMAC-SHA256 | ✅ C runtime builtins (iteration 34, ids 85–87, RFC-vector gated); SHA-512/CRC32 wait for a consumer |
| Unlocks (signed cookies, CSRF, session integrity, webhook verification, JWT HS256) | ⬜ UNBLOCKED (the primitives exist since iteration 34); each is its own slice; **hard stop at JWT HS256** — no RS256, no JOSE zoo |

## Layout and privacy (iteration 17)

This project declares `kind = "library"` in `wo.toml`, so `woc <dir>` runs the
FULL pipeline over it — parse, typecheck, interface satisfaction, ownership, GC
inference — with no `fn main` required, and writes nothing. That retired
iteration 16's `woc --emit` verification workaround. `woc build` on it fails
naming the kind, unless a demo `main` is added (lib+bin is allowed).

- `http/` — the public surface: `Req`/`Resp` and the response builders
  (`types.wo`), auth (`auth.wo`), multipart (`multipart.wo`), and the
  body-inspection pair `media_type`/`form_values` (`form.wo`).
- `router/` — the route table and `Logging`.
- `app.wo` — `App`, the registration helpers, the dispatch loop.
- **`internal/` — not importable by a consumer.** The connection-level request
  parser and carry-state record (`parse.wo`) and the serve loop, status text,
  and response serializer (`serve.wo`) live here. A consuming app that writes
  `use writeonce-framework/internal` gets **WO-E108** at that `use`. The rule
  is Go's: a path segment named `internal` is refused across the `[deps]`
  boundary only — the framework's own modules import it freely.

One honest disclosure: privacy restricts NAMING, not code size. `internal/`
modules still compile into the consumer's single image (there is no dead-code
elimination); a consumer simply cannot name them.

## The consuming sample

`docs/examples/web-app` — a small storefront importing this framework
through `[deps]`. Its acceptance (`just web-app`) exercises the whole chain:
fetch → lock → build → serve → durable restart.
