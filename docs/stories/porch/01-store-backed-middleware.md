---
track: porch
iteration: "1"
status: refine
---

# porch 1 — store-backed middleware: rate limiting and idempotency

> Part of [Story — `porch`, the writeonce web framework](00-story.md).
> Source: [the Fiber parity study](../../plan/exploration/fiber/00-fiber-parity.md) §2.
>
> **First deliberately because it is the cheapest.** Both features need only a
> `@table` and `time.ticks`, both of which already exist — no new builtin, no
> cookie, no change to `Resp`. It exists to prove the store pattern and the gate
> shape on low-risk work before iterations 2–4 touch the runtime and the public
> response type.

## Goals

- **A rate limiter that survives a restart.** Fixed-window counting keyed by
  client, answering 429 with the conventional headers when the window is spent.
  Fiber's `limiter` keeps counters in memory by default and expects Redis for
  anything real; porch's live in a `@table`, so they are WAL-durable and
  crash-recoverable for free. That is the difference worth demonstrating, and it
  is why the acceptance criteria include a restart.
- **Idempotent replay of unsafe requests.** A client resending a POST with the
  same idempotency key gets the stored response and the handler does not run
  twice. This is the correctness feature the storefront sample has silently
  needed since it grew a checkout.
- **Establish the store convention for iterations 2–4.** Sessions and CSRF will
  want the same shape. Decide it once, here, on the cheap slice.

## Phases

### Phase A — the store convention

- Decide the store shape (see Info fork 1) and write it down before any
  middleware exists, because three later iterations inherit it.
- Add the `@table` classes for a counter row and a stored-response row, with
  the secondary indexes their lookups need — the read path is an equality
  probe, which is the only index shape the engine has.
- Decide and document the expiry discipline: rows are pruned lazily on access,
  not by a background sweeper, because porch has no timer and iteration 30 owns
  scheduled work.
- Verify: `woc docs/examples/porch/` typechecks entry-less as a library; the
  new tables appear in the WAL and replay across a restart.

### Phase B — the rate limiter

- A `Limiter` middleware class with a `before` that counts and either passes or
  short-circuits with 429 — the `?Resp` short-circuit the chain already has.
- Key selection: reuse `client_ip(req)` and the existing trusted-proxy
  judgement rather than inventing a second one. A keyed-by-principal variant
  falls out for free once `req.principal` is set by an auth middleware.
- The response headers on both paths, and a `Retry-After` on the refusal.
- Window arithmetic on `time.ticks` (µs monotonic), not `time.now` — a
  wall-clock jump must not hand out a free window.
- Verify: a burst crosses the threshold at exactly N, the window rolls, the
  counters survive `SIGTERM` + restart.

### Phase C — idempotency

- An `Idempotent` middleware pair: `before` looks the key up and replays a hit;
  `after` stores the response for a miss. This is the first real user of the
  `after` chain for something other than headers, which is worth noting.
- Decide what is part of the identity: the key header alone, or key plus a
  digest of method+path+body (`sha256` exists). Replaying a stored response for
  a *different* body under a reused key is the failure mode that matters.
- In-flight collision handling: a second request arriving while the first is
  still running. Fiber takes a lock; porch's shard model means the honest
  answer is probably to refuse with 409 rather than to block.
- Verify: replay returns the stored response, the handler's side effect happens
  exactly once, a reused key with a different body is refused, concurrent
  duplicates do not both execute.

### Phase D — the gate and the ledger

- Extend `scripts/web-app-accept.sh` with the checks above, including the
  restart leg — a durability claim that no gate exercises is not a claim.
- Update porch's README ledger rows for both features, and record in the
  status board what landed versus what was planned.
- Verify: `just web-app` and `just site` both green; `just linkcheck` clean.

## Acceptance Criteria

- **Given** a limiter of N requests per window, **when** a client sends N+1,
  **then** the first N succeed and the last is 429 with `Retry-After` set.
- **Given** counters at their limit, **when** the process is SIGTERMed and
  restarted, **then** the client is still limited — the counters replayed from
  the WAL rather than resetting to zero.
- **Given** a window that has fully elapsed, **when** the same client returns,
  **then** it is served, and the expired row is pruned on that access.
- **Given** the system clock jumping backwards, **when** the window is
  evaluated, **then** no extra allowance is granted (`time.ticks` is monotonic).
- **Given** a POST with an idempotency key that has been seen, **when** it is
  replayed, **then** the stored response is returned byte-identically and the
  handler's side effect count is unchanged — proven by a row count, not by a
  log line.
- **Given** a reused idempotency key with a different request body, **when** it
  arrives, **then** it is refused rather than answered with the other request's
  response.
- **Given** two identical keyed requests in flight at once, **when** both are
  dispatched, **then** exactly one executes and the other gets the decided
  answer (replay or 409), never a partial write.

## Out Of Scope

- **A pluggable `Storage` interface.** Fiber abstracts it so one middleware runs
  on memory or Redis. porch has one store, and interfaces here are structural —
  an abstraction with exactly one implementor is decoration, which iteration 37
  learned the hard way about `Component`. Concrete `@table` until a second
  backend actually exists.
- **Sliding-window or token-bucket algorithms.** Fixed window is what the
  sample needs; a better algorithm is a later slice with a measurement behind
  it.
- **Distributed limiting across processes.** One program owns its database;
  cross-program state is language
  [iteration 20](../language-runtime-database/20-cross-program-tables.md).
- **A background expiry sweeper.** No timer exists (`time.after` is still a
  reserved builtin id in `wob.h`). Lazy pruning on access, deliberately.
- **The TTL cache middleware** — language
  [iteration 18](../language-runtime-database/18-memory-db-features.md) owns it,
  spec already approved. Do not build a second cache here.

## Info

Forks the spec must settle:

1. **One store or two?** A single generic key/value/expiry table serving both
   features, or a purpose-shaped table each. Leaning two: the columns genuinely
   differ (a counter is an Int, a stored response is status + headers + body),
   and a generic table would force everything through `Text`, which is how the
   framework's cache ended up storing JSON strings.
2. **What is the limiter's key when there is no auth?** `client_ip(req)` reads
   `X-Forwarded-For`, which a direct client can forge. Behind the mandated TLS
   proxy that is fine; on an open port it is not. `net.peer(fd)` gives the real
   peer — decide which is authoritative and reuse whatever the trusted-proxy
   slice concludes rather than deciding twice.
3. **Does idempotency store headers?** Fiber has `KeepResponseHeaders` because
   replaying `Set-Cookie` or a fresh `Date` is usually wrong. porch has no
   cookies yet (iteration 2), so this is cheap to decide now and expensive to
   retrofit later.

Nothing here needs a new runtime primitive, which is the point of going first.
