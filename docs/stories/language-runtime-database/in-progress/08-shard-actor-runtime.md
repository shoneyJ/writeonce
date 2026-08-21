---
iteration: "8"
status: in-progress
chain: 1
---

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
> it is a source of ideas, NOT the plan of record (✖ DISCARDED
> 2026-08-21: epoll-based; io_uring is a must).
>
> **RE-SEQUENCED 2026-08-21** (developer decision): the brainstorm →
> spec → plan happened. The arc's plan of record is
> [`2026-08-20-shard-fiber-arc.md`](../../../superpowers/plans/2026-08-20-shard-fiber-arc.md)
> (spec: [`2026-08-20-shard-fiber-arc-design.md`](../../../superpowers/specs/2026-08-20-shard-fiber-arc-design.md)),
> and **stages 1+2 LANDED 2026-08-20** on branch `concurrency-arc`
> (T1–T6, seven disclosed deviations recorded in the plan). Remaining
> scope: **stage 3, the transparent DB actor** — a correctness fix, not
> an optimization: worker VMs are zero-initialized, so a DB statement
> off the primary shard traps `WO_T_DB`. Concurrency-chain order:
> **stage 3 → 22 → 31 → 24 → 23 → 32**.

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
4. **Order — SUPERSEDED 2026-08-21.** Originally 22 → the 8+11 arc → 23;
   in fact the arc's stages 1+2 landed before 22 ever ran (accepted
   deviation — the before/after delta is owed and lands as 22's
   multi-shard pass). Current order: **stage 3 → 22 → 31 → 24 → 23 → 32**.
   23 still waits for the arc's tick boundary and 22's baseline.

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
- What to achieve? (stage-3 refinement, 2026-08-21)
    - **Given** a worker shard issuing a write through the DB actor,
    - **when** the process is killed between the worker's send and the
      owner's commit,
    - **then** the write was never acknowledged AND replay shows no
      partial state — a write RPC is exactly one owner-shard commit,
      and the ack crosses shards only after the owner's fsync.
- What to achieve? (stage-3 refinement, 2026-08-21)
    - **Given** a multi-shard boot,
    - **when** workers start serving,
    - **then** WAL replay has already completed on the primary, and no
      worker ever opens the WAL or the data directory (asserted in
      debug builds).
- What to achieve? (stage-3 refinement, 2026-08-21)
    - **Given** concurrent workers hammering reads and writes at one
      table,
    - **when** the deterministic multi-shard corpus runs under TSan,
    - **then** no torn read exists — every statement sees the serialized
      moment its envelope executes on the owner shard (replies are
      materialized copies). Full guarantee map:
      [the marker doc](../../../in-progress/2026-08-21-arc-stage-3.md).

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
  [22](../refine/22-durability-throughput-scale.md) at the concurrency
  scale it unlocks and recording the before/after delta; it is also
  where [23](../refine/23-io-uring-commit.md) gets a thread to overlap
  durability against.

## Proposed Solution

Execute stage 3 of the plan of record
([`2026-08-20-shard-fiber-arc.md`](../../../superpowers/plans/2026-08-20-shard-fiber-arc.md)):
the DB-actor migration — engine calls off the owner shard become message
sends with parked replies, closing the `WO_T_DB` hole. Stages 1+2
(scheduler, fibers, unified spawn/send, WO-E222 traced-send rejection,
cross-shard envelopes with home-routed frees) landed 2026-08-20. After
stage 3: 22 measures, then iteration 24 proves the arc. Actor lifecycle
(request/response, backpressure, supervision, timers) is deliberately
NOT the arc's scope — it is [iteration 31](../refine/31-actor-lifecycle.md).
