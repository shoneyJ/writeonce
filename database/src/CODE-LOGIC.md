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

## Verifying a change

- `make -C runtime test` — `test_table` is this directory's suite (round
  trips across kinds, nil encodings, shard interleave, slab growth, slot
  reuse, misuse), ASan+UBSan like every runtime test.
- `just oop-e2e`, `just log-watcher` — regression that linking the engine
  into wovm changed nothing observable (it is dead code until Task 3 wires
  the first builtin).
