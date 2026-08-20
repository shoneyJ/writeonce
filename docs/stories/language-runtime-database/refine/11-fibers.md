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
> `docs/examples/fibers` (`just fibers` 8/0). The shard-context criteria
> (TID assertions, cross-shard sends, blue-green drain reuse) close with
> the arc's stage 2.

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
- Open questions to settle in the spec — REDUCED 2026-08-20: the spawn
  surface is settled (the unified actor address, iteration 8 decision 2).
  Still open for the arc's spec: budget size and check granularity,
  parked-fiber drop semantics, run-queue fairness (FIFO v1), and how a
  parked fiber's borrow state interacts with the shard's GC safepoints.

## Proposed Solution

- No implementation plan exists yet — this iteration starts with the
  brainstorming → spec → writing-plans chain (the superpowers path every
  prior iteration followed), then executes that plan. The research note
  above is the brainstorm's entry material.
