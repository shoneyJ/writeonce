# Iteration 16 — the web framework + web-app: implementation plan

> **Status: COMPLETE (2026-08-19)** — all tasks landed on branch
> `web-framework`; `just web-app` 14/0. Deviations from the plan as written,
> recorded honestly: (1) registration is `app.add(Route { method, pattern,
> h })` — the get/post/put helper fns were dropped because the ctor-literal-
> plus-`take` shape is the ownership pattern the corpus pins
> (`run/container-owned-move`); a helper taking an interface-typed parameter
> is unproven ground, deferred; (2) show/delete key on `:name` (the @unique
> index probe, the employee-proven pattern), not `:id` — row-id lookup is not
> in the query surface; (3) the `[deps]` key is `framework` (hyphens are not
> identifier characters in `use` paths); (4) connection policy became
> pipelined-keep-alive/close-when-idle after a probe showed an idle parked
> connection starves accept on a single-threaded server; (5) two compiler
> gaps surfaced and were fixed en route: owned-values-in-containers now MOVE,
> and a dep's internal `use` paths resolve dep-relatively; (6) Dispatcher
> takes `mut req` so :param captures land on the borrowed request — the
> borrow checker correctly refused rebuilding a Req from borrowed maps.

> **For agentic workers:** use superpowers:executing-plans (inline) or
> subagent-driven-development. Steps are checkboxes. Per repo rule, this plan
> carries **actions in words + verification commands, no code blocks** — the
> normative design is the spec, which travels with this plan.

**Goal:** `docs/examples/writeonce-framework/` — a web framework written in
writeonce (HTTP/1.1 keep-alive server core, router with `:param` captures,
`Handler`/`Middleware` structural interfaces, `Req`/`Resp` records) — and
`docs/examples/web-app/`, a storefront that consumes it **through
`[deps]`** (iteration 15) and persists through `@table`. The app, not the
framework, owns the entry, the data model, and the routes.

**Architecture:** pure `.wo` — no compiler or runtime changes are expected
(the two enabling probes already pass: interface-typed fields dispatch via
ICALL, and the route-table pattern is pinned ASan-clean by
`tests/corpus/run/container-owned-move`). The server core is the proven
log-watcher `mcp` pattern (blocking `net` loop, fd-clean, stop-clean),
generalized. TLS/h2 live at the reverse proxy; the backend speaks HTTP/1.1
keep-alive with `Content-Length` bodies only.

**Spec:** [`../specs/2026-08-18-web-framework-design.md`](../specs/2026-08-18-web-framework-design.md)
section B (normative; §C's h2c stays parked). Story:
[`16-web-framework.md`](../../stories/language-runtime-database/done/16-web-framework.md).

## Global Constraints

- Framework and app are SEPARATE projects: the framework never references the
  app; the app reaches the framework only via `use` over a `[deps]` fetch —
  never a relative path. Extraction to its own repo must be a URL change.
- Single-threaded blocking serving, stated in the framework README with the
  nginx upstream-keepalive deployment story; no chunked encoding, no
  WebSockets, JSON-first (all disclosed).
- A handler trap answers 500 and the server survives; a malformed request
  answers 400 and closes; every accept path closes its fd; `env.stopping()`
  honored — the same discipline log-watcher's soak enforces.
- The committed `web-app/wo.toml` carries the framework's FUTURE GitHub URL
  as documentation; the acceptance gate substitutes a run-time `file://`
  remote (a temp git repo built from the framework directory) into a temp
  copy of the app, so the repo never contains `.wo-deps`/`wo.lock` artifacts
  and CI never touches the network.
- Gates: `just web-app` (the new acceptance), plus the standing
  `just woc-test` / `just oop-e2e` / sample gates stay green untouched.

---

## Task 1 — framework skeleton: `Req`/`Resp`, builders, project shape

**Files:** create `docs/examples/writeonce-framework/{wo.toml,README.md,
http/types.wo}`.

- [x] Project manifest (`name = "writeonce-framework"`); README states what
  it is, the single-thread/proxy deployment story, and the disclosed limits
  up front.
- [x] `Req` record: method, path, params (`:param` captures), query, headers
  (all `map<Text, Text>`), body Text. `Resp` record: status, headers, body.
  Builder free fns: ok_text, ok_json, not_found, bad_request, server_error,
  redirect — each returns a fully-formed `Resp` (content-type set).
- [x] Verify: the framework directory typechecks standalone (`woc` on it —
  no `fn main`, so check-only) with zero diagnostics. Commit.

## Task 2 — HTTP/1.1: parse, serialize, keep-alive serve loop

**Files:** create `docs/examples/writeonce-framework/http/{parse.wo,serve.wo}`.

- [x] Request parsing from a `net` connection: request line (method,
  target — split path from query string, decode `%`-escapes in both), header
  lines to the blank line, then exactly `Content-Length` bytes of body
  (missing length = empty body; a non-integer length or an oversized one is
  a 400). Header names lowercase on read so lookups are predictable.
  Anything malformed: respond 400, close, continue serving.
- [x] Response serialization: status line with reason text, headers,
  `Content-Length` always computed from the body, `Connection: keep-alive`
  unless the request asked to close.
- [x] The serve loop: `net.listen`, accept, then per connection read
  requests until EOF/close/stop — the keep-alive inner loop; the dispatch
  callback boundary is a structural interface the router provides (Task 3),
  wrapped in `try` so a trapping handler answers 500 and the loop lives.
  Close the connection fd on every exit path and the listener on stop.
- [x] Verify with a throwaway `.wo` main beside the framework (not
  committed): serve one echo handler, curl matrix — GET with query,
  keep-alive reuse (two requests, one connection), 400 on garbage, SIGTERM
  stops cleanly. Commit.

## Task 3 — router, `Handler`/`Middleware`, the `App` assembly

**Files:** create `docs/examples/writeonce-framework/{router/router.wo,app.wo}`.

- [x] `Handler` interface (`handle(req) -> Resp`) and `Middleware` interface
  (`before(req) -> ?Resp`, nil = continue) — the spec's shapes verbatim.
- [x] Route table: `App` holds `multi` of route records (method, the pattern
  split into segments, the handler value). Matching walks segments; a
  `:name` segment captures into `req.params`. First match wins; no match is
  the framework's 404. Registration helpers: get/post/put/delete_ plus a
  generic route(method, pattern, handler).
- [x] `App.serve(port)`: run middleware in order (a `Resp` short-circuits —
  `?Resp` narrowing), then route, then the matched handler, all inside the
  Task-2 loop's try boundary.
- [x] Verify with the throwaway main: two routes incl. `/things/:id`
  echoing the capture, a header-checking middleware that short-circuits 401,
  404 for unknown paths, 500 for a deliberately trapping handler with the
  server surviving. Commit.

## Task 4 — the web-app storefront

**Files:** create `docs/examples/web-app/{wo.toml,README.md,types.wo,main.wo}`
(+ a module justfile mirroring the other samples).

- [x] `wo.toml`: `[deps] writeonce-framework = { git = <future GitHub URL>,
  rev = "v0.1.0" }` (documentation value; the gate substitutes a `file://`
  remote), `[build]` runtime unpinned (portable, per the log-watcher
  precedent).
- [x] Data model: `@table Product` (name @unique, price, stock) and
  `@table Order` (`ref Product`, qty) — small, honest, exercising `@unique`
  and FK restrict through web routes.
- [x] Routes: list products (query + json encode), show by `:id`, create
  product (json decode body — the checked decode's nil path is a 400),
  create order (FK), delete product (FK restrict surfaces as a 409-style
  error body, caught via try). One auth middleware (a shared-token header,
  401 otherwise) registered before the routes.
- [x] `fn main`: build the App, register middleware + routes, `serve(port)`
  with the port from args. README documents the curl matrix and the nginx
  h2-in-front config sketch.
- [x] Verify by hand end to end once (fetch via a local file:// remote,
  serve, curl, restart, drop). Commit.

## Task 5 — the acceptance gate

**Files:** create `scripts/web-app-accept.sh`; modify `justfile`
(`web-app` recipe).

- [x] The gate builds the whole chain at run time: git-init a temp remote
  from `docs/examples/writeonce-framework/` (tag `v0.1.0`), copy
  `docs/examples/web-app/` to a temp dir, substitute the `file://` URL into
  its manifest, then: fetch+build (lock written); serve on a scratch port
  with `WO_DATA` set; curl matrix — 401 without the token, list empty,
  create product, list shows it, show by id, unknown path 404, malformed
  json body 400, create order, delete-restricted product answers the error
  body while the server keeps serving; keep-alive reuse; kill -TERM stops
  cleanly; restart and the product list still answers (WAL persistence);
  fd/resident flatness via an opt-in `WA_SOAK` mirroring log-watcher's.
- [x] Wire `just web-app`; run it plus the standing gates
  (`just woc-test`, `just oop-e2e`, `just deps-accept`, `just log-watcher`,
  `just employee`) — all green.
- [x] Commit.

## Task 6 — docs closeout

**Files:** modify `docs/00-status.md` (iteration 16 → done with what
landed), story `16-web-framework.md` (landing note), `README.md` (one
paragraph + pointer under the samples list), `docs/08-project-structure.md`
(the two new sample entries).

- [x] Apply; `just web-app` still green; commit.

## Success criteria (spec §Success criteria, restated)

1. web-app builds purely through `[deps]` + `wo.lock`; no path references;
   offline once locked.
2. The storefront serves with `@table` persistence across restart; nginx in
   front gives browsers h2 while the backend speaks HTTP/1.1 (documented,
   demonstrated manually).
3. A non-conforming handler is WO-E205 at compile time; a trapping handler
   answers 500 and the server survives (gate-proven).
4. Extraction = changing the app's `[deps]` URL only.

## Self-review notes

- Spec §B coverage: types/builders → T1; parse/serialize/loop → T2;
  router/interfaces/App → T3; storefront + data model → T4; the gate → T5;
  docs → T6. §C (h2c) deliberately absent. No placeholders; interface names
  and record shapes match the spec exactly.
- Two risks retired BEFORE this plan: interface-typed fields dispatch
  (probe passed) and the route-table double-free (fixed + pinned by
  `run/container-owned-move`).
- Open risk, disclosed: `%`-escape decoding and header-case handling are
  easy to get subtly wrong — T2's verify step includes them explicitly, and
  the gate's curl matrix covers a query with an encoded space.
