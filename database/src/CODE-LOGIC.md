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
