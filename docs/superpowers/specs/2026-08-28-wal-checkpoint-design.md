# WAL checkpoint — design

> databasev2 [3](../../stories/databasev2/03-wal-checkpoint.md), chain 6.
> Brainstormed and approved 2026-08-28, after
> [databasev2 4 part A](2026-08-28-wal-group-commit-design.md) landed.
>
> **One sentence:** compact the log by rewriting it as one record per live row
> into a temporary file, then `rename` it over the live WAL — so recovery is
> unchanged and crash safety comes from the filesystem.

## Decisions taken (the brainstorm's forks, settled)

| Fork | Decision |
| --- | --- |
| Snapshot format | **None.** The compacted log *is* the snapshot, in the existing record grammar |
| One source or two | **One.** Rewrite + atomic `rename`; boot logic is untouched |
| Trigger | **Volume only**, as a ratio against the last compaction's own size, with an absolute floor. **No timer** — see below |
| Write availability | **Stop-the-world**, measured against a stated budget rather than assumed acceptable |
| Composition with group commit | Compaction runs only where **nothing is staged** — immediately after a barrier |
| `resident: keys` (iteration 2) | Compaction **rebuilds the offset map** as it writes. It cannot be left to discover this later |

## Why one file, and why Postgres cannot do it

Postgres was read for this (`.dev/reference/postgresql`), and the conclusion is
that its design is *unavailable* to us — which is what makes the simpler option
legitimate rather than lazy.

| | PostgreSQL | writeonce |
| --- | --- | --- |
| Where data lives | heap/data files; the WAL is a redo tail | **the WAL is the only durable form**, replayed into RAM |
| WAL contents | page deltas and full-page images | **full row images** — `apply_record` implements UPDATE as remove-then-recreate |
| Compaction | **never**; segments before the redo point are recycled by `rename` or unlinked | possible, because a log of row images *is* a complete store |
| Bounded replay | recovery starts at the redo LSN in the control file | recovery starts at byte 0 of a *shorter* log |
| Crash safety of the switch | control file written in place, full block, torn writes caught by **CRC32C** (`update_controlfile`) | one `rename` |
| Trigger | `CheckPointTimeout` (300 s) **or** WAL volume (`XLogCheckpointNeeded`) | volume only |
| Pause | none; flush is spread over time in a **separate process** | stop-the-world |

Postgres cannot compact its WAL because a compacted redo log is not a store —
its records describe changes to pages that live elsewhere. Ours describe whole
rows, so the compacted log needs no companion. That single difference removes
the control file, the redo pointer, the second recovery source, and the separate
process from our design.

**What is worth porting is not the architecture but the ordering discipline:**
publish the new "recovery starts here" atomically and *last*, so a crash at any
instant falls back to the previous state with nothing to undo. Postgres achieves
that with a redo pointer computed at checkpoint *start* and a control file
updated at the *end*. We achieve the same property with `rename`, in one
syscall, because we can swap the entire data set atomically and Postgres cannot.

**Correction to a prior exploration doc.**
`docs/plan/exploration/postgresql/buffer-and-checkpoint.md` states that Postgres
updates its control file by rename ("the same in `BasicOpenFile` +
`fsync_parent_path`"). It does not — `update_controlfile` opens the existing file
`O_WRONLY`, writes a zero-padded full block in place, and relies on CRC32C to
detect a torn write. That doc also assumes writeonce has **segment files**
("records before that LSN are *known* to be in the segment files"), which it
does not and, per databasev2 2, deliberately will not. The doc predates the
databasev2 direction and should be annotated rather than followed.

## The design

### Compaction

Run on the owner shard, which owns the WAL. Walk each class's live rows — the
bitmap-over-slabs walk that three call sites in `db.c` already perform — and
append one INSERT record per live row to a **new** file, using the existing
append path. No new encoder, no new decoder, no format.

Then: fsync the new file, `rename` it over the live path, fsync the parent
directory (the rename's atomicity is in-kernel; the directory entry is not
durable until the parent is synced — Postgres does the same, and the existing
exploration doc is right about *this* part), and reopen the WAL descriptor,
because the old one now refers to an unlinked inode.

**Every crash point is safe without any recovery logic of ours.** Before the
rename, the live WAL is untouched and the temp file is garbage. After it, the new
log is complete by construction. There is no window in which a reader could see a
mixture, so the acceptance criterion — "recovery produces the same consistent
store as if the checkpoint had never started" — is satisfied by `rename`, not by
code we must get right.

Two obligations follow. Boot must **unlink a stale temp file** if one is present,
because a crash mid-rewrite leaves one behind and it must never be mistaken for
data. And the temp file must be zero-padded beyond its records exactly as the
live WAL is, because the tail scan identifies the end of the log by a zero
length field.

### When it runs, and where in the sequence

**The point matters more than the policy.** The drain stages records into one
buffer and commits them together; compaction rewrites the file those records
would land in. So compaction may run **only when nothing is staged** — in
practice, immediately after a barrier, before the next statement is served.
Anywhere else and a staged record would either be written to a file about to be
replaced, or be lost with it. This is the normative ordering rule that
[`04-db-binding.md`](../../plan/oop-vm/04-db-binding.md) must carry.

**Trigger: volume, as a self-tuning ratio.** Compact when the WAL's used bytes
exceed a multiple of the bytes the *last* compaction wrote, with an absolute
floor so a small store never bothers. The denominator is known exactly — the
compactor wrote it — so this needs no estimate of the live set's size, which is
not cheaply knowable when rows hold Text. The floor exists because a store whose
whole log is a few hundred kilobytes has nothing to reclaim.

**No timer, and that is a deliberate difference from Postgres.** Postgres needs
`CheckPointTimeout` because its dirty buffers are not durable until flushed — an
idle-but-dirty system must still checkpoint or it loses data. Our records are
already durable at commit; a checkpoint reclaims space and shortens boot and
nothing else. An idle system's log does not grow, so a timer would fire with
nothing to do. Adding one would be copying Postgres' mechanism without its
reason.

A manual trigger exists for tests, because a policy that can only be observed by
waiting is a policy that cannot be tested.

### The pause, and how it is judged

Compaction is stop-the-world: the owner shard rewrites the log as one long
operation while no statement is served. This is the simplest correct thing, and
part A's own experience argues for measuring before buying complexity to avoid
it. The dump is O(live rows) encodings plus one write and one barrier, so the
expectation is that it is fast — but an expectation is not a measurement, and
the proof plan below states the budget it must meet.

If the measured pause exceeds the budget, **that is a finding and a follow-up,
not something this iteration solves by adding concurrency.** The alternative
designs (incremental copy, fork-and-dump) cost exactly what Postgres pays, and
should only be bought against a number.

### The interaction that will otherwise be discovered late

**Compaction invalidates every stored WAL offset.** Rewriting the log moves every
record, so any offset captured from the old file is meaningless afterwards — not
stale-but-readable, but pointing at an arbitrary byte of a different file.
[Iteration 2](../../stories/databasev2/02-table-storage-modes.md)'s
`resident: keys` stores exactly such offsets, one per row, and reads rows back
through them.

The compactor therefore **rebuilds the offset map as it writes**: it is emitting
the new records and knows each one's new position, so this is the cheap
direction and the only one that keeps both features usable together. The
alternative — forbidding compaction while any `resident: keys` table is live —
would mean the feature that exists to handle huge tables is incompatible with
the feature that stops their log growing forever.

This is recorded here because iteration 2's storage half is not yet
implemented, so nothing will fail today. It will fail later, in a way that looks
like data corruption rather than a design gap.

## Proof plan

| Claim | How it is proven |
| --- | --- |
| Space is reclaimed | An aged store shrinks. Measured today on master: `seed 20000` gives a 986 614-byte log; 20 000 updates take it to 2 590 262 bytes with **the same live rows**. Compaction must return it to approximately the former |
| Replay is bounded | Boot time on the aged store before and after, recorded. Measured today: boot+verify on that store is 155 ms |
| Crash safety | `kill -9` at many instants *during* compaction, then replay: the store must equal either the pre-compaction or post-compaction state, never a mixture, and no acked write may be missing. This is the criterion the whole design is shaped around, so it gets the crash battery's treatment rather than one case |
| A stale temp file is harmless | Boot with one present, containing plausible records: it is removed and never read |
| The pause is known | The stop-the-world pause measured on the largest store the harness builds, recorded as a number with a stated budget — not asserted to be acceptable |
| The ordering rule holds | Compaction with records staged must be impossible by construction; a test that stages and then requests compaction must find it deferred, not executed |
| Nothing regressed | The full battery, and specifically part A's `wmix` legs: compaction must not change the ack contract or the batching it introduced |

## Out of scope

- **A second file, a control file, or a redo pointer.** The Postgres shape,
  priced above and not needed once the log is self-sufficient.
- **Avoiding the pause.** Incremental or forked dumps are bought against a
  measurement, not in advance.
- **Per-shard compaction policy.** One owner shard owns the WAL today; when that
  changes, this decision is revisited with it.
- **Compacting away tombstones across shards, or any cross-shard coordination.**
  There is one log.
- **io_uring for the rewrite** — part B of databasev2 4, whose premise is
  already under revision.
- **Changing the record grammar.** The entire argument for this design is that
  the grammar already suffices.

## Alternatives rejected

**Snapshot + WAL tail (the Postgres shape).** Rejected because it buys write
availability at the cost of a second recovery source, a cutoff offset, a control
file with its own torn-write detection, and a crash-safety guarantee that
depends on our ordering rather than on `rename`. Postgres pays this because its
log cannot stand alone; ours can.

**Compacting in place.** Rejected outright: there is no crash point at which a
partially rewritten live log is recoverable, and it trades the one property that
makes this design defensible for nothing.

**A timer trigger.** Rejected with a reason rather than on taste: Postgres' timer
exists to bound data loss from unflushed buffers, and we have no unflushed
buffers. An idle log does not grow.

**A ratio against an estimated live-set size.** Rejected in favour of the last
compaction's measured output, because estimating the live size means estimating
Text, and the compactor already knows the true number.
