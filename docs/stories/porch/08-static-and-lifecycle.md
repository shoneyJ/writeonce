---
track: porch
iteration: "8"
status: pending
readiness: ready
---

# porch 8 — static files, lifecycle hooks, and the small middleware everyone ships

> Part of [Story — `porch`, the writeonce web framework](00-story.md).
> Source: [the Fiber parity study](../../plan/exploration/fiber/00-fiber-parity.md) §3, §5,
> re-checked 2026-09-06 against `.dev/reference/fiber` (v3, `3ca9a9d`) and the
> existing `http/files.wo`.
> The static half needs [iteration 6](06-streaming-core.md) and iteration
> [5](05-routing-response-ergonomics.md)'s `Download` helper; the `Last-Modified`
> half needs **one small language-track builtin** (`time.utc`, decision 4). The
> rest is pure `.wo`. Not exposed to the lang-41 hang.
>
> The clean-up iteration, and the last in the track. Individually every item is
> small; together they are most of what makes a framework feel finished rather
> than adequate.

## Goals

- **Static files that can serve something large.** `StaticFiles` exists and is
  already careful — traversal is refused, `max_bytes` is a hard ceiling, and the
  site's `/dl` downloads run through it — but it reads the whole file with
  `fs.read_all` and so cannot serve a file it cannot hold in memory, resume a
  partial download, or let a browser cache correctly. Byte ranges are what make
  video and large downloads work at all.
- **Cache headers that let a client skip the request.** `etag_for`/`with_etag`
  give conditional GETs already; `Cache-Control`, `Last-Modified` and
  `If-Modified-Since` are the other half, and `fs.stat` already returns the mtime
  they need.
- **Lifecycle hooks.** The ledger records "no user teardown hooks yet". porch
  needs a small handful; the shutdown one is the one that matters, because an app
  with its own resources currently has nowhere to close them.
- **The small middleware every framework ships**: healthcheck, favicon,
  redirect, rewrite, and a `skip` combinator — each a handful of lines whose
  absence is felt immediately.

## Decisions locked (brainstorm 2026-09-06)

1. **Three lifecycle hooks: on-listen, on-shutdown, on-route-registered.** Hooks
   are classes, like everything else. on-shutdown is the essential one, wired
   into `env.stopping()`, and runs exactly once even on a trapping path.
   on-listen runs app code after the port binds (post-bind warmup, a readiness
   signal). on-route-registered fires at registration time for plugins and
   logging — it composes with iteration 5's route introspection rather than
   duplicating it (a fire-at-registration hook versus a query of the table, two
   different timings).
2. **The healthcheck ships both liveness and readiness as distinct endpoints.**
   `/livez` is a static 200 (the process is up, do not kill me); `/readyz`
   consults an app-supplied readiness predicate (DB reachable, warmup done) and
   answers 503 until ready (do not send traffic yet). Conflating them is why
   deployments flap; shipping one endpoint that is silently only one of the two
   is the trap.
3. **Directory listing ships, off by default, documented.** Index resolution is
   always on (a directory resolves to `index.html` and friends). A listing is
   available behind a config flag defaulting off, with the doc stating plainly
   what it exposes — the alternative is every app hand-rolling a worse, leakier
   one. Traversal refusal is re-proven against both the index-resolution and
   listing paths, which is exactly where traversal creeps back in.
4. **`Last-Modified` uses a new small `time.utc(ms) -> TimeParts` builtin.**
   `time.local` is localtime (wrong zone for an HTTP-date) and `time.iso` is UTC
   but ISO format (wrong shape). `time.utc` is the gmtime sibling of `time.local`
   — the same `TimeParts` record (including `dow`) but in UTC — a tiny,
   broadly-reusable builtin that fills a genuine gap (any GMT timestamp or log
   line wants it). HTTP-date formatting is then pure `.wo` (static day/month name
   tables plus the record fields). `If-Modified-Since` is handled by
   string-equality against the formatted `Last-Modified` — the echo-back flow
   clients actually use — so no HTTP-date *parser* is needed. This is the track's
   third and smallest language touch, after iteration 2's `random_bytes` and
   iteration 7's `deflate`/`crc32`.

## Phases

### Phase A — ranges, cache headers, and the `time.utc` builtin

- Add `time.utc(ms) -> TimeParts` (decision 4): a module builtin at the next free
  `wob.h` id (re-check — the runtime-v2 track has been consuming them),
  `gmtime_r`-based, mirroring `time.local`'s implementation. Document it in the
  builtin-surface contract in the same change.
- `Range` request parsing (single range first; multi-range can wait),
  `206 Partial Content`, `Content-Range`, and `Accept-Ranges`. An unsatisfiable
  range is `416`, not a truncated `200`.
- `Last-Modified` from `fs.stat`'s mtime formatted with `time.utc`,
  `If-Modified-Since` by string-equality, and `Cache-Control` with a configurable
  max-age.
- Serve the body through iteration 6's writer with `fs.read_at`, so file size
  stops bounding what can be served (retiring the `fs.read_all` whole-file read).
- Verify: a ranged request returns exactly the requested bytes; a file much
  larger than the arena serves; a conditional request returns 304 with no body.

### Phase B — directory behaviour

- Index-file resolution and an optional directory listing defaulting off
  (decision 3).
- `Download`/`Attachment` disposition, reusing the helper iteration 5 added.
- Re-verify the traversal refusal against every new path.
- Verify: index resolution works; listing is off unless asked for; traversal is
  still refused on all new paths.

### Phase C — lifecycle hooks

- The three hook interfaces (decision 1). Wire the shutdown hook into the
  existing `env.stopping()` path so an app can flush and close before the process
  exits, guaranteed to run exactly once even on a trapping path.
- Verify: the shutdown hook fires on SIGTERM before the listener closes, once; a
  trapping hook does not prevent shutdown; on-listen fires after bind; the
  on-route-registered hook fires per registration.

### Phase D — the small middleware set

- Healthcheck (both endpoints, decision 2), favicon, redirect (permanent and
  temporary), rewrite (internal, no round trip), and `skip` wrapping another
  middleware with a predicate.
- Verify: each behaves; `skip` composes with the existing chain in registration
  order; `/livez` and `/readyz` answer independently.

### Phase E — the gate and the ledger

- Both serving gates. The site is the natural subject: it already serves
  `/favicon.svg`, `/health`, and `/dl` downloads through `StaticFiles`.
- Close out the porch track's ledger rows and record what the whole track
  actually landed versus what the Fiber study predicted.
- Verify: `just web-app`, `just site`, `just linkcheck` green.

## Acceptance Criteria

- **Given** a `Range: bytes=a-b` request, **when** it is served, **then** the
  response is `206` with exactly those bytes and a correct `Content-Range`.
- **Given** an unsatisfiable range, **when** it is served, **then** the response
  is `416`, never a truncated `200`.
- **Given** a file larger than the heap, **when** it is requested, **then** it
  serves completely and peak memory does not track file size.
- **Given** `If-Modified-Since` equal to the file's formatted `Last-Modified`,
  **when** the request arrives, **then** the response is `304` with no body.
- **Given** a directory with an index file, **when** the directory is requested,
  **then** the index is served; **and** with no index and listing disabled, the
  response is `404`, not a listing.
- **Given** a traversal attempt through the index-resolution and listing paths,
  **when** it is served, **then** it is refused.
- **Given** a registered shutdown hook, **when** the process receives SIGTERM,
  **then** the hook runs exactly once before the listener closes, and in-flight
  requests still complete.
- **Given** a hook that traps, **when** shutdown runs, **then** shutdown still
  completes.
- **Given** `/livez` and `/readyz`, **when** the app is up but not ready,
  **then** `/livez` is 200 and `/readyz` is 503.
- **Given** `skip` wrapping a middleware with a predicate, **when** the predicate
  matches, **then** the wrapped middleware does not run and the chain continues
  in order.

## Out Of Scope

- **Multi-range requests.** A multipart byte-range response is a separate format;
  single ranges cover downloads and media seeking.
- **Precompressed asset serving** (`file.gz` beside `file`). Composes with
  iteration [7](07-sse-and-compression.md); worth doing once, later, when both
  exist.
- **A file-watching or hot-reload story.** Caching with invalidation is language
  iteration [18](../language-runtime-database/18-memory-db-features.md).
- **`pprof`, `expvar`, metrics endpoints.** Language iteration 30. A healthcheck
  is one bit, not observability.
- **Fiber's fork, mount and prefork hooks.** The shard runtime owns placement;
  there is no worker-pool to hook.
- **`SendFile` with kernel `sendfile(2)`.** No such builtin exists and no
  iteration owns adding one; the streaming writer is the portable answer.
- **An HTTP-date *parser*.** `If-Modified-Since` is matched by string-equality
  against the formatted `Last-Modified` (decision 4); parsing arbitrary
  HTTP-date shapes is not needed for the echo-back flow.

## Info

The one language dependency is small: `time.utc`, a `gmtime_r` sibling of the
existing `time.local`. Everything else is pure `.wo` — `fs.read_at` (id 44)
plus iteration 6's writer for ranges and large files, `fs.stat` (id 42) for
mtime, `env.stopping()` (id 50) for the shutdown hook, and the small middleware.
None of it touches actors or the per-key pool, so, like the rest of the track
since iteration 1, it is not exposed to the lang-41 hang.

With this iteration ready, the whole porch track (2–8) is brainstormed and
locked. The three language touches the track needs are now explicit and small:
`random_bytes` (2), `deflate`/`crc32` (7), and `time.utc` (8) — each a builtin
with a named consumer, none of them a primitive shipped as decoration.
