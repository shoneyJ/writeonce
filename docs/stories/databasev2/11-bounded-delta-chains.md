---
track: databasev2
iteration: "11"
status: done
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

## Progress

| Part | State |
| --- | --- |
| Tier 1 — the fold reports hop count | ✅ `wo_wal_fold_row_at` takes `hops_out`; the walk already visited each hop, so it costs nothing |
| Tier 1 — the update branches on depth | ✅ `row_apply_field_keys` writes a full-row image past `WO_DELTA_MAX_HOPS` (16) instead of a delta |
| Tier 1 — the chain-terminating write | ✅ `wo_wal_append_row_image`, encoded as `WO_WAL_UPDATE` — see the correction below |
| Tier 2 — absolute garbage term | ✅ `WO_CKPT_ABS_BYTES` (64 MiB) triggers regardless of proportion |
| Tier 2 — proportional ceiling | ✅ **removed as dead code** — the absolute term already does this job |
| **Tests** | ✅ four tests; `test_wal` 5700 pass / 0 fail, `just wovm-test` and `just woc-test` green |

**Two corrections the tests forced, both worth recording.**

*The flattened record is a `WO_WAL_UPDATE`, not an `INSERT`.* The reasoning for
INSERT was that a chain's base must be a full row, and INSERT is what compaction
writes. That holds for compaction, which builds a *fresh* log. It is wrong for an
update appending into a *live* one: the row's original INSERT is already in that
log, so a second INSERT for the same id is a duplicate, and replay correctly
refuses it as corruption. `test_delta_chain_flatten_replays` failed on exactly
that. UPDATE replays as remove-then-recreate and the fold terminates on either
full-row kind, so nothing else changed.

*The proportional ceiling was unreachable, and is gone.* With the absolute term
at 64 MiB and the ceiling at 256 MiB, any garbage large enough to reach the
ceiling had already tripped the absolute term — the branch could never execute.
Found by trying to write a test that exercised the ceiling and discovering no
input could. PostgreSQL needs both constants because it thresholds on *tuples*
with its pair at opposite ends (base 50, max 1e8); this thresholds on *bytes*,
where one constant does both jobs. Any ceiling above the absolute term is dead,
and any below it would simply be the trigger.

**Verified by construction where tests do not reach.** Both update entry points
converge on `row_apply_field_keys` (`table.c:1039` and `:1319`), so one branch
covers both. The re-point is transparent to flattening because `db.c` captures
`wo_wal_next_offset(w)` *before* calling into `table.c` — it targets wherever the
next record lands, delta or full row alike.

## Acceptance Criteria

All verified but one, which is narrowed rather than dropped. Tests live in
`runtime/test/test_wal.c`.

- ✅ **Given** a row updated K times, **when** updated once more, **then** the
  record its offset names is a full row and its chain length is zero.
  *`test_delta_chain_flattens_at_k`.*
- ✅ **Given** a row updated far more than K times, **when** it is read, **then**
  it performs at most K + 1 record reads, asserted by counting rather than
  timing. *Both chain tests assert `scratch_hops <= WO_DELTA_MAX_HOPS` on every
  read, which is the count itself, not a proxy for it.*
- ✅ **Given** the same row, **when** the process restarts, **then** replay is
  correct and its cost does not grow with the total updates ever applied.
  *`test_delta_chain_flatten_replays`.*
- ⚠️ **Given** a flattening update, **when** replayed, **then** the row matches
  the same row in a `resident: all` table under the same update sequence.
  *Asserted against an expected value, not against a `resident: all` oracle
  table. Weaker than written: it catches a wrong value, but it would not catch
  the two modes disagreeing in a way that also fooled the expectation.*
- ✅ **Given** a flattening update to an indexed column, **when** queried through
  that index, **then** the row is found by its new value and not its old, before
  and after a restart. *`test_keys_resident_indexed_across_flatten`, checked at
  every step across the bound, not only at the end.*
- ✅ **Given** reclaimable bytes past the absolute threshold but inside the
  ratio, **when** the policy is evaluated, **then** compaction fires.
  *`test_should_compact_absolute_and_ceiling`, which also pins the boundary just
  under the term and the small-log case where the ratio still governs.*
- ✅ **Given** a `resident: all` table, **when** any of this runs, **then**
  nothing about its behaviour or log records changes. *Regression only: the
  existing 856 `test_table` and 5700 `test_wal` assertions pass, and a
  `resident: all` table never reaches `row_apply_field_keys`.*

**Honest note on what the new tests found:** no product defect in the
indexed-column path. Both failures during that test's development were bugs in
the test itself — reading a folded row as a `wo_str` when the fold yields engine
`db_text`, and probing a database whose WAL had been closed. The result is still
worth having: it is the only coverage that the index and the flatten branch
compose, and it now pins that.

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
