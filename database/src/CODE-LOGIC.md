# database/src — how the engine hangs together

The database engine is its own top-level directory, statically linked into
every `wovm` and every runtime test binary (`runtime/Makefile`'s `DBSRC`).
One binary, unchanged. Format doc: `docs/plan/oop-vm/04-db-binding.md`.
Memory-safety doctrine: the 9b design's section 6.

## table.c — rows (iteration 9, Task 1)

```
VM values ──copy──▶ row slots (engine-owned malloc) ──copy──▶ fresh VM values
        wo_row_insert                              wo_row_read
```

- **No VM pointer ever enters a slab; no slab pointer ever leaves.** Encode
  copies per kind (Texts to `db_text`, owned objects flattened recursively to
  `db_rec`, containers element-wise); decode allocates fresh VM values from
  the caller's `wo_rt`. The GCREF kind is refused at encode — the compiler
  should have made that impossible (the GC bulkhead), the engine refuses it
  anyway.
- **Rows never move.** Slabs of 256 are malloc'd and kept for the table's
  life; the free-slot list recycles removed slots before any slab grows;
  the id hash maps id → slot. Ids are never reused (per-table counter,
  shard-interleaved `S+1, S+1+N, …`), which is also what makes the hash's
  tombstone sentinel safe.
- **Choke points**: `wo_row_insert` / `wo_row_remove` carry the `INDEX HOOK`
  comments where Task 4's secondary indexes attach and Task 2's WAL stages
  its record. Nothing else may mutate storage.
- One deliberate file-static: `g_classes` for recursive frees (`db_val_free`
  has no context parameter). One process, one class table; revisit at
  iteration 8 (shards share the same immutable table).

## wal.c — durability (iteration 9, Task 2)

The commit order IS the module: RAM apply → stage → one pwrite + one
fdatasync → ack. `wo_wal_commit` returning 0 is the only thing "durable"
means. Replay never touches the VM heap — payloads decode straight into
engine-owned values and re-enter through the row API, so whatever hooks the
choke points (indexes, Task 4) applies to replayed rows identically. Torn
tails end the intact prefix and get overwritten by the next commit;
CRC-valid-but-undecodable records fail replay loudly (corruption is not a
tear). The crash battery in `runtime/test/test_wal.c` is the module's
meaning proven: acked-over-a-pipe after commit, SIGKILL mid-stream, replay,
zero acked-but-missing.

## db.c — statement executors (iteration 9, Task 3)

One dispatcher, the builtin contract (0 ok, else WO_T_* + msg). The engine
handles ride `wo_rt.db` / `wo_rt.wal` as opaque pointers set by main.c —
NULL db traps WO_T_DB, NULL wal means RAM-only (the corpus's mode; WO_DATA
opts into durability). Insert's contract: RAM apply through the row API,
then stage + commit BEFORE returning — the builtin's return is the
acknowledgment, so a failed commit un-applies the row and traps WO_T_IO
rather than acknowledging what disk never got.

## Verifying a change

- `make -C runtime test` — `test_table` is this directory's suite (round
  trips across kinds, nil encodings, shard interleave, slab growth, slot
  reuse, misuse), ASan+UBSan like every runtime test.
- `just oop-e2e`, `just log-watcher` — regression that linking the engine
  into wovm changed nothing observable (it is dead code until Task 3 wires
  the first builtin).

## The slot-level surface (arc stage 3, 2026-08-21)

- **Why it exists:** the transparent DB actor executes a worker's
  statement on the owner shard, and VM heaps are never read cross-shard —
  so the requester encodes to engine slots on its own thread and the owner
  executes from slots, exactly the shape WAL replay already used.
- `wo_db_val_encode` exposes the in-gate for the RPC marshaler;
  `wo_db_val_clone` deep-copies an engine value (a get-field reply must
  outlive the row: a later serialized statement may free the slot);
  `wo_row_insert_slots` / `wo_row_update_field_slot` are the pre-encoded
  twins of insert/update (slot values consumed either way — installed on
  success, freed on failure); `wo_db_exec_req` (db.c) mirrors
  `wo_builtin_db` case for case with slot inputs and plain outputs, so a
  worker sees byte-identical traps and messages.
- The update refactor extracted `row_apply_field_slot` (the post-encode
  half: unique shadow-check, index fix-up, slot swap) shared by both
  entry points — the VM-value path's behavior is unchanged bit for bit.

## The read-path index probe (2026-08-22)

- **wo_idx_probe** (table.c) answers a single-column equality from the
  index's hash buckets instead of walking slabs — the O(1) wiring the
  db-bench numbers demanded (reads were ~1.5k ops/s at p50 600µs on 20k
  rows; ~1.3M ops/s at p50 1µs after). `idx_hash_key1` must reproduce
  `idx_hash`'s single-column result bit for bit (same FNV over text
  bytes, same float canonicalization, same position mix) or probes and
  maintenance disagree on the bucket and rows silently vanish.
- The VERIFY step compares exactly as the slab walk compared (raw words
  for scalars/floats, byte equality for text; nil text == NULL bytes) —
  the hash canonicalizes only to FIND the bucket, so probe results are
  identical to scan results by construction.
- Composite indexes refuse (return 0) and callers keep the slab walk;
  both probe executors (`wo_builtin_db` and `wo_db_exec_req`) carry the
  same wiring, so worker shards get the speedup through the DB actor.
- The COMPILER half (emit.ml `probe_key_of_where`): a query whose where
  list contains `var.col == key` on a single-column-indexed column
  lowers its source to DB_PROBE; every where guard still runs over the
  candidates, so the guard — not the engine — stays the final arbiter.
  Keys are a plain identifier or an integer literal only; Float/Bytes
  columns excluded (engine raw-eq is narrower than VM float-eq, and a
  probe miss cannot be resurrected by a recheck). Pinned by
  `tests/corpus/run/query-index-probe`.

## Group commit: one barrier per drain (databasev2 4 part A, 2026-08-28)

**What changed:** the engine used to commit per *statement*. `db.c` called
`wo_wal_commit` immediately after every append, at all six sites, so each row
change bought its own `pwrite` and its own `fdatasync`. Now the barrier belongs
to the drain, not to the statement.

**Where the barrier runs, and why there.** A statement on a worker shard has no
WAL to write — the runtime asserts workers hold neither `db` nor `wal` — so it
marshals to shard 0 and parks. Shard 0 executes those requests in its envelope
drain (`wo_vm_adopt`), and the drain now **holds each reply** instead of pushing
it as the statement finishes. When the queue empties it issues one barrier, then
releases every held reply.

Holding the reply is the whole mechanism. Pushing it early would unpark the
requester before its record was durable; holding it means each writer is
acknowledged after the barrier that carried *its own* record. That was always
the intended contract — it was simply true by accident before, because every
batch had exactly one member.

**Why the queue is the boundary.** Not a tick, and not a timer. A queue of one
gives a batch of one, so a lone writer pays exactly what it paid before; the
batch grows only when writes genuinely contend. A tick boundary would have
added latency even with nothing to batch against, which is taxing an idle
system to serve a busy one. There is nothing to tune, which is the point.

**Why the inline path is asymmetric.** A statement already on shard 0 stages and
commits before returning, batch size one. It cannot hold a reply because there
is nobody to reply to — it returns into its own fiber. Batching it would mean
parking that fiber on the barrier, which is part B's machinery. Two consequences
worth keeping in mind: single-shard configurations get no batching at all, by
design; and the inline commit is only safe because the drain commits
*unconditionally* whenever anything is staged, so the buffer is empty when an
inline statement runs. If that ever stops holding, the inline path would make
another statement's record durable early and acknowledge it to the wrong writer.

**One rule for failure: once a statement has mutated RAM, the outcomes are
durable or process death.** It replaced three behaviours that disagreed —
`insert` un-applied itself, while `update` and `delete` returned a catchable
trap and left RAM ahead of disk, which their own comments said out loud.
Batching would have multiplied that from one row to a whole batch. So a failed
stage or a failed barrier now prints one diagnostic (operation, log path,
`errno`, record count) and exits 3; `WO_T_IO` is unreachable from a write.
Retrying is not offered because it is unsound: on Linux a failed `fsync` may
already have discarded the dirty pages, so a second call can report success
having written nothing. Replay is the recovery that works.

**Measuring it.** `WO_WAL_STATS=1` makes the runtime print one line at exit —
batches, records, peak batch, peak staged bytes. Opt-in, because it would
otherwise pollute every durable program's output. The counters live in `wo_wal`
rather than behind a builtin: they are diagnostic, not part of the language.
`db-bench`'s `wmix N C` leg exists to exercise this at all — `mix` writes on one
op in ten with C=4, which produced a measured mean batch of 1.01, so it could
never have shown whether batching worked.

**If you are looking at this because writes got slower**, check the mean batch
first. Mean 1.0 means the mechanism is not engaging, which is expected for a
serial writer or a single-shard configuration and a bug anywhere else.

## Checkpoint: compaction by rewrite + rename (databasev2 3, 2026-08-29)

**The problem:** nothing ever removed superseded records, so the log grew
forever and boot replayed all history. Measured before this: 20 000 rows seeded
gave a 986 KB log; updating those same rows 20 000 times took it to 2.6 MB with
**the same live data**.

**Why one file and not a snapshot plus a tail.** Postgres does the opposite —
its WAL is a redo tail and the data lives in heap files, so a checkpoint flushes
pages and then recycles log segments; it never compacts. It cannot: its records
are page deltas, so a compacted redo log is not a store. **Ours are full row
images** — `apply_record` implements UPDATE as remove-then-recreate — so a log
of one record per live row *is* a complete store. That single difference deletes
the control file, the redo pointer, the second recovery source and the separate
process from this design. Recovery is not merely compatible with compaction; it
is completely unaware of it.

**Why `rename` is the whole crash-safety story.** The dump goes to a temp file,
which is fsynced, renamed over the live log, and then the parent directory is
fsynced (the rename is atomic in-kernel, but the directory entry is not durable
until the parent is — Postgres does the same for the same reason). Before the
rename the live log is intact and the temp is not authoritative; after it the new
log is complete. There is no instant at which a reader sees a mixture, so this
needs no recovery logic of its own. What Postgres achieves with a redo pointer
computed at checkpoint start and a control file written at the end, one syscall
achieves here — because we can swap the entire data set atomically and Postgres
cannot.

A crash mid-rewrite leaves a temp file. The next open **removes it**, and it is
deleted rather than ignored because a file full of well-formed records sitting
beside the log is exactly what a later reader mistakes for data.

**Why the dump flushes periodically, and why it does NOT fsync when it does.**
`stage()` grows the staging buffer by doubling and never shrinks it, so pushing a
whole store through one buffer would hold the entire store in RAM on top of the
store — the unbounded growth databasev2 1 measured as how this engine dies. So
the dump flushes every 256 records. It flushes with a plain write, **not** a
commit: intermediate durability is worthless because the temp is not
authoritative until the rename and is fsynced once immediately before it. Using
the committing path cost one barrier per 256 records and made the pause 8×
larger — measured 107 649 µs against 13 212 µs for a 2 MB live set, ~22 MB/s
against ~181 MB/s.

**Why the replacement is preallocated like the original.** The WAL is
preallocated so that appends never extend the file, which is what lets
`fdatasync` alone serve as the ack barrier. A replacement opened without it
would silently change that property, and the zero-padded tail the open-time scan
relies on.

**When it runs.** Only where the staging buffer is empty — right after a
barrier. Both write paths check: the drain (`vm.c`, after its commit and after
releasing held replies, since those records are already durable and should not
wait out a rewrite) and the inline path (`db.c`). Wiring only the drain left
`WO_SHARDS=1` never compacting, with its log growing forever: measured 536 KB
where the multi-shard run held 446 KB.

**The trigger** compares the log against what the *last* compaction actually
wrote, with an absolute floor. The denominator is measured rather than
estimated, because estimating the live size means estimating Text and the
compactor already knows the true number. There is deliberately **no timer**:
Postgres needs one because its dirty buffers are not durable until flushed, and
ours are durable at commit — an idle log does not grow.

**A failed compaction is a missed optimisation, not a durability event.** It
leaves the original log intact and returns an error the callers ignore. It must
never take `wo_wal_commit_fatal`'s path, which exists for a different problem.

**If you are here because a checkpoint misbehaved:** `WO_WAL_STATS=1` reports
compaction count, the stop-the-world pause (max and total) and the last
compaction's size. `WO_CHECKPOINT_BYTES` and `WO_CHECKPOINT_RATIO` move the
policy; setting a tiny floor forces compaction in a few writes, which is how the
gate tests it at all.
