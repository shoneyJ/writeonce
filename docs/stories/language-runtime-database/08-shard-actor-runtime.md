# Iteration 8 — shard-actor runtime

> Format: fiberloom `product/story-iteration-template`. Part of
> [Story — one language, one runtime, one database, one binary](00-story.md).

## Goals

- The runtime scales past one core the doctrine way: pinned thread-per-core
  shards, each owning its own heap and event loop; cross-shard
  communication is a message send that **moves ownership** — shared mutable
  state never exists.
- The language grows `spawn` and message send; garbage collection stays
  per-shard, so no global pause appears at any core count.
- The `@gc` story completes here: the Bacon–Rajan cycle collector (staged
  out of iteration 2) lands on the shard's own event loop — the per-tick
  budget host it was always specified to run on — retiring iteration 2's
  documented cycle leak.

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
    - **Given** a `@gc` reference,
    - **when** code attempts to send it cross-shard,
    - **then** the compiler rejects it — aliased references cannot cross
      heap boundaries.
- What to achieve?
    - **Given** a cyclic `@gc` object graph that becomes garbage,
    - **when** the shard's budgeted collection ticks run,
    - **then** the cycle is freed within the configured budget, no pause
      exceeds the configured slice, and iteration 2's must-leak fixture
      flips to must-collect.

## Out Of Scope

- Fibers/green threads — iteration 11 extends this scheduler; research at
  `docs/plan/exploration/fibers/00-fibers.md`.
- Cross-shard transactions (the database iteration's 2PC concern, later).

## Info

- The C reference (`runtime/wo-rt.c`, phases A–F) is the substrate: epoll
  loops, eventfd mail, the machinery this iteration lifts into `wovm`.
- The VM's object header has carried a shard id since iteration 2 — no
  relayout.

## Proposed Solution

- Execute the existing plan: `docs/superpowers/plans/2026-08-01-shard-actor-vm-runtime.md`
  (pinned-worker scheduler, shard-stamped heaps, MPSC mailbox rings + mail
  eventfds, send-as-move with home-routed frees, gc pacing per tick,
  spawn/send surface, actor corpus) — plus the cycle-collector task
  adopted from iteration 2's plan: possible-cycle buffer on RC decrement,
  trial-deletion scan under the per-tick budget.
