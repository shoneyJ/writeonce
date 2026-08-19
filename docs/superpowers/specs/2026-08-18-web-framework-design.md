# Web framework + dependency system — design spec

**Date:** 2026-08-18
**Status:** approved design, pre-implementation
**Scope:** (A) a minimal git-backed dependency system (`wo.toml [deps]` +
`wo.lock`), (B) a web framework written in writeonce as an importable `.wo`
library, HTTP/1.1 behind a TLS-terminating reverse proxy, and (C) the parked
HTTP/2 path. Three sub-projects; A and B are the fundable ones, C is a
recorded successor.
**Relates to:** iteration 10 (`service` blocks — this framework becomes their
lowering target, not a rival), iterations 8/9f/11 (the concurrency work h2c
waits for), `docs/plan/discarded.md` (FFI reject row — load-bearing here).

## Decisions locked during brainstorming

| Question | Decision |
| --- | --- |
| TLS | **Proxy-terminated** (nginx/caddy). Browsers get TLS+ALPN+h2 from the proxy; the framework speaks HTTP/1.1 (later h2c) behind it. Zero TLS in the language or runtime. Direct-serving TLS is a separate future iteration and would be judged against the FFI/libc-only doctrine then, not now. Homegrown TLS is refused outright: a decade of side-channel and certificate-validation subtleties makes it a security liability, not a milestone. |
| Dependencies | **`wo.toml [deps]` + git fetch.** A real (mini) package manager: exact-rev git dependencies, a lockfile, a per-project cache. No registry, no semver solving. |
| HTTP/2 | **v1 is HTTP/1.1 keep-alive.** h2's payoff is multiplexing, which a single blocking thread cannot exploit; h2c lands as its own iteration after shards (8) / io_uring (9f) / fibers (11). Behind the proxy, browsers see h2 from day one regardless. |
| Handler model | **Structural interfaces, not closures.** The language has no function values by doctrine; a route handler is a class satisfying a `Handler` interface, dispatched by ICALL — which works on today's runtime and is checked by WO-E205. |
| Incubation | Framework is born at `docs/examples/writeonce-framework/`; the consuming app at `docs/examples/web-app/` imports it **through the `[deps]` mechanism** (a local git URL), so the whole import chain is exercised by the sample. Extraction to `github.com/shoneyj/<name>` later is a `git subtree split`, not a redesign. |

Rejected: TLS in the runtime via rustls/OpenSSL (breaks libc-only for a
benefit the proxy already provides; revisit only if direct serving becomes a
requirement); nghttp2 binding (same doctrine cost, and the scheduler cannot
use multiplexing yet); vendor-directory imports (the user chose the real
dependency mechanism); native h2 in v1 (HPACK synchronization and
flow-control deadlocks are where the time goes — the user's own estimate).

## A. Dependency system (`wo.toml [deps]`, `wo.lock`)

**Manifest.** `wo.toml` gains one section:

```
[deps]
niceframework = { git = "https://github.com/shoneyj/niceframework", rev = "v0.1.0" }
```

`rev` is mandatory and exact (a tag or SHA). No version ranges, no registry,
no resolution algorithm — those are future work a corpus of real deps must
justify first.

**Fetch.** `woc` shells out to the `git` binary (`git clone --depth 1`,
`git checkout <rev>`) — no network code inside the compiler, OCaml-stdlib
doctrine intact; `git` joins `cc` in the "external tools the toolchain may
invoke" set. Dependencies land in `.wo-deps/<name>/` beside `wo.toml`
(gitignored). A missing `git` binary or unreachable remote is a plain
diagnostic, never a hang without a message.

**Lockfile.** `wo.lock` records `name -> resolved SHA` for every fetch.
Present lockfile wins over the manifest's rev label (a moved tag is detected
and reported, not silently followed). `woc --update-deps` refreshes the lock;
plain builds never touch the network when `.wo-deps` already satisfies the
lock.

**Resolution.** A dependency is an ordinary writeonce project (its own
`wo.toml` with `name`). `use <depname>` resolves the dep's root directory as
a module root, exactly like a project-internal module; `use <depname>/sub`
reaches its subdirectories. Collisions between a dep name and a local module
diagnose (the existing WO-E collision family).

**Transitive deps are refused in v1.** A fetched dep whose own `wo.toml`
carries `[deps]` is a diagnostic naming the dep — flat-only keeps the
resolver small and the failure honest. Recorded as the successor iteration's
first fork.

**Entry-point rule.** A dep's `fn main` (if any) is ignored — only the
consuming app owns the entry. `pub` visibility applies across the dep
boundary exactly as across modules.

## B. The framework (v1, HTTP/1.1, library-only)

**Repo shape.** `docs/examples/writeonce-framework/` is a complete writeonce
project (`wo.toml name = "niceframework"` — final name decided at extraction)
whose modules are the framework:

- `http/` — request parsing, response serialization, keep-alive loop.
  Extracted from the proven log-watcher `mcp` pattern (601k requests soaked,
  fd-clean, SIGTERM-clean).
- `router/` — method+path table with `:param` captures.
- `app.wo` — the assembly: register routes/middleware, `serve(port)`.

**Types (records).**

- `Req`: `method: Text`, `path: Text`, `params: map<Text, Text>` (the
  `:param` captures), `query: map<Text, Text>`, `headers: map<Text, Text>`,
  `body: Text`.
- `Resp`: `status: Int`, `headers: map<Text, Text>`, `body: Text`, plus
  builder helpers (`ok_json`, `ok_text`, `not_found`, `redirect`, …).

**Handlers.** One structural interface:

```
interface Handler {
  fn handle(req: Req) -> Resp
}
```

A route is any class satisfying it (state lives in the class's own fields —
the closure substitute). Registration passes the handler *value*:
`app.get("/products/:id", ProductShow { ... })`. Dispatch is ICALL;
WO-E205 makes a non-conforming handler a compile error.

**Middleware.** Its own one-method interface (distinct from `Handler`,
because the signatures differ):

```
interface Middleware {
  fn before(req: Req) -> ?Resp
}
```

The app holds a `multi` of middleware run in order before routing (auth,
logging, content-type defaults). Returning a `Resp` short-circuits; nil means
"continue" — `?Resp` narrowing is exactly what the 2026-08-18 `?T`
enforcement shipped, used as a design tool.

**Server core.** `net.listen/accept` blocking loop, HTTP/1.1 with keep-alive
and `Content-Length` bodies (no chunked encoding in v1 — disclosed), fd
closed on every exit path, `env.stopping()` honored. **Single-threaded,
blocking, one request at a time** — stated in the framework README, with the
proxy config (nginx upstream keepalive) as the deployment story. Concurrency
arrives via iterations 8/11 underneath the same library surface.

**Database.** Nothing to build: handlers use `@table` + the query surface
directly. This is the differentiator — a durable, compiler-checked data layer
with no ORM and no separate database process, in the same binary.

**The consuming app.** `docs/examples/web-app/` — a small storefront:
`Product`/`Order` as `@table` classes, list/show/create routes, one auth
middleware, JSON responses. Its `wo.toml [deps]` points at the framework by
git URL (`file://` in CI, the GitHub URL after extraction). Its acceptance
script is the whole feature's gate: fetch deps, build, serve, curl the
routes, restart-persistence check, SIGTERM.

## C. HTTP/2 (h2c) — parked successor

After iterations 8 (shards) / 9f (io_uring) / 11 (fibers): h2c framing +
HPACK, either natively (needs a bytes/buffer type with cheap slicing — that
type rides with this iteration, not v1) or via nghttp2-in-runtime (a doctrine
decision to re-argue then, with the TweetNaCl precedent and the libc-only
rule both on the table). Behind the proxy, h2c's win is backend multiplexing;
browsers already had h2 since v1. No TLS obligation even here.

## Error handling

Dep failures (missing git, bad rev, dirty cache, transitive deps) are
compiler diagnostics with the WO-E1xx driver family. Framework runtime
failures follow house rules: malformed requests get a 400 and a closed
connection, handler traps are caught at the serve loop (`try`) and answered
with a 500 — a bad request must never kill the server; the soak asserts
resident + fd flatness exactly like log-watcher's.

## Testing

- Dep system: fixture projects under `tests/` — fetch-and-build from a local
  git repo, lockfile drift detection, transitive-dep refusal, collision
  diagnostic. Network-free (local `file://` remotes).
- Framework: unit-ish `.wo` fixtures for the router and parser; the web-app
  acceptance script (curl matrix incl. keep-alive reuse, 404/400/500 paths,
  restart persistence, stop) is the gate; `LW_SOAK`-style opt-in soak.
- The whole chain (deps fetch -> build -> serve) runs in `just web-app`.

## Success criteria

1. `docs/examples/web-app` builds by fetching `writeonce-framework` through
   `[deps]` + `wo.lock` with no path references, and `woc` never touches the
   network when the lock is satisfied.
2. The storefront serves list/show/create with `@table` persistence across a
   restart, behind nginx with browser-visible h2 (proxy-terminated), while
   the backend speaks HTTP/1.1.
3. A non-conforming handler class is WO-E205 at compile time; a trapping
   handler answers 500 and the server survives (soak-proven).
4. Framework extraction to a standalone repo requires changing only the
   app's `[deps]` URL.

## Out of scope (v1)

TLS anywhere in the toolchain; HTTP/2 and the bytes/buffer type (parked to
C); chunked transfer encoding; WebSockets/SSE (needs the push story);
transitive dependencies, version ranges, registries; templates/SSR (the
removed UI track is not resurrected here — JSON APIs first); multipart
uploads.
