---
iteration: "18"
status: hold
---

# Iteration 18 — framework v2: memory-rich features over the embedded database

> **Scope label (2026-08-20): this iteration is FRAMEWORK V2.** Framework
> v1 is the transport/routing/body/security surface tracked in the
> [framework README's status ledger](../../../examples/writeonce-framework/README.md);
> v2 is what the embedded store adds on top. v1 gaps land before or
> alongside v2 as slices, per the ledger.

> Format: fiberloom `product/story-iteration-template`. Part of
> [Story — one language, one runtime, one database, one binary](../00-story.md).
>
> **Inserted 2026-08-20, forks settled the same day** (developer decisions
> below). Next step: spec + plan, implementation on approval — the
> iteration-17 discipline.

## Why this iteration exists

The single binary owns its memory AND its durable store — the class of
features other stacks buy with Redis, a message broker, and an outbox
pattern falls out of what is already here: an in-process map needs no
serialization format or cache-invalidation protocol (one process, one
node); a `@table` is a durable queue with WAL recovery for free; and the
WAL already stages multi-write batches internally (`wal_append*` →
`wal_commit`) — `db.c` merely commits per statement today, so exposing the
batch as a language transaction makes "enqueue a job and write the order
in ONE commit" true, which is the outbox problem dissolved rather than
worked around.

## Ground truths the design stands on (verified 2026-08-20)

- `time.now` exists (Timestamp) — TTL/expiry is expressible; there are no
  timers, so expiry must piggyback on requests (lazy), never on a clock.
- The runtime is single-threaded and `net.accept` blocks without a
  timeout: an IDLE server executes nothing. Any "background" work runs
  only when requests give it time — disclosed, not hidden.
- Long-lived state lives on the instances the app wires (`App`,
  middleware, handler fields) — they survive across requests; GC (7b)
  handles the churn. No globals needed, none exist.
- WAL commit is per statement in `db.c`; the staged-batch machinery
  beneath it is already transactional in shape (RAM apply → append →
  commit=write+fdatasync).

## Settled decisions (2026-08-20)

1. **Scope: TTL cache, feature flags, background jobs.** Pub/sub for
   WebSockets is REJECTED for now — WebSockets do not exist, and
   long-lived connections on a single-threaded accept loop is the
   iteration-16 starvation lesson magnified; both wait for shards/fibers
   (8/11). A channel data structure without delivery is mechanism without
   a consumer.
2. **Jobs execution model: drain-on-request.** A bounded job budget runs
   after each served response, in-process, adjacent to the app's own
   writes. Honest limit stated everywhere it matters: an idle server
   drains nothing until the next request arrives. Fibers (11) later
   replaces the scheduler; the queue table and job shape stay.
   (Rejected for v1: a second worker process over 20 attach — real
   parallelism but blocks on finishing 20; parking jobs entirely — the
   queue-plus-drain is useful today.)
3. **`transaction { }` ships in this iteration.** Language block deferring
   `wal_commit` to block end; a trap unwinding out of the block aborts the
   staged batch (nothing committed, RAM state rolled back or rebuilt per
   the spec's choice — the spec must settle recovery semantics precisely).
   This is the headline: job enqueue + business write, one fdatasync, no
   outbox.
4. **Recorded as iteration 18; spec + plan before any code.**

## Goals (draft — the spec refines)

- `framework/cache.wo`: a TTL + size-cap cache class (lazy `time.now`
  expiry on read, evict-on-write over capacity) usable as a field on any
  long-lived instance. Pure `.wo`, no engine change.
- Feature flags: `@table`-backed flags with a cached read-through map and
  bump-on-write invalidation — a framework pattern (and helper) proving
  "table + cached read" with zero cross-node invalidation problem.
- Background jobs: a `@table` queue (durable, WAL-recovered, restart-run
  proven like the storefront's) + a framework drain seam — the serve loop
  offers a bounded after-response tick to a job runner the app registers;
  job handlers are classes satisfying an interface (the Handler doctrine).
- `transaction { }`: multi-statement atomicity exposed in the language,
  engine-backed by the existing staged batch; enqueue-with-write becomes
  one commit. Trap = abort.
- The web-app demonstrates: an order-confirmation job enqueued in the same
  transaction as the order insert, drained after later requests.

## Open questions for the spec (not forks — details)

- Transaction semantics under trap: RAM apply happens before append —
  abort must undo RAM state; the spec settles whether the engine keeps an
  undo list or applies RAM changes only at commit.
- Nested `transaction { }`: reject (WO-E1xx) or flatten; leaning reject.
- Job table shape: id, kind, payload (json Text), attempts, not_before —
  the spec fixes it; retries/backoff policy stays app-side in v1.
- Drain seam shape: interface on the Dispatcher, or a second registration
  on `App` (`app.jobs(runner, budget)`); leaning the App registration.
- Cache eviction order: exact LRU needs an ordered structure — the spec
  decides between approximate (FIFO of keys) and true LRU cost.

## Acceptance Criteria (draft)

- **Given** a cache with TTL 1 and capacity N, **when** a request reads an
  expired key or writes past capacity, **then** the entry is gone /
  evicted — proven by a probe without sleeping the server (stamps
  injected, not waited).
- **Given** two writes inside `transaction { }` and a crash (SIGKILL)
  between block end and the next request, **when** the server restarts,
  **then** both rows exist; **given** a trap inside the block, **then**
  neither row exists and the server keeps serving.
- **Given** an order POST that enqueues a job transactionally, **when**
  the response has been sent and a subsequent request arrives, **then**
  the job has run within the drain budget; **given** a SIGKILL before the
  drain, **then** the job survives restart and runs after the next
  request.
- **Given** a flag flipped through its table, **when** the next request
  reads it, **then** the cached read reflects the write (bump
  invalidation), and `just web-app` stays green throughout.

## Out Of Scope

Pub/sub and WebSockets (behind 8/11); streaming job payloads; cross-node
anything (there is one node by doctrine); job priorities/cron scheduling
(the log-watcher `run` mode already covers time-based execution
externally); exposing the WAL batch API beyond `transaction { }`;
distributed cache invalidation (does not exist to invalidate).

## Proposed Solution

Spec next: transaction semantics (the one engine+language seam), the four
framework pieces as `.wo` (cache, flags, queue+drain, web-app demo), and
`just web-app` extended as the gate — including the SIGKILL/restart
transactional-jobs proof. Plan follows the spec; implementation on
approval.
