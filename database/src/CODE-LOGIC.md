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
