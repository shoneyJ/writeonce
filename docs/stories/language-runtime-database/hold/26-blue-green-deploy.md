# Iteration 26 — blue-green in-runtime deployment

> Format: fiberloom `product/story-iteration-template`. Part of
> [Story — one language, one runtime, one database, one binary](00-story.md).

## Goals

- The story's closing promise: the running binary updates its own code.
  Two fixed VM slots (activity alternating); a proposal pipeline —
  propose → approve → in-runtime compile → additive schema migration →
  load → health → atomic switch — with the previous version staying
  resident as the instant rollback target.
- The developer drives it remotely: `wo remote pull / propose / diff /
  approve / rollback / status` over a loopback management transport,
  every stage streaming live and WAL-audited.

## Acceptance Criteria

- What to achieve?
    - **Given** a running fixture app and an additive code+schema change,
    - **when** the developer proposes and approves it,
    - **then** the deploy completes with zero dropped requests (in-flight
      work — including parked fibers — drains on the old slot through
      iteration 11's unwind path), and the binary's embedded source
      trailer matches the new active version afterward.
- What to achieve?
    - **Given** a failure at any pipeline stage (compile diagnostic,
      destructive-change rejection, load failure, health failure, drain
      timeout),
    - **when** it occurs,
    - **then** the active slot keeps serving untouched, the failure is
      visible in the SSE stream and the WAL trail, and a destructive
      change was rejected at propose time naming the offending
      declaration.
- What to achieve?
    - **Given** a completed deploy,
    - **when** `wo remote rollback` runs,
    - **then** the previous version serves again in under one second with
      no compile and no data change — additive-only migration guarantees
      old code runs correctly against the migrated schema.
- What to achieve?
    - **Given** kill -9 during COMPILING / MIGRATING / SWITCHING /
      trailer-rewrite,
    - **when** the unit restarts,
    - **then** it serves one consistent version and the WAL shows whole
      migrations only.

## Out Of Scope

- Script-based/destructive migrations, in-runtime editing workspace,
  MCP/agent wrapper over the management plane — all recorded follow-ups.
- New scheduler work — fibers landed in iteration 11; this iteration's
  drain unwinds parked fibers through 11's shutdown path, it does not
  extend it.

## Info

- Approved spec: `docs/superpowers/specs/2026-08-03-blue-green-vm-design.md`;
  its implementation plan is deliberately authored only after iterations
  9–10 ship (prerequisites: a catalog to diff, HTTP machinery to build on).
- VMs own code; the engine owns data — the separation that makes the
  switch cheap and rollback unconditional.

## Proposed Solution

- Author the implementation plan from the approved spec once iterations
  9–10 land, then execute it (slots, deploy state machine, additive
  differ, management surface + SSE, `wo remote` verbs, crash battery).
