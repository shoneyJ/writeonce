---
iteration: "31"
status: refine
chain: 3
---

# Iteration 31 — actor lifecycle: request/response, backpressure, death, timers

> Format: fiberloom `product/story-iteration-template`. Part of
> [Story — one language, one runtime, one database, one binary](../00-story.md).
>
> **Inserted 2026-08-21** (concurrency-chain re-sequence; the iteration
> was named as "new 31" in the 2026-08-20 code-review re-sequence — this
> is its story file). Third in the chain,
> **stage 3 → 22 → 31 → 24 → 23 → 32**: chat
> ([iteration 24](24-chat-websocket-workload.md)) cannot be written
> honestly without these four mechanisms.

## Why this iteration exists

The arc's stages 1+2 shipped `spawn`/`send` mechanism without lifecycle:
`send` is one-way and callers `sleep`-poll to await an answer; the
mailbox FIFO grows without bound (a hot sender can exhaust a shard's
memory); an actor that traps dies silently (nobody learns, nothing
restarts, its mailbox rots); and there is no timer surface beyond a
fiber blocking in `time.sleep`. Every real serving program — chat first —
is made of request/response turns, bounded queues, death notices, and
deadlines. Without this iteration the arc is a demo, not a runtime.

## Goals

- **Request/response over one-way sends.** A caller can send and park
  until the reply arrives — one surface, no `sleep`-polling, no second
  concurrency vocabulary. Ownership rules unchanged: the request moves,
  the reply moves back.
- **Bounded mailboxes with a stated backpressure policy.** A mailbox has
  a cap; what happens at the cap (park the sender vs error) is decided by
  the spec, one policy, doctrine-pure — no silent unbounded growth
  anywhere in the runtime.
- **Actor death is observable.** A trap or normal exit produces a signal
  another actor can receive; a fiber-trap already isolates (stage 1) —
  this makes the fact of death deliverable, so a supervisor CAN be
  written in `.wo`.
- **Timers as messages.** A deadline or interval delivers to a mailbox
  like any other send, riding the shard's existing io_uring/epoll
  timeout plumbing (arc T4) — no new event loop.

## Acceptance Criteria (draft — the spec refines)

- What to achieve?
    - **Given** an actor that answers requests,
    - **when** a caller awaits the reply,
    - **then** the caller's fiber parks (the shard serves other fibers,
      TID-verified), resumes with the moved reply, and never busy-waits.
- What to achieve?
    - **Given** a mailbox at its cap,
    - **when** another send arrives,
    - **then** the stated backpressure policy fires deterministically,
      memory stays bounded (RSS flat under a hot-sender soak), and no
      message is silently dropped.
- What to achieve?
    - **Given** an actor that traps mid-message,
    - **when** it dies,
    - **then** its drop maps run (ASan zero leaks), a death signal
      reaches the observer that asked for one, and a supervisor written
      in `.wo` can respawn it.
- What to achieve?
    - **Given** a timer armed by an actor,
    - **when** it fires,
    - **then** the actor receives it as an ordinary message on its own
      shard, and cancelling before expiry means it never delivers.

## Out Of Scope

- Supervision TREES / OTP-scale restart policy — a `.wo` library once
  death signals exist, not runtime policy.
- Priorities and custom scheduling — the reduction budget stays the only
  fairness mechanism.
- Distributed (cross-process) supervision — no network layer exists.
- Changing the ownership-move rule or the unified address surface —
  iteration 8's decisions stand.

## Info

Forks the spec must settle:

1. **The request/response surface.** A reply-address baked into the
   message shape vs a runtime-level call that parks — and what the
   compiler checks (does a request type name its reply type?).
2. **The backpressure policy at the cap.** Park the sender (natural with
   fibers, risks deadlock cycles) vs fail the send (explicit, pushes
   handling to the program). One policy, stated; not configurable per
   mailbox in v1.
3. **The death-signal shape.** Erlang's link (bidirectional, dies
   together) vs monitor (one-way notice) — likely monitor-only v1.
4. **The timer surface.** Builtin (`time.after` delivering a message) vs
   actor-spawned sleeper fiber — and cancellation semantics.

Sources: the 2026-08-20 code-review findings (the gaps this iteration
answers), the arc plan's stage-2 deviations
([`2026-08-20-shard-fiber-arc.md`](../../../superpowers/plans/2026-08-20-shard-fiber-arc.md)
— the unbounded FIFO is deviation 4's mutex inbox), and BEAM precedent
already surveyed in
[`docs/plan/exploration/fibers/00-fibers.md`](../../../plan/exploration/fibers/00-fibers.md).

## Proposed Solution

Brainstorm → spec → plan after the arc's stage 3 lands and iteration 22
has its baseline (the mailbox-cap and inbox-ring decisions want 22's
mutex number). The spec is written against iteration 24's needs — chat
names the lifecycle mechanisms it consumes, this iteration names chat as
its first honest consumer.
