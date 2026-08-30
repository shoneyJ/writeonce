# Bounding a keys-resident row's delta chain

Design settled 2026-08-30. Fixes the limitation shipped with
[databasev2 2](../../stories/databasev2/02-table-storage-modes.md)'s delta
updates and recorded in
[`2026-08-30-keys-resident-delta-updates-design.md`](2026-08-30-keys-resident-delta-updates-design.md).

## The problem, and why the shipped mitigation does not fire

An update to a `resident: keys` row appends a delta — one field's new value plus
a back-pointer. Reading the row folds the chain backward, so a read costs
`1 + chain length` preads, and replay costs **O(N²)** per chain because it folds
once per delta and each fold walks back to the base.

The shipped design chose not to cap chain length, on the reasoning that
compaction flattens every chain and that deltas grow the log, pulling the next
checkpoint forward. **That reasoning is wrong for the case that matters.**
`wo_wal_should_compact` decides on `used > last * ratio` — a byte ratio over the
whole log. It cannot see that one row has a five-thousand-delta chain. A single
hot row taking many small updates barely moves that ratio in a large database,
so the checkpoint never fires, that row's chain grows without bound, and its
replay cost grows as the square.

The motivating workload is precisely this shape: one popular SKU whose stock
moves on every order while the rest of the catalogue sits still.

## What PostgreSQL does, read from source

Verified against `.dev/reference/postgresql`, not recalled. Postgres solves the
same class of problem — chains of row versions that must be collapsed — with
**two tiers**, and neither is a size ratio.

**Tier 1, `heap_page_prune_opt` in `pruneheap.c`** — opportunistic and local.
Three gates, cheapest first: an O(1) `pd_prune_xid` hint stored on the page; a
visibility test; then `PageIsFull(page) || PageGetHeapFreeSpace(page) < minfree`
where `minfree = Max(fillfactor target, BLCKSZ / 10)`. The work happens on a
page the process **already holds** because it is reading or updating it anyway.
The source is explicit that the check is deliberately approximate — it reads
free space without taking a lock, because "avoiding taking a lock seems more
important than sometimes getting a wrong answer in what is after all just a
heuristic estimate."

**Tier 2, autovacuum** — background and per-table:
`vacthresh = vac_base_thresh + vac_scale_factor * reltuples`, clamped by
`autovacuum_vacuum_max_threshold`. Shipped defaults are 50, 0.2 and 100 000 000.
A **count** with a floor, a proportional term and a ceiling — computed per
table, never per database.

Three lessons, and one correction to our own vocabulary:

- **Do the work while you already hold the thing.** That is the whole of tier 1.
- **The floor exists to catch what the proportion hides.** `base_thresh = 50`
  fires on a small table where 20% would not. `Max(…, BLCKSZ/10)` does the same
  for space.
- **The ceiling exists so scale does not defer forever.**
- **Our "floor" is the opposite of theirs, despite the name.**
  `wo_wal_should_compact` reads `if (used < floor) return 0` — ours *suppresses*
  compaction on a small log. Postgres's floor *triggers* cleanup on a small
  absolute problem. We have the proportional term and the suppressor; we have
  neither the triggering floor nor the ceiling.

Postgres also, notably, does **not** threshold on "new bytes versus old bytes",
despite knowing exactly what every chain costs. Its space check is "will the
next version physically fit", a hard operational constraint, not an economic
comparison. That rules out the byte-ratio shape for us as well — and our cost is
worse suited to it still, since each hop is one `pread` whose cost barely varies
with the bytes it carries.

## Tier 1 — flatten on update

**The update path already folds the row.** It must: it needs the old values to
maintain indexes. And the fold already walks the chain hop by hop. So it can
report how many hops it took, and the update path learns the chain's depth for
free — no new record field, no extra read, no per-row RAM. That reported hop
count is our `pd_prune_xid`: the cheap signal that says whether work is worth
doing, obtained from something we were doing anyway.

The rule is one branch. When the fold reports a depth at or beyond **K**, the
update appends a **full-row record** instead of a delta, and the chain resets to
zero. Otherwise it appends a delta as today.

Consequences:

- A read costs at most **K + 1** preads, always, independent of when a
  checkpoint fires.
- Replay costs **O(K²) per row**, bounded rather than unbounded.
- Write cost rises by one row-sized record per K updates — amortised, under
  `1/K` extra bytes against today.
- Compaction, replay and the fold are untouched. A full-row record is a shape
  all three already handle, because it is what an insert writes.

**K is a fixed constant, not a per-table knob.** Postgres ships `fillfactor` and
`base_thresh` as documented constants that are rarely tuned, and that is the
right precedent: K is a *bound*, not a dial. Anything from 8 to 64 caps the
pathology, and being wrong by a factor of two costs one extra row-write per K
updates.

**K does NOT scale with table size, and that is deliberate.** Postgres scales its
threshold by `reltuples` because it thresholds a table-level aggregate whose harm
is proportional. Ours is a per-row property with additive cost: reading one
product costs `1 + depth` preads whether the catalogue holds a hundred rows or
ten million, and total replay is the sum over every row's chain. Scaling K up
with table size would make the largest databases boot worst — exactly backwards.

**Row width is the one thing that might justify varying K**, since flattening
writes a whole row while a delta writes one field, so the write-amplification
break-even genuinely depends on row size. Deliberately **not** done now: hop
count is what bounds read and replay cost, which are the costs actually hurting,
and width would optimise only the write side. Revisit if measurement shows write
amplification matters.

## Tier 2 — give the checkpoint the trigger shape it is missing

Smaller, and separable from tier 1. Tier 1 bounds one row; tier 2 corrects the
whole-log policy's shape so it stops being blind to absolute garbage.

`wo_wal_should_compact` gains, alongside its existing ratio:

- **An absolute garbage term** — compact when reclaimable bytes exceed an
  absolute threshold regardless of ratio. This is postgres's `base_thresh`, and
  it is what our current "floor" is not.
- **A ceiling** — cap the proportional term so a very large live set does not
  defer compaction indefinitely. This is `autovacuum_vacuum_max_threshold`.

> **Implementation outcome (2026-08-30): the ceiling was built and then removed
> as dead code — the absolute term above already does its job.** Borrowing both
> constants from postgres was the wrong inference. Postgres needs two because it
> thresholds on *tuples*, with its pair at opposite ends of the range (base 50,
> max 1e8). This design thresholds on *bytes*, and "compact once garbage exceeds
> X bytes" is itself a cap on deferral: with the absolute term at 64 MiB, any
> garbage large enough to reach a 256 MiB ceiling has already tripped it, so the
> branch is unreachable. Any ceiling above the absolute term is dead; any below
> it would simply be the trigger. Caught by trying to write a test for the
> ceiling and finding no input could reach it. Do not reintroduce it without
> also changing what the absolute term means.

The existing floor keeps its current meaning — do not bother with a tiny log —
but the doc comment must stop calling it a floor in postgres's sense, because it
does the opposite thing.

## Acceptance criteria

- **Given** a keys-resident row updated K times, **when** it is updated once
  more, **then** the record its offset names is a full row, not a delta, and its
  chain length is zero.
- **Given** a row updated many times more than K, **when** it is read, **then**
  the read performs at most K + 1 record reads — asserted by counting, not by
  timing.
- **Given** a row updated many times more than K, **when** the process restarts,
  **then** replay reconstructs it correctly and its cost does not grow with the
  total number of updates ever applied to it.
- **Given** a flattening update, **when** it is replayed, **then** the row is
  identical to the same row in a `resident: all` table subjected to the same
  update sequence. The resident table is the oracle.
- **Given** a flattening update that changes an indexed column, **when** the row
  is queried through that index, **then** it is found by the new value and not
  the old — before and after a restart.
- **Given** reclaimable bytes past the absolute threshold but within the ratio,
  **when** the policy is evaluated, **then** compaction fires. *(Tier 2.)*
- **Given** a `resident: all` table, **when** anything here runs, **then**
  nothing about its behaviour or its log records changes.

## Out of scope

- Varying K by row width. Reasoned above; revisit only with a measurement.
- A time-based compaction trigger. Records are durable at commit, so an idle log
  does not grow — the existing design's reasoning still holds.
- The mid-drain stale-read limitation, which a separate fix already closed.
- Whether `resident: keys` is worth having at all. That is
  [databasev2 2](../../stories/databasev2/02-table-storage-modes.md)'s task 7,
  and this design does not answer it.

## Risks

- **K is a constant chosen without measurement.** The bound is right in shape;
  its value is a judgement. The mitigation is that being wrong is cheap and
  symmetric — too small costs write amplification, too large costs read latency,
  and neither is a correctness failure.
- **Flattening makes one update in K expensive.** A burst of updates to one row
  pays a row-sized write on every Kth. Acceptable, and the alternative is an
  unbounded read path, but it should be visible in the measurement rather than
  discovered in production.
