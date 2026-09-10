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
- **Storage is per-table since databasev2 2.** `@table(durable: false)` sets
  `WO_CLASSF_VOLATILE` in the class descriptor (`.wob` v7), and `db.c`'s
  `table_is_durable` gates all three mutation sites: a volatile table stages
  nothing, so it pays none of the fsync cost and is empty after a restart.
  Measured: 50 inserts wrote 1500 WAL bytes durable, **0** volatile. The three
  sites stayed three — the predicate is one function, not an inlined condition,
  precisely so this file's "nothing else may mutate storage" claim keeps
  holding.
- **A mode mismatch refuses, it does not convert.** If the log holds records
  for a class the loaded image now declares volatile, `apply_record` returns
  **-2** (distinct from -1 corruption) and `wo_wal_replay_ex` reports the class
  id so `main.c` can name it. Silently skipping those records would resurrect
  nothing but would also hide a real migration; silently applying them would
  load rows into a table declared not to have any. `wo_wal_replay` remains as
  the NULL-out-param wrapper so the 156 WAL unit checks are untouched.
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

**Where the log lives (databasev2 7, 2026-09-10).** `wo_wal_resolve_data_path`
turns `WO_DATA` into the log path before main.c opens anything: an existing
directory or a trailing `/` → `<dir>/shard-0.wal` byte for byte (the pre-7
form, `//` after a trailing slash included); anything else IS the log —
opened if a regular file, created by `wo_wal_open` if absent. Two refusals,
exit 2, one stderr line each, worded in main.c from the resolver's codes:
`WO_WAL_PATH_NO_PARENT` (the parent comes back in `out`, so the line names
the path AND the parent; no `mkdir -p` — a typo must not plant a store
somewhere unexpected, the operator creates directories, the runtime never
does) and `WO_WAL_PATH_NOT_A_FILE` (fifo, socket, device).
`WO_WAL_PATH_TOO_LONG` refuses what the old 512-byte `snprintf` silently
truncated. A trailing slash on a MISSING directory is still the directory
form and still fails at `wo_wal_open` (`cannot open`), unchanged on purpose.
Nothing below main.c knows which form was used: compaction and migration
build `<log path>.compact` and fsync `parent_dir_of(log path)` — the same
static helper the resolver's parent check uses, so the directory checked at
boot is the directory synced after every rename. Tests:
`test_resolve_data_path` (every arm of the rule, fifo via `mkfifo`) and
`test_file_form_temps_beside_log` (a directory planted at `<file>.compact`
makes compaction and migration refuse with the log untouched; removed, both
succeed and the file is the only artifact beside a decoy sibling directory).

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

## Keys-resident updates: read-modify-append, stage-here/commit-in-caller (databasev2 2/3, 2026-08-30)

**The shape.** A keys-resident row has no slab slot to mutate — its payload
lives in the log — so `row_apply_field_keys` (table.c) does read-modify-
**append** instead of a slot swap: borrow (folds the row's current value),
append a WAL delta record (id, field, new value) chained off the row's
current offset via a back-pointer, RAM-apply the index swap. `wo_wal_fold_row_at`
is THE fold — written once, called by every reader (`wo_row_borrow`), by
replay, and by compaction — so a read, a boot, and a checkpoint can never
disagree about a chain's current value.

**Stage-here, commit-in-caller — mirrors insert exactly.** `row_apply_field_keys`
stages the delta but does **not** commit and does **not** move the id map:
table.c applies RAM and appends; `db.c` owns the barrier and the post-barrier
map move, the same split insert already used (`wo_wal_pend_drop` /
`wo_db_flush_drops` for insert; `wo_wal_pend_repoint` / `wo_db_flush_drops`
for update). The caller captures the delta's own offset via
`wo_wal_next_offset()` **before** calling in — insert's own `koff` pattern —
since nothing between that capture and `wo_wal_append_delta` stages any other
bytes on the WAL. `back_off` — the back-pointer a new delta chains from —
checks a PENDING re-point (`wo_wal_repoint_offset1`) before falling back to
the durable `wo_row_offset1`: two updates to the same row staged behind one
drain's barrier must chain to each other, not both to the row's pre-drain
offset, or the first update would be orphaned from the chain.

**The unique shadow-check runs against a THROWAWAY buffer, never `t->scratch`.**
The row under update already occupies the table's one scratch buffer
(`wo_row_borrow` refuses a nested borrow on the same table), so a candidate
probe needs a buffer of its own — `keys_fold_into`, the fold-into-a-caller-
supplied-buffer half of `wo_row_borrow`, bypasses the scratch gate for exactly
this. A candidate updated earlier in the SAME uncommitted drain has its
re-point only pending, so the candidate probe also consults
`wo_wal_repoint_offset1` — and `wo_wal_fold_row_at` itself reads the WAL's
staging buffer (not yet durable) for an offset that falls inside it, so a
same-drain candidate's NEW value is what a real `@unique` clash sees.

**A keys-resident borrow holds ENGINE values, exactly `wo_row_ptr`'s contract
— restored 2026-08-30.** `table.h`'s opening doctrine: "the engine and the VM
heap are two memory worlds crossed only by copy... a row stores NO VM
pointer." `keys_fold_into` used to decode the fold's engine output to a VM
value before handing the row back, which every OTHER reader of a borrowed row
(`db.c`'s GET_FIELD/PROBE, `wo_row_read`, and `idx_hash`/`idx_cols_equal`/
`wo_idx_probe`) was NOT written to expect — they all decode engine→VM
themselves, on the assumption a borrow is engine-encoded like a slab row.
Invisible for SCALAR/FLOAT (decode is identity either way), and un-exercised
for TEXT/BYTES because the loader refused `resident: keys` outright until
this task lifted it — nothing had ever read a keys-resident Text field
through `db.c` at all. Fixed by making `keys_fold_into` stop decoding: the
fold's engine output lands straight in the borrowed row's slots,
`wo_row_release` frees them with `db_val_free` (not `wo_drop_kind`) exactly
like `table_destroy` frees a slab row's fields, and `row_apply_field_keys`
uses its already-engine-encoded `nv` directly instead of decoding a throwaway
VM copy. No index function needed to change, and neither did `db.c`.
Reproduced as a genuine ASan heap-buffer-overflow (a `wo_str*` read through
the `db_text*` layout) before the fix, pinned by
`test_keys_resident_update_indexed_text` (`runtime/test/test_wal.c`) after it.

**Three limitations, shipped and documented rather than fixed:**

1. *Mid-drain stale reads.* A request reading a row inside the same uncommitted
   drain as an earlier request's in-flight update to it may see the last
   durable value. Read-your-writes holds within a request, not across requests
   sharing a drain; closing it needs the fold to consult the staging buffer
   generally, not only for the same-drain unique shadow-check above.
2. *Replay is O(N²) in a row's delta-chain length* — `apply_delta` folds the
   pre-delta row, and `wo_row_remove` (called internally) folds the SAME
   offset again, so each replayed delta re-walks its whole chain.
3. *Compaction triggers on byte ratio only* — **closed by databasev2 11**:
   the fold reports hop count and `row_apply_field_keys` writes a full-row
   image (`WO_WAL_UPDATE`) past `WO_DELTA_MAX_HOPS` (16), so a hot row's
   chain is bounded in the update path itself; the checkpoint no longer
   carries that burden. `wo_wal_should_compact` also gained an absolute
   garbage term (`WO_CKPT_ABS_BYTES`).

## Schema migrations (databasev2 12)

A `@table` class is the schema; the log is the database; boot compares them.

- **The log describes itself.** `WO_WAL_SCHEMA` (kind 5) is the head record
  of every fresh and every compacted log: per class its NAME, storage flags,
  and per field name + kind + the two encoding-relevant metadata words.
  Written lazily ahead of the FIRST real record — never for a log that
  stays empty, because `durable: false` programs have a documented
  zero-bytes contract. `apply_record` skips it before reading cid/id (its
  class count would be misread as a cid); replay does not count it.
- **Head before any offset capture (defect fix 2026-09-10).** One helper,
  `stage_schema_head`, stages the pending head; `stage()` calls it on the
  first append and `wo_wal_next_offset()` calls it BEFORE answering, so the
  offset a caller records for a keys-resident row (`db.c`'s `koff`/`roff`,
  taken before the append) can never name the head. It used to: boot sets
  the schema (`main.c`, `wo_wal_set_schema`) and never forces the head, so
  the first `resident: keys` row of a fresh log was re-pointed at the schema
  record — its first read folded "record header is malformed", and through
  `wo_idx_probe` (a borrow with `msg == NULL`) that was a zero-page write:
  the residency example's `seed` died rc 139 in both `WO_DATA` forms.
  `wo_wal_next_offset` is therefore no longer pure; a head-stage OOM there is
  `wo_wal_stage_fatal`. Compaction and migration stage the head explicitly
  on a schema-less replacement log and were never exposed. Pinned by
  `test_keys_resident_fresh_log_first_row` (test_wal.c): the db.c:78
  sequence call for call, then read-by-id, `wo_idx_probe`, and replay.
- **The diff is name-keyed** (`wo_schema_diff`). Classes match by name,
  fields by name + kind, owned references (`fclass`) by the NAME the number
  resolves to — so pure declaration reordering costs only a cid remap, which
  closes the old silent hole where reordering decoded rows into the wrong
  class. Verdicts are per-class POISONS carried in the plan: retype,
  same-shape delete+add (a disguised rename), vanished class, storage-flag
  change, and the embed closure (any class whose stored values carry a
  CHANGED class's old sub-shape, to a fixpoint). A poison forces the
  transcode and bites only when a record of the class is actually met — no
  rows, no verdict.
- **The migration is a record-level transcode** (`wo_wal_migrate`), not a
  replay: no id maps, no indexes, no keys-resident logic. Old shapes decode
  through a classdesc shim built from the stored schema; embedded cids are
  renumbered by `mig_fixup_cids` (owned values carry a cid on the wire);
  surviving fields move slots, deleted values are freed, added fields take
  `enc_val(0)` — the kind's zero. Delta back-pointers rewrite through an
  offset map, and a delta on a deleted field is SPLICED: it maps to its own
  target, so later deltas step over it. Temp + fsync + rename, compaction's
  own crash discipline — a kill anywhere leaves the old log authoritative,
  including a kill after the temp is complete (`test_migrate_crash_before_rename`).
- **Legacy logs** (no head record) replay exactly as before and adopt the
  head at their next compaction. v1 verbs are add and delete only; rename
  wants `@renamed_from` (v2), data/seed migrations are v2.
