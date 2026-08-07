# Iteration 9 — database engine

> Format: fiberloom `product/story-iteration-template`. Part of
> [Story — one language, one runtime, one database, one binary](00-story.md).

## Goals

- The language's oldest promise executes on the new runtime: every class is
  a table. `insert` and `select` stop trapping (`DB_STUB` retires) and run
  against class-shaped row storage inside the VM's shards.
- Data survives anything: a typed write-ahead log with ack-after-fsync,
  parallel boot replay, and a crash battery proving no committed row is
  ever lost and no half-applied transaction ever visible.

## Acceptance Criteria

- What to achieve?
    - **Given** a class annotated `@table` and a method inserting rows,
    - **when** the method runs,
    - **then** the insert is WAL-logged before acknowledgment, visible to
      subsequent `select`, and the pricing fixture that previously trapped
      `DB_STUB` now passes with real data.
- What to achieve?
    - **Given** kill -9 at arbitrary points during committed writes,
    - **when** the process reboots and replays,
    - **then** every acknowledged commit is present, no unacknowledged
      partial write is visible, and replay across shards completes without
      manual steps.
- What to achieve?
    - **Given** a `@unique` field and a duplicate insert,
    - **when** it executes,
    - **then** the insert traps with the unique-violation code and
      secondary indexes remain consistent (maintained only through the
      engine's row choke points).

## Out Of Scope

- Cypher/document query paradigms and `LIVE` subscriptions (the query
  layer's later phases; `prototypes/wo-db` stays the reference).
- Cross-shard 2PC transactions; Postgres mirroring (exists on the Rust
  side; ports after parity).

## Info

- Doctrine: RAM is authoritative; the WAL makes it durable; indexes drift
  unless writes go through the row API — the Rust runtime learned this
  lesson, the C engine enforces it.
- The wo-db overlap manifest keeps the C++ prototype and this engine
  answer-compatible where features overlap.

## Proposed Solution

- Execute the existing plan: `docs/superpowers/plans/2026-08-01-db-engine-binding.md`
  (class-shaped row slabs with a choke-point row API, typed WAL + parallel
  replay + crash battery, insert execution, doctrine-enforced indexes +
  `@unique` trap, select subset, db corpus).
