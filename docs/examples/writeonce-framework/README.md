# writeonce-framework

A web framework **written in writeonce**, consumed as a `[deps]` dependency
(iteration 15). Spec: `docs/superpowers/specs/2026-08-18-web-framework-design.md` §B.

```toml
[deps]
writeonce-framework = { git = "https://github.com/shoneyj/writeonce-framework", rev = "v0.1.0" }
```

## What it is

- **HTTP/1.1** server core (`http/`): request parsing (`Content-Length`
  bodies, %-decoded paths and query strings), response serialization, a
  blocking serve loop that answers 400 to malformed requests, 500 to
  trapping handlers (and survives), closes every fd, and honors SIGTERM.
  Connection policy: **pipelined requests are served on one connection;
  idle connections close after the response** — on a single-threaded server
  a parked keep-alive connection would block `accept` and starve every
  other client, so closing is the correct shape until shards/fibers (8/11).
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
  underneath this same surface with the shard/fiber iterations (8/11).
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

## The core checklist (what a framework core owes, and where this one is)

| Core concern | State |
| --- | --- |
| HTTP parsing + connection lifecycle | ✅ `http/parse.wo`, `http/serve.wo` (keep-alive, 400-and-survive, fd-clean, SIGTERM) |
| Routing: path params, method dispatch, precedence | ✅ `:param` captures, first-match-wins, wrong-method = 405 + `Allow` |
| Middleware chain, ordering guarantee | ✅ registration order, `?Resp` short-circuits |
| Request/response types | ✅ `Req`/`Resp` + builders + `set_header` |
| Bearer/Basic auth mechanism + principal | ✅ `http/auth.wo`, `req.principal` |
| Body parsing hooks: JSON | ✅ the language's checked `json.decode` |
| Body parsing hooks: form-encoded | ✅ `form_values(req)` — nil unless the content-type says form; `media_type(req)` exposed for content negotiation |
| Body parsing hooks: multipart | ✅ `multipart_parts(req)` (RFC 7578: fields + file parts, filename/mime kept) + `part_named` |
| Error handling → status mapping | 🔶 trap = 500, builders per status; a per-error mapping hook is a candidate slice |
| Body streaming, backpressure | ⏸ needs fibers/shards (iterations 8/11) — whole bodies until then, by design |
| Cancellation propagation | ⏸ process-level only (`env.stopping()`); per-request cancel needs fibers (11) |
| Configuration + graceful shutdown | 🔶 SIGTERM drains and closes clean; config is ctor fields — a config record is a candidate slice |

## The consuming sample

`docs/examples/web-app` — a small storefront importing this framework
through `[deps]`. Its acceptance (`just web-app`) exercises the whole chain:
fetch → lock → build → serve → durable restart.
