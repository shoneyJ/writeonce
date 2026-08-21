# Iteration 11 — fibers (the 8+11 concurrency arc, part 2)

> Format: fiberloom `product/story-iteration-template`. Part of
> [Story — one language, one runtime, one database, one binary](../00-story.md).
>
> **REFINED 2026-08-20** (developer decisions, no code): 8 and 11 ship as
> **one arc** — the DB becomes an actor on an owner shard
> ([iteration 8](08-shard-actor-runtime.md)'s decision), so serving
> shards must PARK on cross-shard replies, which is this iteration.
> The spawn-surface question below is SETTLED: one unified actor-address
> surface (`spawn` returns an address, fiber or remote alike; `send`
> moves ownership; same-heap sends take the cheap path). The arc's
> driving workload is [iteration 24: chat](24-chat-websocket-workload.md)
> — fiber-per-WebSocket-connection is the serving model that retires the
> framework's close-when-idle keep-alive policy.
>
> **STAGE-1 SUBSTANCE LANDED 2026-08-20** (branch `concurrency-arc`):
> reduction-budget fibers (back-edge accounting — the livelock lesson is
> in the plan's deviations), `spawn`/`send`/`actor M`, one-message-at-a-
> time delivery, main-return reap, fiber-trap isolation, and parked
> `net`/`time` builtins on the io_uring-first per-shard I/O plane
> (`WO_IO=uring|epoll`, epoll fallback proven). Demonstrated by
> `docs/examples/fibers` (`just fibers` 8/0).
>
> **STAGE-2 SUBSTANCE LANDED 2026-08-20** (branch `concurrency-arc`,
> T5–T6): pinned thread-per-core shards, envelope sends with ownership
> move, round-robin placement, home-routed frees, WO-E222 on EVERY
> spawn/send (placement makes any actor potentially remote), TID-verified
> shard context. Deviations disclosed in the plan of record
> ([`2026-08-20-shard-fiber-arc.md`](../../../superpowers/plans/2026-08-20-shard-fiber-arc.md)):
> the inbox is a mutex-guarded list + eventfd (rings arrive only if
> iteration 22 measures the mutex as a cost), the deterministic corpus
> pins `WO_SHARDS=1`, two TSan races and one teardown SEGV fixed.
>
> **RE-SEQUENCED 2026-08-21**: what remains for the chain is the arc's
> stage 3 (transparent DB actor — [iteration 8](08-shard-actor-runtime.md)),
> then measurement. Order: **stage 3 → 22 → 31 → 24 → 23**.

## Goals

- Concurrency **inside** a shard the doctrine way: cooperative fibers
  scheduled by the VM — a fiber is the interpreter state wovm already
  isolates (register windows + frame stack + pc), made per-fiber. One
  kernel task per core stays the rule; thousands of fibers run above it
  where the kernel tops out at hundreds of threads (CFS O(log N),
  ~8 MiB + ~20 µs per thread vs an arena-allocated fiber context).
- **Preemption by reduction budget**, the Erlang shape: the dispatch loop
  counts down and parks the fiber at zero — no signals, no safepoints, no
  stack copying, deterministic replay.
- **Blocking builtins park instead of block** on server shards: the same
  `net`/`time`/`fs` calls that block the thread in program mode hand their
  fd to the shard's epoll/io_uring loop and resume on completion. One API,
  same source text, no async/await, no function coloring — ever.
- Cross-fiber sends follow the iteration 8 rule — ownership moves — on the
  cheaper same-heap path.

## Acceptance Criteria

- What to achieve?
    - **Given** a shard running thousands of spawned fibers with one hot
      compute loop among them,
    - **when** the reduction budget expires,
    - **then** the hot fiber parks, every other fiber makes progress
      (starvation-free corpus fixture), and output is identical across
      runs (deterministic scheduling).
- What to achieve?
    - **Given** a fiber blocked in a stdlib builtin on a server shard,
    - **when** the completion arrives on the shard's event loop,
    - **then** the fiber resumes with the result while the shard served
      other fibers in the interim — one OS thread, verified by TID.
- What to achieve?
    - **Given** a parked fiber at shard shutdown,
    - **when** the shard unwinds it,
    - **then** every drop map runs (ASan zero leaks) — parked fibers die
      as cleanly as trapped ones. (Iteration 26's blue-green drain reuses
      exactly this unwind path.)
- What to achieve?
    - **Given** TRACED objects (GC-ness inferred since 7b) referenced only
      from a parked fiber's frames,
    - **when** the per-shard mark-sweep collector scans,
    - **then** parked fibers' frames are roots exactly as the live frame
      stack is (`vm_gc_roots` grows fiber awareness) — nothing live is
      collected, nothing dead survives. (Originally said `@gc`; restated
      2026-08-20 in inference terms.)

## Out Of Scope

- Fiber migration across shards (ownership doctrine forbids it; cross-shard
  is a message send, iteration 8).
- Priorities, timers, structured-concurrency policy — recipe-box
  capabilities for a later `.wo` library, not runtime policy.
- Program mode: stays single-fiber in v1 (blocking legal, log-watcher
  needs nothing more).
- async/await keyword — permanently rejected surface, not deferred.

## Info

- Research note: [`docs/plan/exploration/fibers/00-fibers.md`](../../../plan/exploration/fibers/00-fibers.md)
  — kernel's-eye evidence (task_struct costs, CFS collapse at high task
  counts) and the precedent survey (BEAM reductions adopted; Go stack
  copying and Tokio coloring rejected; Loom's park-under-blocking-API
  matches the stdlib posture).
- Vision origin: [blue-green vision §3](../../../plan/exploration/blue-green-vm/00-vision.md);
  iteration 8's scheduler is the substrate this extends.
- Open questions — REDUCED AGAIN 2026-08-21: the spawn surface, budget
  size/granularity (back-edge accounting — see the plan's livelock
  deviation), run-queue fairness (FIFO), and parked-fiber drop semantics
  (fiber-trap isolation + main-return reap) are all settled by the landed
  implementation. Still open: how a parked fiber's borrow state interacts
  with the shard's GC safepoints — tracked for stage 3 / the collector's
  next pass. Lifecycle surface (request/response, backpressure,
  supervision, timers) is [iteration 31](31-actor-lifecycle.md)'s, not
  this story's.

## Proposed Solution

- The plan exists and its fiber stages are landed:
  [`2026-08-20-shard-fiber-arc.md`](../../../superpowers/plans/2026-08-20-shard-fiber-arc.md)
  (stages 1+2 complete 2026-08-20). This story closes when the arc's
  stage 3 lands and iteration 22 records the delta; the chat workload
  ([iteration 24](24-chat-websocket-workload.md)) is the proof.
