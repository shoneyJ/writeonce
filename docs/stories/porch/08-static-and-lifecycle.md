---
track: porch
iteration: "8"
status: pending
readiness: refine
---

# porch 8 — static files, lifecycle hooks, and the small middleware everyone ships

> Part of [Story — `porch`, the writeonce web framework](00-story.md).
> Source: [the Fiber parity study](../../plan/exploration/fiber/00-fiber-parity.md) §3, §5.
> The static half needs [iteration 6](06-streaming-core.md); the rest does not.
>
> The clean-up iteration. Individually every item is small; together they are
> most of what makes a framework feel finished rather than adequate.

## Goals

- **Static files that can serve something large.** `StaticFiles` exists and is
  already careful — traversal is refused rather than normalised, `max_bytes` is a
  hard ceiling, and the site's `/dl` downloads run through it. What it cannot do
  is serve a file it cannot hold in memory, resume a partial download, or let a
  browser cache correctly. Fiber's static ships `ByteRange`, `MaxAge`,
  `CacheDuration`, `IndexNames`, `Browse` and `Download`; the range support is
  what makes video and large downloads work at all.
- **Cache headers that let a client skip the request.** `etag_for`/`with_etag`
  give conditional GETs; `Cache-Control`, `Last-Modified` and `If-Modified-Since`
  are the other half, and `fs.stat` already returns the mtime they need.
- **Lifecycle hooks.** The ledger records "no user teardown hooks yet" as a known
  gap. Fiber has eleven hook families; porch needs a small handful — on-listen,
  on-shutdown, and on-route-registered — and the shutdown one is the one that
  matters, because an app with its own resources currently has nowhere to close
  them.
- **The small middleware every framework ships**: healthcheck, favicon,
  redirect, rewrite, and a `skip` combinator. Each is a handful of lines and
  their absence is felt immediately by anyone starting a new app.

## Phases

### Phase A — ranges and cache headers

- `Range` request parsing (single range first; multi-range is a multipart
  response and can wait), `206 Partial Content`, `Content-Range`, and
  `Accept-Ranges`. An unsatisfiable range is `416`, not a truncated `200`.
- `Last-Modified` from `fs.stat`'s mtime, `If-Modified-Since` handling, and
  `Cache-Control` with a configurable max-age.
- Serve the body through iteration 6's writer so file size stops bounding what
  can be served.
- Verify: a ranged request returns exactly the requested bytes; a file much
  larger than the arena serves; a conditional request returns 304 with no body.

### Phase B — directory behaviour

- Index-file resolution (`index.html` and friends) and an optional directory
  listing, defaulting **off** — a listing that is on by default is an
  information leak the first time someone points it at the wrong directory.
- `Download`/`Attachment` disposition, reusing the helper iteration
  [5](05-routing-response-ergonomics.md) added.
- Re-verify the traversal refusal against every new path (index resolution and
  listing both construct paths, which is exactly where traversal creeps back
  in).
- Verify: index resolution works; listing is off unless asked for; traversal is
  still refused on all new paths.

### Phase C — lifecycle hooks

- Decide the minimal set (fork 1) and the interface — hooks are classes, like
  everything else here.
- Wire the shutdown hook into the existing `env.stopping()` path so an app can
  flush and close before the process exits, and guarantee it runs exactly once
  even on a trapping path.
- Verify: the shutdown hook fires on SIGTERM before the listener closes, once;
  a trapping hook does not prevent shutdown.

### Phase D — the small middleware set

- Healthcheck (liveness and readiness are different questions — say which),
  favicon, redirect (permanent and temporary), rewrite (internal, no round
  trip), and `skip` wrapping another middleware with a predicate.
- Verify: each behaves; `skip` composes with the existing chain in registration
  order.

### Phase E — the gate and the ledger

- Both serving gates. The site is the natural subject: it already serves
  `/favicon.svg`, `/health`, and `/dl` downloads through `StaticFiles`, so these
  features have a real consumer rather than a synthetic one.
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
- **Given** `If-Modified-Since` matching the file's mtime, **when** the request
  arrives, **then** the response is `304` with no body.
- **Given** a directory with an index file, **when** the directory is requested,
  **then** the index is served; **and** with no index and listing disabled, the
  response is `404`, not a listing.
- **Given** a traversal attempt through the index-resolution and listing paths,
  **when** it is served, **then** it is refused — the existing guarantee, re-proven
  against the new code paths.
- **Given** a registered shutdown hook, **when** the process receives SIGTERM,
  **then** the hook runs exactly once before the listener closes, and in-flight
  requests still complete.
- **Given** a hook that traps, **when** shutdown runs, **then** shutdown still
  completes.
- **Given** `skip` wrapping a middleware with a predicate, **when** the
  predicate matches, **then** the wrapped middleware does not run and the chain
  continues in order.

## Out Of Scope

- **Multi-range requests.** A multipart byte-range response is a separate
  format; single ranges cover downloads and media seeking, which is what the
  workload needs.
- **Precompressed asset serving** (`file.gz` beside `file`). Composes with
  iteration [7](07-sse-and-compression.md); worth doing once, later, when both
  exist.
- **A file-watching or hot-reload story.** Assets are read from disk per
  request; a cache with invalidation is a different feature and language
  iteration [18](../language-runtime-database/18-memory-db-features.md) owns
  caching.
- **`pprof`, `expvar`, metrics endpoints.** Language iteration 30 (no story file
  yet). A healthcheck is not observability — it is one bit.
- **Fiber's fork, mount and prefork hooks.** The shard runtime owns placement;
  there is no worker-pool to hook.
- **`SendFile` with kernel `sendfile(2)`.** No such builtin exists and no
  iteration owns adding one; the streaming writer is the portable answer here.

## Info

Forks the spec must settle:

1. **Which hooks, exactly?** Fiber has eleven families and porch needs the
   fewest that are load-bearing. On-shutdown is clearly one — an app with open
   resources has nowhere to close them today. On-listen is convenient for a
   startup banner. On-route-registered is only useful for introspection, which
   iteration [5](05-routing-response-ergonomics.md) may already cover. Fewer is
   better; each hook is a contract forever.
2. **Liveness or readiness for healthcheck?** They answer different questions
   and conflating them is why deployments flap: liveness says "do not kill me",
   readiness says "do not send me traffic yet". A framework shipping one
   endpoint called `/health` should say which it is, and probably ships both.
3. **Is directory listing available at all?** Off-by-default is not the same as
   present-but-off. Shipping it at all means it will eventually be switched on
   somewhere it should not be. Leaning: ship it, off, with the doc saying
   plainly what it exposes — the alternative is every app hand-rolling a worse
   one.
