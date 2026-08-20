# Iteration 16 — the web framework: a `.wo` library, HTTP/1.1 behind a proxy

> Format: fiberloom `product/story-iteration-template`. Part of
> [Story — one language, one runtime, one database, one binary](00-story.md).
>
> **Inserted 2026-08-18. LANDED 2026-08-19** (branch `web-framework`):
> `just web-app` 14/0 — deps chain, auth middleware, CRUD with @unique 409 /
> FK-restrict 409 / checked-decode 400, :name captures, 404, pipelined
> keep-alive, SIGTERM, WAL restart persistence. Two notable as-built facts:
> (1) connection policy is pipelined-keep-alive/close-when-idle — a probe
> showed a parked keep-alive connection starves accept on a single-threaded
> server; (2) the chain exposed and fixed two compiler gaps — owned values
> stored in containers now MOVE (`run/container-owned-move`), and a dep's
> internal `use` paths resolve dep-relatively.
>
> **v1 polish LANDED 2026-08-20** (branch `framework-v1`, developer
> directive: ship routing/middleware/req-resp as a polished micro-framework,
> nothing MVC-scale): registration helpers `get/post/put/delete_` (the
> take-Handler shape, probe-proven — deviation 1 of the 16 plan retired),
> 405 + `Allow` on wrong-method path hits, HEAD served as GET with the body
> suppressed (RFC 9110 §9.3.2), a request-line `Logging` middleware,
> `set_header`; `just web-app` 16/0. It exposed and fixed a third compiler
> gap: a Text single-segment interpolation of a place crossed
> `let`/assignment boundaries uncopied — `copy_place_text` now sees through
> `Interp` (`run/interp-borrowed-field`).
>
> **Auth-in-core LANDED 2026-08-20** (same branch): `http/auth.wo` — the
> MECHANISM per the core doctrine: `Authorization` parsing, pure-`.wo`
> base64 (RFC 4648), constant-time `ct_eq`, `req.principal` as the blessed
> "who is this" slot (Middleware.before now takes `mut req` to write it),
> `BearerAuth` + `BasicAuth` (WWW-Authenticate challenge) middlewares.
> Policy stays app-side. Probe matrix 26/26 incl. RFC vectors, ASan-clean;
> web-app dogfoods `BearerAuth` (its hand-rolled Auth deleted);
> `just web-app` 17/0. The framework README now carries the core CHECKLIST:
> what is ✅ (parsing, routing, middleware, types, auth, JSON), what is a
> candidate slice (form/multipart, error-mapping hook, config record), and
> what parks behind 8/11 by design (streaming, backpressure, per-request
> cancellation).
>
> **Form-encoded bodies LANDED 2026-08-20** (same branch): `media_type(req)`
> (content-type lowercased, parameters stripped) and `form_values(req)`
> (nil unless the content-type says form; '+' and %XX decoded through the
> existing query decoder). Probe 7/7 release + ASan; web-app's
> CreateProduct now accepts form OR JSON into one insert path;
> `just web-app` 19/0. Multipart stays the candidate next slice.
>
> The framework is written IN writeonce and imported
> like any dependency (iteration 15 is the prerequisite). TLS terminates at a
> reverse proxy — browsers get TLS+ALPN+h2 from nginx/caddy while the
> framework speaks HTTP/1.1 keep-alive behind it, so no TLS exists anywhere
> in the toolchain. h2c is the parked successor (after iterations 8/9f/11,
> when multiplexing has a scheduler to pay off on).
>
> **Spec exists:** [`2026-08-18-web-framework-design.md`](../../superpowers/specs/2026-08-18-web-framework-design.md)
> sections B (normative) and C (the parked h2c successor). **Plan:**
> [`2026-08-19-web-framework.md`](../../superpowers/plans/2026-08-19-web-framework.md)
> (6 tasks: types/builders; HTTP/1.1 parse+serve; router+interfaces+App;
> the web-app storefront; the `just web-app` gate; docs closeout). Two
> enabling risks retired before planning: interface-field dispatch (probe)
> and the route-table owned-move double-free (fixed, pinned by
> `run/container-owned-move`).

## Goals

- **The framework**, incubated at `docs/examples/writeonce-framework/`: an
  HTTP/1.1 keep-alive server core (extracted from the soak-proven log-watcher
  `mcp` pattern), a method+path router with `:param` captures, `Req`/`Resp`
  records with builder helpers, and the no-function-values handler model —
  `Handler` / `Middleware` structural interfaces dispatched by ICALL, with
  WO-E205 making a non-conforming handler a compile error and `?Resp`
  middleware short-circuiting on the shipped `?T` narrowing.
- **The consuming app**, `docs/examples/web-app/`: a small storefront
  (`Product`/`Order` as `@table` classes, list/show/create routes, one auth
  middleware, JSON responses) that imports the framework **through
  `[deps]`** — the sample exercises the whole chain: fetch → lock → build →
  serve → durable data across restart.
- **Honest limits stated where users read them**: single-threaded blocking
  (concurrency arrives underneath via iterations 8/11), no chunked encoding,
  no WebSockets, JSON-first (no templates — the removed UI track stays
  removed).
- **Relationship to iteration 10 recorded in both**: `service` blocks later
  *lower onto this library* — compiler sugar over the same router, never a
  rival stack.

## Acceptance Criteria

- **Given** the web-app sample, **when** `just web-app` runs, **then** deps
  fetch, the app builds, serves list/show/create with `@table` persistence
  across a process restart, answers 404/400 correctly, a deliberately
  trapping handler returns 500 while the server survives, and SIGTERM stops
  it cleanly — fd- and resident-flat under the opt-in soak.
- **Given** a handler class missing `handle`, **when** the app compiles,
  **then** WO-E205 names the class and interface.
- **Given** nginx in front (documented config), **when** a browser hits it,
  **then** the browser negotiates h2 with the proxy while the backend link is
  HTTP/1.1 — proving the TLS/h2 story without TLS in writeonce.
- **Given** the framework extracted to its own repository, **when** the app's
  `[deps]` URL is updated, **then** nothing else changes (success criterion 4
  of the spec).

## Out Of Scope

TLS in the toolchain (proxy-terminated by decision); HTTP/2 + the
bytes/buffer type (parked to the h2c successor, after 8/9f/11); chunked
transfer encoding; WebSockets/SSE; templates/SSR; multipart uploads;
performance work beyond the soak's flatness gate (benchmarks belong to 9e's
measurement backbone).

## Proposed Solution

Framework modules `http/`, `router/`, `app.wo` as spec §B lays out; the
web-app acceptance script is the gate (curl matrix + restart + stop + soak);
`just web-app` wires fetch-build-serve-verify into one command. Plan follows
after this iteration is approved on the board.
