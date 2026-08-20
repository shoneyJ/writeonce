# Iteration 8 — shard-actor runtime (the 8+11 concurrency arc, part 1)

> Format: fiberloom `product/story-iteration-template`. Part of
> [Story — one language, one runtime, one database, one binary](../00-story.md).
>
> **REFINED 2026-08-20** (developer decisions, no code): iterations 8 and
> 11 are **one arc** — the scheduler, fibers on it, then serving — because
> the database-ownership decision below makes a fiberless multi-shard
> server block a whole thread per cross-shard call. The arc's driving
> workload is **iteration 24 (chat: WebSocket pub/sub)**. The pre-existing
> plan (`docs/superpowers/plans/2026-08-01-shard-actor-vm-runtime.md`)
> predates inferred GC (7b), the unified surface, and the DB decision —
> it is a source of ideas, NOT the plan of record; the arc starts with a
> fresh brainstorm → spec → plan.

## Settled decisions (2026-08-20)

1. **The database is an actor.** The engine (`wo_db` + WAL) lives on one
   owner shard; every query/write from another shard is a message send,
   and results come back materialized (queries already copy rows out —
   the model was built for this). Doctrine-pure: no lock, no shared
   mutable state. Consequence, accepted deliberately: callers must PARK
   while the reply travels, which is why 8 and 11 ship as one arc.
   (Rejected: a coarse engine lock — bends the doctrine and caps write
   scaling anyway; partitioned tables — the real scale answer, but it
   waits for a measured need, not v1.)
2. **One concurrency surface: everything is an actor address.** `spawn`
   returns an address whether the spawnee lands on this shard (a fiber)
   or another (placement policy's call); `send` always moves ownership;
   a same-heap send skips the ring and is cheap. The language never
   shows a fiber-vs-actor split. (This dissolves iteration 11's
   "handle vs address" open question.)
3. **Driving workload: chat** (iteration 24) — rooms, broadcast, N
   concurrent WebSocket clients, one binary. The arc's acceptance is the
   chat sample's, not only synthetic corpora.
4. **Order: 22 → the 8+11 arc → 23.** The io_uring write path waits for
   the arc (its batch boundary is the shard tick) and for 22's baseline.

## Goals

- The runtime scales past one core the doctrine way: pinned
  thread-per-core shards, each owning its own heap and event loop;
  cross-shard communication is a message send that **moves ownership** —
  shared mutable state never exists.
- The language grows `spawn`/`send` (the unified address surface);
  garbage collection stays per-shard (7b's collector is already
  per-shard by construction), so no global pause appears at any core
  count.
- The database keeps its single-writer truth by BEING an actor on its
  owner shard.

## Acceptance Criteria

- What to achieve?
    - **Given** a program spawning actors across shards,
    - **when** an owned object is sent to another shard,
    - **then** the sender can no longer touch it (compile-time move), the
      receiver owns it, and its eventual free routes back to its
      allocation-home arena.
- What to achieve?
    - **Given** debug builds with shard-ownership asserts,
    - **when** the deterministic actor corpus runs under ASan and TSan,
    - **then** zero races, zero leaks, and identical output across runs.
- What to achieve?
    - **Given** a reference to a TRACED object (GC-ness is inferred since
      7b — there is no `@gc` to write),
    - **when** code attempts to send it cross-shard,
    - **then** the compiler rejects it: aliased references cannot cross
      heap boundaries, and the diagnostic names the inferred-traced class
      and why it is traced. (This criterion originally said `@gc`;
      restated 2026-08-20 in inference terms — same rule, current
      language.)
- What to achieve?
    - **Given** handlers on serving shards querying and writing through
      the DB-owner shard,
    - **when** the employee/web-app matrices run multi-shard,
    - **then** every answer is byte-identical to the single-shard run and
      the WAL's ack-after-durable contract is unchanged.

## Out Of Scope

- Cross-shard transactions (2PC) — the DB actor serializes writers, so
  iteration 18's `transaction { }` is unaffected; distributing it is a
  later story.
- Fiber details beyond the shared scheduler substrate — part 2
  ([iteration 11](11-fibers.md)) owns them.
- WebSocket framing — the framework's (iteration 24's) job.

## Info

- The C proving ground (`docs/plan/exploration/c-runtime/`, phases A–F:
  epoll loops, eventfd mail) is the substrate this lifts into `wovm`.
  (Path restated 2026-08-20; the old `runtime/wo-rt.c` reference was
  stale — that tree was removed with the Rust runtime.)
- The VM's object header has carried a shard id since iteration 2 — no
  relayout.
- **Gated by the benchmark:** landing the arc means re-running
  [22](22-durability-throughput-scale.md) at the concurrency
  scale it unlocks and recording the before/after delta; it is also
  where [23](23-io-uring-commit.md) gets a thread to overlap
  durability against.

## Proposed Solution

Fresh brainstorm → spec → plan for the WHOLE arc (8+11), staged: the
pinned-worker scheduler + shard-stamped heaps + MPSC mailboxes + the
unified spawn/send surface and the traced-send rejection; fibers on that
scheduler (part 2's document); the DB-actor migration; then iteration 24
proves it. The 2026-08-01 plan is reference material for the mailbox and
heap-stamping shapes only.
