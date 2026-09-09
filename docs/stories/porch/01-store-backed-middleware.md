---
track: porch
iteration: "1"
status: done
readiness: ready
---

# porch 1 — store-backed middleware: rate limiting

> Part of [Story — `porch`, the writeonce web framework](00-story.md).
> Source: [the Fiber parity study](../../plan/exploration/fiber/00-fiber-parity.md) §2.
> Spec: [`2026-08-29-porch-store-backed-middleware-design.md`](../../superpowers/specs/2026-08-29-porch-store-backed-middleware-design.md).
>
> **Re-scoped 2026-08-30 — this iteration is now RATE LIMITING ONLY.**
> Idempotency was built, reviewed and reverted; it moved to
> [porch 9](09-idempotent-replay.md); the C-runtime crash that blocked it
> ([language 41](../language-runtime-database/41-actor-arena-crash.md)) was
> fixed 2026-09-09, so porch 9 is now buildable.
> The split was made because the limiter is provably stable (five consecutive
> gate runs, 56 checks, 0 failures) while idempotency's gate flaked on a
> runtime defect — and a feature whose test passes some of the time is not
> shipped. Detail in History.
>
> **Rewritten 2026-08-29** after the brainstorm settled every fork. The
> iteration's premise changed: it was scoped as the cheapest slice because it
> needed "only a `@table` and `time.ticks`", and it now serializes through an
> actor pool. That is not new runtime surface — `spawn`, `send`, `call`,
> `monitor` and `time.after` all landed with the actor-lifecycle work — but it
> is more than the original framing, and the reason is in History below.
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

## The design, as settled

One rule, inherited by porch 2 (sessions) and 3 (CSRF):

> **Serialize through an actor. Persist in a `@table`. Never read-modify-write
> from a handler fiber.**

| Concern | Owner |
| --- | --- |
| per-key ordering, in-flight ownership, waiter lists | a sharded pool of actors, selected by hash of the key |
| counters, stored responses, request digests | `@table` rows — WAL-durable, replayed at boot |
| deciding whether a request passes | the actor, never the handler fiber |

The read-modify-write is the defect both features shared: a handler reads a
count, adds one and writes it back, so two interleaved fibers lose an
increment. An actor processes one message at a time, so routing both features
through the pool buys per-key serialization with no locks and no polling. A
pool rather than one actor because ordering is needed *per key*, never
globally.

## Progress

| Phase | State |
| --- | --- |
| A — store convention | ✅ `519d411`, `3a9bddc` — two purpose-shaped tables plus the digest column. `IdempotencyKey` is retained but unused; the file says why |
| B — rate limiter | ✅ `676e651`, `153fd29` (the pool), `a653dd0`, `831e9d8` (the limiter). Exact counting under 30 parallel clients, restart-durable, `trust_proxy` off by default |
| C — idempotency | ⏸ **moved to [porch 9](09-idempotent-replay.md).** Built and reviewed (`eae1b06`…`9ad5947`, plus the final wave), then reverted — see History |
| D — gate and ledger | ✅ `21934b1`, `2ac1b8b`, and the re-scope commit. 56 checks, 0 failures, stable across five consecutive runs |

**What the split bought.** Before it the suite reported anywhere from 0 to 6
failures run to run; after it, five consecutive runs at 56/0. The limiter was
finished either way — it was being held hostage by a defect in code it does not
call.

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
  **Met** — `scripts/web-app-accept.sh` §17a: 5 requests pass, the 6th is 429,
  carrying `Retry-After` and `X-RateLimit-Remaining: 0`.
- **Given** counters at their limit, **when** the process is SIGTERMed and
  restarted, **then** the client is still limited — the counters replayed from
  the WAL rather than resetting to zero. **Met** — §17b: same client, same
  key, still 429 after a restart against the same `WO_DATA`.
- **Given** a window that has fully elapsed, **when** the same client returns,
  **then** it is served, and the expired row is pruned on that access. **Met
  by construction, not gate-exercised** — `keypool.wo`'s kind-1 arm deletes the
  stale row and inserts a fresh count-1 row once `now - row.window >
  msg.window`; no gate leg waits out a full window (the keypool leg's own
  window is 60s) to observe it end to end.
- **Given** the system clock jumping backwards, **when** the window is
  evaluated, **then** no extra allowance is granted (`time.ticks` is monotonic).
  **Met by construction, not gate-exercised** — the counting path reads only
  `time.ticks()`, never `time.now()`; `time.now()` feeds only the advisory
  `reset_at`/`X-RateLimit-Reset` value, never the count itself. No gate leg
  fakes a backward clock jump.
- **Given** a POST with an idempotency key that has been seen, **when** it is
  replayed, **then** the stored response is returned byte-identically and the
  handler's side effect count is unchanged — proven by a row count, not by a
  log line. **Met** — §18a: same key + same body both 200, byte-identical
  bodies, `ExecMark` row count stays 1.
- **Given** a reused idempotency key with a different request body, **when** it
  arrives, **then** it is refused rather than answered with the other request's
  response. **Met** — §18b: 200 then 422, the 422 body is the refusal (never
  the first response), the refused request never ran the handler.
- **Given** two identical keyed requests in flight at once, **when** both are
  dispatched, **then** exactly one executes and the other receives that one's
  stored response — never a refusal, never a partial write. *(Restated
  2026-08-29: this used to permit 409. Blocking supersedes it — the duplicate
  parks on `call` until the owner reports.)* **Met** — §18c: two genuinely
  concurrent duplicates both answer 200 with identical bytes, overlap timing
  proves genuine concurrency, and `ExecMark` shows exactly one execution.
- **Given** N concurrent requests for one limiter key, **when** they are
  counted, **then** the total is exactly N and no increment is lost. *(Added:
  unreachable before the pool, and the defect that most undermines a limiter.)*
  **Met** — §17c: 30 genuinely parallel requests, the count afterward is exact.
- **Given** a saturated actor pool, **when** a request arrives, **then** it is
  refused with 503 rather than served uncounted. *(Added: fail-closed, because
  saturating the pool must not become the limiter's bypass.)* **Met** — §19: a
  one-actor pool with `WO_MAILBOX` shrunk to 2, 15 concurrent requests, exactly
  3 served (1 running + 2 queued) and 12 answer 503, each 503 carrying
  `Retry-After` and naming the real cause; the `SatMark` execution count
  matches the 200 count exactly — no overflow request ran uncounted.

Seven of nine criteria are gate-proven end to end
(§17a/§17b/§17c/§18a/§18b/§18c/§19). The remaining two — window-elapse pruning
and clock-monotonicity — are implemented and hold by construction and code
inspection; neither was gated even in the original phase plan below, and
gating them (a real wait-out-a-window run, a faked backward clock) is future
work, not this task's. The concurrency legs needed genuine parallelism: a test
that cannot fail before the fix is not a test, and `scripts/web-app-accept.sh`
§17c/§18c/§19 all use backgrounded, concurrently-launched clients rather than
a sequential loop.

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
  [databasev2 9](../databasev2/09-cross-program-tables.md).
- **A background expiry sweeper.** Lazy pruning on access, deliberately —
  porch has no scheduler. **Corrected 2026-08-29**: this used to say "no timer
  exists (`time.after` is still a reserved builtin id)". That is false —
  `time.after` is builtin 90 and implemented (`runtime/src/builtin.c`, via
  `wo_vm_timer_after`), along with `spawn` (68), `send` (69), `call` (88) and
  `monitor` (89). The exclusion stands on its own merits: `time.after` is a
  one-shot timer aimed at an actor, not a recurring sweep. But the *reason*
  given was wrong, and it is the claim that made this iteration look cheaper
  than it is.
- **The TTL cache middleware** — language
  [iteration 18](../language-runtime-database/18-memory-db-features.md) owns it,
  spec already approved. Do not build a second cache here.

## Info

**All three settled 2026-08-29** — kept with their outcomes rather than
deleted, so the reasoning survives:

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

Nothing here needs a new runtime primitive — still true, and now verified
rather than assumed: `call` is "a send that WAITS", whose park/reply protocol
lives in the VM, and that is exactly the blocking primitive the design needs.

**Outcomes:**

1. **Two stores**, as leaned. Confirmed by construction in `519d411`; a
   generic table would have forced a counter and a response body through the
   same `Text` column.
2. **The peer address, unless the app declares otherwise.** A `trust_proxy`
   flag defaulting to off: off keys on `net.peer(req.conn)`, which cannot be
   forged; on keys on the left-most `X-Forwarded-For` entry. Only the deployer
   knows the topology, so the declaration belongs in their code. The built
   version branched on `req.ctx["verified_proxy"]`, which nothing anywhere
   sets — a dead branch. Verifying the proxy is genuinely story 35's, and
   `client_ip` says so in its own comment.
3. **`content-type` only.** Settled as built, and settled correctly: replaying
   a stored `Set-Cookie` or a stale `Date` is wrong, and cookies arrive in
   iteration 2, so this is cheap now and expensive later.

## History

**2026-08-30 — Task 5: the saturation leg, and closing out.** The gate now
proves fail-closed saturation (§19 of `scripts/web-app-accept.sh`): a one-actor
pool, `WO_MAILBOX` shrunk to 2, 15 genuinely concurrent requests — exactly 3
served (1 running + 2 queued) and 12 answer 503, retry-after set, the real
cause named, and the execution count matches the 200 count exactly. Also
fixed while wiring pool size: `make_pool(n)` with `n < 1` was a mod-by-zero in
`pool_select`; guarding it in `pool_select` alone would not have helped —
every `pool_select` call runs inside the middleware's own `try ... catch (e)
nil`, so the trap would have been swallowed and misreported as ordinary 503
saturation forever. `make_pool` now clamps `n < 1` to 1.

Four things discovered building Tasks 2–4, not in the original design, now
recorded in `docs/examples/porch/README.md` (not only here, since anyone
wiring this into a real app needs them): `Idempotent` is a `Handler`
decorator, not a `Middleware`; `Pool` cannot live in actor state or a message
(WO-E222) and must be re-wrapped from a bare `actor PoolMsg` handle per use;
a `call` reply must be a copyable scalar (WO-E226), which is why the response
travels through the `@table`; and pool size is a capacity decision — a
saturated pool fails closed with 503, never a silent bypass.

**Runtime defects found during this work — C runtime, not porch bugs:**

- `try EXPR catch (e) nil` cannot distinguish a literal `Int 0` reply from a
  trap. Worked around by never packing a zero outcome code (`pool_pack` in
  `middleware/keypool.wo`).
- A `Text`/map value read off `json.decode(...) as T` is corrupted once
  embedded in a struct crossing a function-return boundary. Worked around by
  forcing fresh text with `.. ""` on every field copied out of a decoded
  record (`idempotent.wo`'s replay path).
- Under concurrent `call()`-parked callers doing real per-request table I/O,
  `main()` returns cleanly but the OS process sometimes hangs (~1-in-5); an
  aggressive variant produced a segfault. Reproduces more readily at higher
  sequential insert+delete volume against the same key (N=4/5 crashed; N=1–3
  clean over 12+ trials). Both `idempotent-check`'s own SIGTERM leg (§18) and
  the new saturation leg (§19) — the sharpest reproducer yet, by design — hit
  this; both contain it with an unconditional `kill -9` fallback rather than
  asserting graceful shutdown, so it cannot flake a leg whose actual subject
  is something else. Root-causing this is C-runtime work, out of scope here.

**2026-08-29 — Phases B and C superseded before review.** Both were built
against the original framing and both store the response *after* the handler
returns. Three of the seven original criteria cannot hold in that shape, which
is why this is a rebuild rather than a patch:

- **In-flight collision was undetectable.** `before` finds nothing and passes;
  the row appears in `after`, once the handler has already run. Concurrent
  duplicates both miss and both execute — there is no reservation to collide
  on.
- **The 10-second in-flight heuristic was inverted.** The timestamp is stamped
  when the response is *stored*, not when the request *starts*, so it fired on
  legitimate fast replays — the common case — answering 409 where the stored
  response was owed, and could never fire on a genuinely concurrent request.
- **"Reused key, different body is refused" was unreachable.** The body digest
  was folded into the lookup key, so a different body was a different key and
  simply missed. Safe — the wrong response is never served — but nothing looks
  the bare key up, so nothing can refuse. The digest becomes a column.

Two defects independent of the redesign, both since fixed or scheduled: the
limiter emitted a monotonic tick into `X-RateLimit-Reset`, where a client
expects a Unix timestamp; and it used delete-then-insert where assigning to a
row field writes through (`compiler/src/emit.ml`), which doubled WAL traffic
and left a window where a failed insert after a successful delete silently lost
the counter — handing out a free window, the exact inverse of the durability
this iteration exists to demonstrate.
