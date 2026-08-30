---
track: databasev2
iteration: "11"
status: pending
readiness: ready
---

# databasev2 11 — bounding a keys-resident row's delta chain

> Part of [Story — databasev2: the database beyond RAM](00-story.md).
> Spec: [`2026-08-30-bounded-delta-chains-design.md`](../../superpowers/specs/2026-08-30-bounded-delta-chains-design.md).
>
> **Why this exists.** [Iteration 2](02-table-storage-modes.md) shipped delta
> updates with a deliberate decision not to cap chain length, on the reasoning
> that compaction bounds it. A whole-branch review showed that reasoning does
> not hold for the one workload the feature is motivated by. This iteration
> closes it. `readiness: ready` — every fork is settled in the spec.

## The finding this iteration answers

A read of a keys-resident row costs `1 + chain length` preads, and replay costs
**O(N²)** per chain. The shipped mitigation is compaction, which flattens chains
to zero. But `wo_wal_should_compact` triggers on `used > last * ratio` — a byte
ratio over the whole log — and cannot see that one row has a very long chain.

One popular SKU whose stock moves on every order, in a catalogue that is
otherwise quiet, grows an unbounded chain without ever moving that ratio. The
guard that bounds replay in general is structurally blind to the single case
that makes replay quadratic.

## The design, in one sentence each

**Tier 1 — flatten on update.** The update path already folds the row, because
it needs the old values for index maintenance, and the fold already walks the
chain hop by hop; so it reports the depth for free, and when that depth reaches
**K** the update appends a full-row record instead of a delta. Read cost becomes
at most `K + 1` reads and replay `O(K²)` per row, independent of when a
checkpoint fires.

**Tier 2 — give the compaction policy an absolute term and a ceiling.** Our
current policy has a proportional term and a *suppressor* misleadingly named a
floor; it lacks the triggering floor and the ceiling that keep a size-based
policy honest.

## Where the design came from

Read from PostgreSQL's source at `.dev/reference/postgresql`, not recalled:

- **`heap_page_prune_opt`** collapses HOT chains opportunistically, on a page the
  process already holds, gated by an O(1) on-page hint and then by page fullness
  against `Max(fillfactor, BLCKSZ/10)`. Tier 1 is this shape: do the work while
  you already hold the thing, using a signal you already computed.
- **autovacuum** thresholds on
  `vac_base_thresh + vac_scale_factor * reltuples`, clamped by a maximum —
  defaults 50, 0.2, 100 000 000. A count with a floor, a proportion and a
  ceiling, per table. Tier 2 borrows the floor and the ceiling.
- **Postgres never thresholds on new-bytes-versus-old-bytes**, despite knowing
  exactly what a chain costs. Its space test is "will the next version fit" — an
  operational constraint, not an economic comparison. That ruled out the
  byte-ratio shape here too.

## Acceptance Criteria

Outstanding — none met; this iteration has not started.

- **Given** a row updated K times, **when** updated once more, **then** the
  record its offset names is a full row and its chain length is zero.
- **Given** a row updated far more than K times, **when** it is read, **then** it
  performs at most K + 1 record reads, asserted by counting rather than timing.
- **Given** the same row, **when** the process restarts, **then** replay is
  correct and its cost does not grow with the total updates ever applied.
- **Given** a flattening update, **when** replayed, **then** the row matches the
  same row in a `resident: all` table under the same update sequence — the
  resident table is the oracle.
- **Given** a flattening update to an indexed column, **when** queried through
  that index, **then** the row is found by its new value and not its old, before
  and after a restart.
- **Given** reclaimable bytes past the absolute threshold but inside the ratio,
  **when** the policy is evaluated, **then** compaction fires. *(Tier 2.)*
- **Given** a `resident: all` table, **when** any of this runs, **then** nothing
  about its behaviour or log records changes.

## Out Of Scope

- **Varying K by row width.** Hop count is what bounds read and replay cost;
  width would optimise only write amplification. Revisit with a measurement, not
  before.
- **A time-based compaction trigger.** Records are durable at commit, so an idle
  log does not grow.
- **Whether `resident: keys` earns its place at all.** That is iteration 2's
  task 7, and it should arguably run *before* this work — see below.

## Info — the forks, settled

1. **Where to fix it: the update path, not the checkpoint.** Making compaction
   depth-aware would mean one hot row triggering a stop-the-world rewrite of the
   entire log — a 2 651 µs pause that scales with total live rows, not with the
   row that misbehaved. Postgres reaches for the local, opportunistic fix first
   for the same reason.
2. **The metric is hop count, not bytes.** Each hop is one `pread` whose cost
   barely varies with the bytes it carries, so hops are what our read cost is
   made of. Bytes govern write amplification, which is the secondary concern.
3. **K is a fixed constant and does not scale with table size.** Postgres scales
   by `reltuples` because it thresholds a table-level aggregate with
   proportional harm. Ours is per-row with additive cost — reading one product
   costs the same whether the catalogue holds a hundred rows or ten million, and
   total replay is the sum across rows. Scaling K up with size would make the
   largest databases boot worst.

## Sequencing note

This iteration is **ready but arguably should not be next**. Iteration 2's
task 7 has still never measured whether `resident: keys` beats the kernel's own
paging, and everything built on it — including this — assumes it does. If that
measurement comes back poorly, this work is optimising something that should be
deleted. Recommended order: measure first, then this.
