# writeonce-framework

A web framework **written in writeonce**, consumed as a `[deps]` dependency
(iteration 15). Spec: `docs/superpowers/specs/2026-08-18-web-framework-design.md` §B.

```toml
[deps]
writeonce-framework = { git = "https://github.com/shoneyj/writeonce-framework", rev = "v0.1.0" }
```

## What it is

- **HTTP/1.1 keep-alive** server core (`http/`): request parsing
  (`Content-Length` bodies), response serialization, a blocking serve loop
  that answers 400 to malformed requests, 500 to trapping handlers (and
  survives), closes every fd, and honors SIGTERM.
- **Router** (`router/`): method + path table with `:param` captures into
  `req.params`; first match wins; no match is the framework's 404.
- **Handlers without closures**: the language has no function values by
  doctrine, so a route handler is a class satisfying the `Handler` interface
  (`fn handle(req: Req) -> Resp`), dispatched structurally — a
  non-conforming handler is a compile error (WO-E205). Middleware is its own
  interface (`fn before(req: Req) -> ?Resp`; nil = continue, a `Resp`
  short-circuits).
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
  JSON-first (no templates).

## The consuming sample

`docs/examples/web-app` — a small storefront importing this framework
through `[deps]`. Its acceptance (`just web-app`) exercises the whole chain:
fetch → lock → build → serve → durable restart.
