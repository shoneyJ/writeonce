---
track: databasev2
iteration: "3"
was_language_iteration: "32"
status: done
readiness: ready
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

> **BRAINSTORMED 2026-08-28.** Spec:
> [`2026-08-28-wal-checkpoint-design.md`](../../superpowers/specs/2026-08-28-wal-checkpoint-design.md)
> · plan: [`2026-08-28-wal-checkpoint.md`](../../superpowers/plans/2026-08-28-wal-checkpoint.md)
> (6 tasks).
> Read `.dev/reference/postgresql` for this — and the conclusion was that
> Postgres' design is *unavailable* to us, which is what makes the simpler one
> legitimate.
>
> **The design in one sentence:** compact the log by rewriting it as one record
> per live row into a temp file, then `rename` it over the live WAL. Recovery is
> **completely unchanged** — boot still opens one file and replays it — and the
> crash criterion is satisfied by the filesystem rather than by code we must get
> right.
>
> **Why one file works here and not in Postgres.** Postgres never compacts its
> WAL: its records are page deltas, so a compacted redo log is not a store, and
> it must keep heap files, a control file, a redo pointer and a second recovery
> source. Ours are **full row images** — `apply_record` implements UPDATE as
> remove-then-recreate — so a compacted log *is* a complete store. That one
> difference deletes the control file, the redo pointer, the cutoff offset and
> the separate process from the design.
>
> **Forks settled:** no snapshot format (the compacted log is the snapshot); one
> source, not two; **volume-only trigger** as a ratio against the last
> compaction's own measured output, with an absolute floor — **no timer**,
> because Postgres' timer exists to bound loss from unflushed buffers and we have
> none; stop-the-world, with the pause measured against a stated budget rather
> than assumed acceptable.
>
> **The coupling that would otherwise be found late — and was found in time:**
> compaction moves every record, so it **invalidates every WAL offset**
> [iteration 2](02-table-storage-modes.md)'s `resident: keys` stores. The
> compactor rebuilds the offset map as it writes. Recorded here while iteration
> 2's storage half was still unimplemented; **it landed 2026-08-29 and the
> obligation was discharged** (`f606fc9`), including a worse failure this note
> did not predict — see the hazard section at the end.
>
> **Measured on master 2026-08-28, grounding the whole iteration:** `seed 20000`
> leaves a 986 614-byte log; 20 000 updates take it to **2 590 262 bytes with the
> same live rows** (2.6× history for no data), and boot+verify on that store is
> **155 ms**.

## Progress — landed 2026-08-29

| # | Task | State |
| --- | --- | --- |
| 1 | `wo_wal_compact` — rewrite, fsync, rename, fsync parent, reopen | ✅ `8ea510d` |
| 2 | a stale compaction temp is removed at open | ✅ `8bfbd4b` |
| 3 | the trigger (pure decision + env knobs) and the ordering guard | ✅ `6dbcb9a` |
| 4 | `kill -9` DURING compaction — 40 rounds, mutation-proven | ✅ `9b283d5` |
| 5 | measure space, boot and the stop-the-world pause | ✅ `d87f65a` |
| 6 | closeout | ✅ this change |

### Measured

| | checkpointing off | checkpointing on |
| --- | --- | --- |
| WAL used | 1 962 358 B | **907 094 B** |
| boot | 114 ms | **64 ms** |

**2.16× space reclaimed, 1.78× faster boot**, stop-the-world pause **2 651 µs**
against a stated 50 ms budget. Full details, including the pause's scaling, are
in [`perf-targets.md`](../../plan/perf-targets.md) §7.

### Two bugs the work found, both mine

**Wiring only the drain left `WO_SHARDS=1` never compacting** — its log grew
forever (536 KB where the multi-shard run held 446 KB), because a statement on
the owner shard never enters that drain. Both write paths now check.

**The dump was 8× slower than it needed to be**, flushing through the
committing path and so paying one `fdatasync` per 256 records for durability
that is worthless before the rename. One final barrier took the pause from
107 649 µs to 13 212 µs on a 2 MB live set — ~22 MB/s to ~181 MB/s.

## Acceptance Criteria

Met:

- **Given** an aged store, **when** it is compacted, **then** disk is reclaimed.
  ✅ 2.16× on the full campaign, asserted rather than merely recorded — the leg
  fails if the log is not smaller with checkpointing on.
- **Given** the same store, **when** it boots, **then** replay is bounded by the
  live set rather than by history. ✅ 114 → 64 ms.
- **Given** `kill -9` at ANY instant during a checkpoint, **when** the process
  restarts, **then** recovery produces the same consistent store as if the
  checkpoint had never started, with no acknowledged write lost. ✅ 40 rounds
  per run, 10 consecutive clean runs, and **proven to have teeth**: against the
  design's rejected alternative (in-place rewrite instead of `rename`) the
  battery fails every run with the log destroyed.
- **Given** the iteration-22 replay numbers, **then** a before/after delta is
  recorded. ✅ `perf-targets.md` §7.
- **Given** writes arriving while a checkpoint runs, **then** the ack contract
  holds. ✅ compaction runs only where nothing is staged, asserted by a test
  that stages and requires refusal; `wo_wal_compact` also refuses as a backstop.

Outstanding:

- ~~**The `resident: keys` offset map.**~~ **Discharged 2026-08-29 by
  iteration 2's task 5d** (`f606fc9`). The obligation written at the compactor
  in `wal.c` did its job: the implementer hit it there. See the hazard section
  below for what it caught — and for the second, worse failure it did not
  predict.
- **The pause is O(live rows).** At ~181 MB/s a 1 GB live set implies ~5.5 s,
  past any interactive budget. Incremental or forked copying was deliberately
  not bought in advance; this is the number to buy it against.

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
  **The "before" now EXISTS** (databasev2 1, 2026-08-27): `db-bench`'s
  `replay` leg measures **≈5.5 µs per WAL record**, and — the number
  this iteration is actually about — **1.9× the boot cost for an
  identical live dataset** once the same rows have been updated once
  each (20 000 rows: 110 ms at 20 000 records, 211 ms at 40 000). Boot
  cost tracks **history, not data**, which is exactly what a checkpoint
  collapses. Metrics: `replay.inserts.*`, `replay.history.*`,
  `replay.history_penalty_x`.
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
with). **Correction (2026-08-27):** this said "using 22's aged-store
replay numbers as the policy input", but iteration 22 produced no such
numbers — it proved restart *correctness* and never timed it, and
`bench/baseline.json` carried zero replay metrics until databasev2 1
added them. The policy input is the `replay` leg's ≈5.5 µs/record and
its 1.9× history penalty. Extend
`04-db-binding.md`'s WAL section with the snapshot format the way the
record grammar is documented today.
## Hazard: compaction invalidates every `resident: keys` offset

Surfaced while refining this iteration and recorded here so it is not
rediscovered late. [Iteration 2](02-table-storage-modes.md)'s
`resident: keys` stores a **WAL byte offset per row** and reads the row
back with `pread` at that offset. Compaction — whichever of the two
shapes below wins — **rewrites the log and moves every record**, so
every stored offset becomes wrong. Not stale-but-readable: pointing at
an arbitrary byte in a rewritten file, which is a correctness fault,
not a performance one.

So the two iterations are coupled and the coupling has to be designed,
not discovered: either compaction rebuilds the offset map as it
rewrites (it knows both addresses, so this is the cheap direction), or
the snapshot persists the map and compaction is forbidden while any
`resident: keys` table is live. **The first is almost certainly right**,
but it means compaction cannot be written as a pure file operation that
ignores in-memory table state.

### Settled 2026-08-29 — and the hazard was only half the danger

The first shape was implemented, in iteration 2's task 5d (`f606fc9`).
Compaction re-points each row as it writes it, using a value-only map update
that cannot rehash, so a walk in progress stays valid and no per-row buffer of
new offsets is needed. Compaction is therefore **not** a pure file operation,
exactly as predicted above.

**What this section did not predict is the failure that would actually have
struck first.** It described stored offsets going stale — a pointer into a
rewritten file. But the compactor walked the slab **bitmap**, and a
keys-resident row holds no bitmap bit: its slot returns to the free list when
the payload is dropped. Every such row would therefore have been omitted from
the new log altogether. That is silent data loss, not a bad pointer, and
rebuilding offsets would never have caught it — the rows would simply have been
gone.

Both failure modes are now pinned by `test_keys_resident_survives_compaction`,
which rewrites rows in hash order so the offsets genuinely move; a map left
un-repointed lands on another row and fails the identity check rather than
passing by luck.

A failure *after* any row has been re-pointed is fatal by design: the map would
name offsets inside a temp file that the failure path unlinks, and the intact
original log replays correctly, so stopping is strictly better than serving
wrong rows.
