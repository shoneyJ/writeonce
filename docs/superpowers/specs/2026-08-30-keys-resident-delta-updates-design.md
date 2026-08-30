# `resident: keys` updates — delta records folded on read

Design settled 2026-08-30. Closes the last gap in
[databasev2 2](../../stories/databasev2/02-table-storage-modes.md), which is
the only reason the loader still refuses `resident: keys` outright.

**Supersedes** the one-line sketch in
[`2026-08-26-table-residency-design.md`](2026-08-26-table-residency-design.md)
— "Update: append a new record, repoint the offset". That was written before
any of the mode was built, and it describes the full-row shape this design
rejects.

## The workload that decides the shape

The docs used to motivate this mode with a 120 GB audit table. That was a bad
example, because an audit log is append-only and therefore argues for nothing:
if rows are never updated, the missing update path costs nothing and the mode
could simply have been declared append-only.

The real case is a **product catalogue**, and it is the one in
`docs/examples/residency`. Its shape decides everything below:

- The id is stable. A SKU does not change.
- Other fields change constantly. Every order moves `stock`.
- Reads are hot. Browsing a catalogue is the dominant operation.
- Rows are wide. Name, price, description, and whatever else a shop carries.
- The table is the one that outgrows RAM first, which is why it wants this mode.

So an update touches **one narrow field of a wide row, frequently, on the
hottest write path a shop has**, while reads must stay fast. Appending the whole
row per sale would rewrite every field to move one integer. That is the argument
for a delta.

## The design

### The record

A fourth WAL record kind beside `WO_WAL_INSERT` (1), `WO_WAL_REMOVE` (2) and
`WO_WAL_UPDATE` (3). It carries the class id, the row id, the field index, the
encoded new value, and a **back-pointer**: the log offset of the record this one
supersedes.

The back-pointer is what keeps the id map at exactly one slot per row. That is
not an optimisation — a map that grew per update would defeat the mode's entire
premise, which is that the map is the only resident part.

### The fold, and why it lives in one function

Reading a row means walking the chain **backward** from the newest record,
taking first-seen-wins per field, until a full-row record terminates it, then
materialising that base and overlaying the collected fields.

Three callers need exactly this: reads (`wo_row_borrow`), replay, and
compaction. They must agree perfectly — a fold that differs between reading and
replaying is a database that changes its mind at boot. **This is the design's
principal risk**, and the mitigation is that the fold is written once and all
three call it. No caller may implement its own walk.

### Update

Borrow the row, which folds it. That read is needed regardless: index
maintenance requires the old value, and this design allows indexed columns to
change (see below). Then append the delta, commit, and re-point the map at the
new record.

The two existing entry points — the VM-value path and the engine-value slot
path — converge on one internal append. They currently both refuse keys tables;
that refusal is replaced, not duplicated.

### Indexed columns may change

Allowed deliberately. On a product table `price` is exactly the kind of column
you would index, so forbidding it would be a restriction users meet immediately
rather than a theoretical one.

The cost is that index maintenance runs on every delta: the old value comes from
the row just folded, the new one from the delta itself. `@unique` probing
already borrows each bucket candidate, and those borrows now fold — so a unique
check against a heavily-updated table walks chains. Disclosed, not hidden.

### Crash safety

Commit precedes re-point, and that ordering is the whole argument:

- Crash **after** the barrier, before the re-point: the delta is durable, the
  in-memory map is stale, and replay rebuilds the map from the log — which sees
  the delta and lands on the same state. Correct.
- Crash **before** the barrier: the delta never reached the log and the row is
  unchanged. Correct.

Under group commit an update stages like an insert, and the re-point defers to
the post-barrier flush — a pending-repoint list mirroring the pending-drop list
that already exists in `wo_wal` (`pend`/`pend_len`/`pend_cap`).

### Compaction is the bound

Compaction already walks live ids and rewrites each row. It now **folds** each
row and writes a single full-row record, so every checkpoint resets every chain
to length zero.

This is what makes read cost bounded without a cap: a read is
`1 + deltas-since-the-last-compaction` preads, and deltas grow the log, which
pulls the next compaction forward on the existing `WO_CHECKPOINT_BYTES` and
`WO_CHECKPOINT_RATIO` policy. The workload that lengthens chains is the same one
that triggers the fold that shortens them.

**Settled deliberately: no hard chain cap.** The self-limiting property above is
real, and a cap is a number nobody would tune. The risk this accepts is stated
under Risks.

### Replay

A delta at replay re-points the row's offset, exactly as a dropped payload
does today. Because indexed columns may change, replay must also fold the delta
into the indexes it built from the base record — so boot work grows with chain
length. Chains are short by the compaction argument, and this is the same fold
function reads use.

## What this does NOT change

- The id map's shape — one offset per row, unchanged.
- The insert path, the read path for unmodified rows, deletes, scans, or
  checkpoint survival. All of those shipped and are tested.
- `resident: all`. Nothing here touches the fully-resident path.

## Risks

- **The three folds drifting apart.** Mitigated by one function, and the
  acceptance criteria below test the same row through all three paths rather
  than testing the fold once.
- **One hot row under an otherwise quiet write rate.** This is the shape that
  breaks the compaction argument: a single SKU selling far faster than the rest
  lengthens its chain without growing the log enough to trigger a checkpoint.
  Deliberately not solved with a cap. **Task 7's benchmark must include it**,
  because it is the case where this design is worst and it is a realistic
  inventory shape, not a contrived one.
- **Unique probing on a hot table.** Each bucket candidate now folds. Bounded by
  the same checkpoint argument, but it multiplies: candidates times chain length.

## Acceptance criteria

- **Given** a keys-resident row, **when** one field is updated, **then** a read
  returns the new value for that field and the original values for every other.
- **Given** a row updated N times, **when** it is read, **then** the result
  equals the same row's value in a fully-resident table subjected to the same
  updates. The resident table is the oracle.
- **Given** a row with a delta chain, **when** the process restarts, **then**
  replay produces the same row the pre-restart read produced.
- **Given** a row with a delta chain, **when** compaction runs, **then** the row
  reads identically afterwards and its chain is length zero.
- **Given** a delta that changes an indexed column, **when** the row is queried
  through that index, **then** it is found by the new value and not by the old —
  before and after a restart, and before and after a compaction.
- **Given** a crash between the commit barrier and the re-point, **when** the
  process restarts, **then** the update is present.
- **Given** an update to a `resident: all` table, **when** it runs, **then**
  nothing about its behaviour or its log records changes.
- **Given** `resident: keys` on a table, **when** the program loads, **then** it
  is **accepted** — the loader refusal is lifted by this work, and that lift is
  the deliverable.

## Out of scope

- A hard delta-chain cap, and any read-triggered fold. Settled above.
- Changing the compaction policy knobs. They already exist and this design
  leans on them unchanged.
- `resident: keys` performance measurement. That is
  [databasev2 2](../../stories/databasev2/02-table-storage-modes.md)'s task 7,
  and it decides whether this mode is worth having at all — a question this
  design does not answer and must not be read as answering.
