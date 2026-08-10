# Iteration 10 — HTTP service layer

> Format: fiberloom `product/story-iteration-template`. Part of
> [Story — one language, one runtime, one database, one binary](00-story.md).

## Goals

- The single binary serves: `service rest` blocks compile to routes and the
  runtime answers HTTP from its shards — hand-rolled HTTP and JSON, zero
  libraries, per the dependency doctrine.
- The new stack reaches feature parity with the Rust runtime's Stage 2
  REST surface — the concrete bar the retirement decision measures
  against.

## Acceptance Criteria

- What to achieve?
    - **Given** the blog sample's `service` declarations,
    - **when** the compiled binary boots and `.dev/reference/rest/blog.rest`
      runs against it,
    - **then** every request in the smoke file answers as documented —
      including the intentional 501/405/404 responses.
- What to achieve?
    - **Given** a method call arriving over REST that traps (borrow
      violation, abort, unique violation),
    - **when** the response returns,
    - **then** exactly one trap-to-HTTP table governs the mapping (e.g.
      conflict-shaped 409s) and the structured error body carries the trap
      record — the same trap surface everywhere, now over the wire.
- What to achieve?
    - **Given** transports declared at boot,
    - **when** the runtime starts under systemd socket activation or with
      zero listeners,
    - **then** both boot correctly — a runtime is not tied to a port.

## Out Of Scope

- WebSocket/live subscriptions and UI (the parked `##ui` story).
- The management plane endpoints (iteration 12 builds them on this
  machinery).

## Info

- Route declarations ride the `.wob` format as a versioned section — a
  coordinated format bump, the pattern later sections follow.
- The C reference's phase-C HTTP machine is the porting source.

## Proposed Solution

- Execute the existing plan: `docs/superpowers/plans/2026-08-01-http-service-layer.md`
  (per-shard http module, hand-rolled JSON codec, service-block route
  section, Stage-2-parity CRUD, method RPC with the trap table, blog.rest
  smoke).
