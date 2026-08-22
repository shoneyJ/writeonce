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

- **Single-threaded, blocking** — one request at a time. Concurrency arrives
  underneath this same surface now that the arc (8/11) has landed
  (2026-08-21); the switch itself rides iteration 24's serving slice.
- **TLS: none, anywhere.** Deploy behind nginx/caddy; the proxy terminates
  TLS+ALPN and gives browsers HTTP/2 while this backend speaks HTTP/1.1
  keep-alive. See the web-app sample's README for the nginx sketch.
- `Content-Length` bodies only (no chunked encoding), no WebSockets/SSE,
  JSON-first (no templates). Form-encoded bodies parse through
  `form_values(req)` (`+` and `%XX` decoded, nil on any other
  content-type); multipart/form-data through `multipart_parts(req)`
  (whole-body, bounded by BODY_MAX — no streaming uploads until
  fibers/shards) with `part_named` for fields; `media_type(req)` names
  the body's media type for content negotiation.

## The v1 surface — status ledger (2026-08-20)

The target surface of **framework v1**, tracked per item. The memory-rich
features (TTL cache, feature flags, durable job queue, `transaction { }`)
are **framework v2** — iteration 18, spec written, NOT part of v1.
Legend: ✅ shipped · 🔶 partial (gap named) · ⬜ candidate slice ·
⏸ parked behind a runtime iteration · 🔧 needs a runtime/compiler seam
first (pure `.wo` cannot express it yet).

### Transport

| Item | State |
| --- | --- |
| HTTP/1.1 parsing | 🔶 parses + 400-and-survive; STRICT ambiguity rejection (duplicate/conflicting `Content-Length`, oversize checks beyond BODY_MAX) not audited — hardening slice |
| Keep-alive | ✅ pipelined-serve / close-when-idle (arc landed 2026-08-21; retirement of close-when-idle rides iteration 24's fiber-per-connection slice) |
| Read/write/idle timeouts | 🔧 `net` has no timeout surface — runtime seam, then a framework knob |
| Request size limits | ✅ BODY_MAX bounds headers AND body |
| Unix socket binding | 🔧 `net.listen` is TCP-only — runtime seam |
| Graceful SIGTERM | ✅ in-flight request completes (blocking model), listener + fds closed, storage is per-commit durable (WAL fdatasync — nothing to checkpoint) |

### Routing

| Item | State |
| --- | --- |
| Path matching | 🔶 linear scan, first-match-wins; a radix tree is a performance slice that waits for iteration 22 to MEASURE it first |
| Method dispatch · path params · 404 · 405+`Allow` | ✅ |
| Wildcards | ⬜ only `:param` today; `*rest` capture is a candidate slice |
| Precedence rules | 🔶 registration order IS the rule (documented); specificity-based precedence unneeded until wildcards exist |
| Route groups | ⬜ candidate slice (prefix + per-group middleware) |

### Request/response

| Item | State |
| --- | --- |
| Case-insensitive headers · query parsing | ✅ (names lowercased on read) |
| JSON · form-urlencoded · multipart | ✅ all three hooks (`json.decode`, `form_values`, `multipart_parts`) |
| Content negotiation | 🔶 `media_type(req)` covers the request side; `Accept`-driven response negotiation ⬜ |
| Trusted-proxy client IP | 🔶 `X-Forwarded-For/-Proto` parsing is expressible (candidate slice); VERIFYING the peer is the trusted proxy needs a peer-address runtime seam 🔧 |
| Status/header setting · redirects | ✅ builders + `set_header` |
| Lazy body streaming + backpressure · streaming responses · explicit commit point | ⏸ UNBLOCKED by the arc (8/11 landed 2026-08-21) — stays parked until its own slice |
| ETag + conditional requests | ⬜ candidate; wants the crypto slice's hashing |

### Context & middleware

| Item | State |
| --- | --- |
| Ordered middleware chain | ✅ registration order, `?Resp` short-circuits |
| Request-scoped context | 🔶 `req.params` + `req.principal` are the context today; a general `req.ctx` bag is a candidate slice |
| Guaranteed teardown | 🔶 every fd closes on every path (gate-proven); no user teardown hooks yet |
| Cancellation into pending storage ops | ⏸ fibers (11) |
| Panic recovery | 🔶 trap = 500 and the server survives ✅; "rolls back the transaction" is framework v2 (needs `transaction { }`, iteration 18) |

### Storage integration (the differentiator — framework v2 territory)

| Item | State |
| --- | --- |
| Transaction-per-request middleware (commit on 2xx, roll back otherwise) | ⏸ **v2** — needs iteration 18's `transaction { }` |
| Cancellation → rollback | ⏸ fibers (11) + v2 |
| Migration generation + review workflow | ⬜ recorded future story (script-based destructive migrations) |
| Eager-loading API (N+1) | ⬜ query-surface work (9-series), not framework code |
| Tenant-scoped query roots | ⬜ future; wants the query surface to grow scoped roots first |

### Security

| Item | State |
| --- | --- |
| Constant-time comparison · Authorization parsing · Basic auth · principal | ✅ `http/auth.wo`, `req.principal` |
| CORS | ⬜ candidate slice (middleware + preflight answers) |
| Security headers | ⬜ candidate slice (one middleware, a header set) |
| Host validation | ⬜ candidate slice (middleware against a host allowlist) |
| Strict parsing | 🔶 same item as Transport's hardening slice |

### Crypto (self-written, hard-stop after JWT HS256)

| Item | State |
| --- | --- |
| base64 | ✅ pure `.wo` (`http/auth.wo`) |
| SHA-256 · SHA-512 · HMAC · CRC32 | 🔧 the language has NO bitwise operators — these are C runtime builtins (libc-only doctrine permits hand-rolled crypto in the runtime) or the language grows bit ops first; the fork goes to a brainstorm before the slice |
| Unlocks (signed cookies, CSRF, session integrity, webhook verification, JWT HS256) | ⬜ framework slices AFTER the hash primitives exist; **hard stop there** — no RS256, no JOSE zoo |

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
