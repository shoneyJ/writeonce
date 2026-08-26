---
track: databasev2
iteration: "3"
was_language_iteration: "32"
status: refine
chain: 6
---

# databasev2 3 — WAL checkpoint: disk space reclamation and bounded replay

> **Moved 2026-08-26** from the language track, where this was iteration 32.
> Part of [Story — databasev2: the database beyond RAM](00-story.md). Content unchanged by
> the move; its dependencies are restated in that track index.

> Format: fiberloom `product/story-iteration-template`. Part of
> [Story — one language, one runtime, one database, one binary](../language-runtime-database/00-story.md)
> — the track this iteration was authored in before the 2026-08-26 move.
>
> **Inserted 2026-08-21** (stage-3 guarantee refinement found the hole):
> the WAL is append-only FOREVER — no checkpoint, no truncation exists
> in the engine or anywhere on the roadmap. Disk grows without bound and
> replay time grows with history, so restart cost rises with every write
> the program ever made. RAM reclamation already exists (deleted rows
> free their slot — [`04-db-binding.md`](../../plan/oop-vm/04-db-binding.md):
> "Ids are never reused; slots are"); this iteration is the DISK half.
> LAST in the concurrency chain:
> **stage 3 → 22 → 31 → 24 → 23 → 32** — it wants 22's measured
> replay/restart numbers to justify its policy and must compose with
> 23's group-commit write path.

## Goals

- **Disk space is reclaimed.** A checkpoint writes the live store as a
  snapshot and truncates the WAL behind it; deleted rows and
  overwritten versions stop occupying disk forever.
- **Replay is bounded.** Startup replays snapshot + WAL tail, not the
  program's whole write history — restart time becomes a function of
  store size, not store age.
- **Every existing guarantee holds byte-for-byte.** Ack-after-durable,
  replay-whole-or-not-at-all, torn-tail drop, ids never reused — a
  checkpoint changes where bytes live, never what an ack means. A crash
  DURING checkpoint recovers from the previous snapshot + full tail:
  the old WAL is not truncated until the new snapshot is durable.

## Acceptance Criteria (draft — the spec refines)

- **Given** a store with N rows after many writes and deletes, **when**
  a checkpoint completes, **then** disk usage reflects the live rows
  (plus the WAL tail), and a restart replays snapshot + tail to the
  byte-identical store.
- **Given** kill -9 at ANY instant during a checkpoint, **when** the
  process restarts, **then** recovery produces the same consistent
  store as if the checkpoint had never started — no acknowledged write
  lost, no partial snapshot ever read.
- **Given** the iteration-22 restart benchmark re-run after checkpoint
  lands, **when** replay time is measured on an aged store, **then**
  the bounded-replay improvement is recorded as a before/after delta.
- **Given** writes arriving while a checkpoint runs (the DB actor
  serializes statements; the checkpoint must not stall them beyond the
  stated budget), **when** the mixed load completes, **then** every ack
  held its durability contract and the tail contains exactly the
  post-snapshot writes.

## Out Of Scope

- MVCC / multi-version reads — the store is update-in-place RAM; "old
  versions" exist only as WAL history, which is exactly what truncation
  reclaims.
- Incremental/streaming backup, point-in-time recovery — a snapshot is
  a recovery artifact here, not a backup product.
- Cross-shard checkpoint coordination — the WAL is owner-shard-only
  (stage 3's rule); one shard, one checkpoint.
- Compression, dedup, tiering — measure first (22), add only what a
  number justifies.

## Info

Forks the spec must settle:

1. **Snapshot format** — a row-image dump of the live store (simple,
   O(live rows)) vs a rewritten-compacted WAL (reuses replay machinery,
   O(live rows) too but stays in one format). Leaning: row-image dump
   in the WAL's existing record grammar, so replay needs no second
   decoder.
2. **Trigger policy** — size threshold (WAL bytes vs snapshot bytes
   ratio), boot-time compaction, explicit call, or some mix. Leaning:
   ratio threshold checked at commit, plus manual trigger for tests;
   decided against 22's numbers.
3. **Write availability during checkpoint** — stop-the-world dump
   (simplest; the DB actor just runs one long "statement") vs
   fork-and-dump vs incremental copy. Leaning: measure the
   stop-the-world pause on the 1M-row store first (22); complexity only
   if the pause breaks a stated budget.
4. **Composition with 23** — the snapshot's durability barrier rides
   the same per-shard ring (WRITE+FSYNC chain, then the truncate);
   ordering vs in-flight group commits must be stated normatively in
   [`04-db-binding.md`](../../plan/oop-vm/04-db-binding.md)'s WAL
   section.

## Proposed Solution

Brainstorm → spec → plan after 23 lands (the write path it composes
with) using 22's aged-store replay numbers as the policy input; extend
`04-db-binding.md`'s WAL section with the snapshot format the way the
record grammar is documented today.
