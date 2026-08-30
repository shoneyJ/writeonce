# Guide — how a `resident: keys` row lives in the log

For a developer new to this engine. It explains replay, row chains, how a
checkpoint flattens them, and why replay of a long chain costs quadratic time.
Written 2026-08-30 against the code as it stands, not as a specification —
where this disagrees with `database/src/wal.c` and `table.c`, they are right.

Everything here concerns `@table(resident: keys)`. A `resident: all` table —
the default — keeps whole rows in memory and none of this applies to it.

---

## Replay: how the database gets its memory back

This engine is **RAM-authoritative**. Tables live in memory and reads never
touch disk, which is what makes read latency predictable. Durability is a
separate mechanism: every mutation is appended to a write-ahead log before it
is acknowledged. Insert a row and two things happen — it goes into an in-memory
slab, and an `INSERT` record goes into the log.

Now kill the process. Memory is gone; the log file is all that survives. So on
the next start, before running a line of the program, the runtime opens the log,
reads it front to back, and re-applies every record in order — insert, update,
delete, delta — rebuilding the tables and their indexes exactly as they were.
That is replay: `wo_wal_replay_ex` looping over records and handing each to
`apply_record`, which dispatches on the record kind.

Three consequences worth internalising:

- **The log is the database.** RAM is a cache of it that happens to be
  authoritative while the process lives. That is why an acknowledged write must
  reach the log *before* the client is told it succeeded — anything acknowledged
  but unlogged vanishes on restart.
- **Boot time is O(records since the last checkpoint), not O(live data).**
  Insert a million rows and delete them all, with no checkpoint in between, and
  replay still reads a million inserts and a million tombstones to arrive at an
  empty table. Checkpointing is what stops that; see below.
- **Ordering at boot is delicate.** `main.c` runs replay *before* wiring
  `rt.wal`, so replay lends the runtime a temporary read-only view of the log —
  a keys-resident row's index maintenance has to read rows back *during* replay,
  and there is no live WAL to read through yet. Getting that order wrong made a
  perfectly good tombstone replay as corruption; that was a real bug.

A `resident: keys` table has no rows in RAM at all, so without a log there is
nothing to reconstruct from. That is why the runtime refuses at startup when
such a table is declared and `WO_DATA` is unset, rather than letting every read
return "no such row".

---

## The row chain

A **row chain** is the set of log records that together describe one row's
current state, threaded together by back-pointers.

Take a product table — `sku`, `name`, `price`, `stock`. Insert it, then sell
three units in three separate orders:

```
log (grows rightward, offsets increase) ────────────────────────▶

off 100          off 340        off 372        off 404
┌──────────────┐ ┌────────────┐ ┌────────────┐ ┌────────────┐
│ INSERT       │ │ DELTA      │ │ DELTA      │ │ DELTA      │
│ sku  "SKU-1" │ │ field=stock│ │ field=stock│ │ field=stock│
│ name "kettle"│ │ value=9    │ │ value=8    │ │ value=7    │
│ price 2999   │ │ back=100   │ │ back=340   │ │ back=372   │
│ stock 10     │ └────────────┘ └────────────┘ └────────────┘
└──────────────┘        ▲              ▲              ▲
       ▲                └──────────────┴──────────────┘
       └──────────────── back-pointers ────────────────┘

id map in RAM:   SKU-1 → 404      ← only the NEWEST offset
```

Three things to notice:

- **A delta is tiny.** It carries one field, not the row. Changing `stock` does
  not rewrite `name` or `price` — which is the whole reason for the design,
  since a catalogue row is wide and only one narrow field moves per order.
- **RAM holds one number per row.** Not a list of offsets, just the head of the
  chain. That is non-negotiable: the id map is the *only* resident part of a
  `resident: keys` table, so if it grew per update the mode would lose its
  purpose.
- **The chain is a backward-linked list living inside the log.** Nothing indexes
  it; the only way to find a delta's predecessor is to read its back-pointer.

### Reading a row: the fold

Reading means **folding** — start at the head, walk backward, take the *first*
value seen for each field, stop at the first full-row record:

```
404: stock=7   → stock resolved, take 7
372: stock=8   → already resolved, skip
340: stock=9   → already resolved, skip
100: INSERT    → base: fill sku, name, price; stock already resolved
                 result: SKU-1 / kettle / 2999 / 7   ✓
```

First-seen-wins is what makes newest-wins work, because the walk goes
newest-to-oldest. Walk the other direction and the result is `stock=9` —
plausible, wrong, and invisible unless a test uses two updates to the *same*
field.

`wo_wal_fold_row_at` is that fold, and there is exactly one of it. Reads, replay
and compaction all call it. That is deliberate: a fold that differed between
reading and replaying would be a database that changes its mind at boot.

**Chain length is the cost variable.** A read costs 1 + (chain length) `pread`s.
Chain length grows by one per update and resets to zero when a checkpoint
rewrites the row.

---

## How a checkpoint flattens a chain

Compaction is a **rewrite**, not an edit, and flattening falls out of that.

`wo_wal_compact` opens a *new* temporary log, walks every live row, writes each
into it, `fsync`s once, then `rename`s the temp over the real log and syncs the
parent directory. Garbage in the old log — superseded records, tombstones,
whole delta chains — is simply never copied, and disappears when the old inode
is unlinked. A crash on either side of that `rename` leaves either the complete
old log or the complete new one, never a half-written one. That is why it is a
rename rather than an in-place edit.

Walking live rows, compaction peeks each row's current record kind:

- **Not a delta** — the bytes are copied verbatim. A row nobody updated is
  already a single full-row record; re-encoding it would be slower and pointless.
- **A delta** — `stage_flattened_row` folds the chain, using the same fold reads
  use, collecting the newest value per field, and writes **one fresh
  `WO_WAL_INSERT`** containing every field. Not a delta, not the chain: one
  record.

So the SKU-1 example above becomes a single `INSERT` carrying
`SKU-1 / kettle / 2999 / 7`. The map is re-pointed at it and the chain is length
zero, so the next read costs one `pread` instead of four.

Details that matter:

- **The fold is reused, not reimplemented.** If compaction had its own walk, a
  divergence would silently rewrite rows *wrong*, permanently, at checkpoint
  time.
- **Compaction refuses to run while records are staged** (`w->len != 0`), so it
  always sees a fully durable, quiescent log.
- **The flattened record's kind is `INSERT`, not `UPDATE`.** It must replay into
  an empty database, and there is no earlier record for an `UPDATE` to modify —
  that is the point of flattening.

Measured when checkpointing landed: **2.16× space reclaimed, 1.78× faster
boot**, with a stop-the-world pause of 2 651 µs.

---

## Why replay of a long chain is O(N²)

This is the sharp edge, and it is worth understanding as a class of bug rather
than one defect.

A delta carries one field. To rebuild a row in RAM — and to fix up its indexes —
replay needs the *other* fields, which live in earlier records. So when replay
meets a delta, it folds from that delta's back-pointer to reconstruct the row as
it was just before.

Folding at ΔK costs K hops, because the walk must pass every delta between ΔK
and the base. Replay meets every delta in the log, and each fold starts over
from scratch:

```
Δ1 → fold walks 1 record
Δ2 → fold walks 2 records
…
ΔN → fold walks N records
        total = 1 + 2 + … + N = N(N+1)/2  →  O(N²)
```

Each individual fold is linear and looks harmless. The quadratic appears only
when you notice replay performs one *per delta*.

**Why not keep the row materialised while replaying it?** That would make it
linear, and it is blocked by a deliberate choice: keys-resident rows drop their
payload after each record precisely so replay's peak memory stays bounded, which
is the entire point of the mode. Bounded memory and linear time genuinely
conflict here, and the trade has not been revisited.

### Why the checkpoint does not save you here

Flattening resets chains to zero, so in principle N stays small. But *when*
compaction runs is decided by `WO_CHECKPOINT_BYTES` (a floor below which a log
is too small to bother with) and `WO_CHECKPOINT_RATIO` (how much garbage
relative to live data triggers a rewrite). Both are **ratios over the whole
log**. Nothing counts per-row chain length.

So one hot row — a single popular SKU taking thousands of small stock updates —
barely moves that ratio in a large database. The checkpoint never fires, that
row's chain grows without bound, and its replay cost grows as the square of its
length. The guard that bounds replay in general is structurally blind to the one
case that makes replay quadratic.

This is a known, documented limitation of the shipped feature, not a bug to be
surprised by. It is recorded in
[databasev2 2](../stories/databasev2/02-table-storage-modes.md).

### The reviewing lesson

This survived several rounds of review because every fold is correct, every test
passes, and the per-call cost is linear and obviously fine. **Complexity bugs
hide in the caller's loop, not in the function you are reading.**

The tell is a linear helper called once per element of the same structure it
walks — that shape is quadratic every time. Worth checking whenever a helper
starts from a root and a caller invokes it in a loop.
