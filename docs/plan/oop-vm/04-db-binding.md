# DB binding — row format, id discipline, WAL layout, query subset

> Normative companion to the engine plan
> ([`2026-08-01-db-engine-binding.md`](../../superpowers/plans/2026-08-01-db-engine-binding.md)),
> the way `00-wob-format.md` is normative for the image. Grows with the
> plan's tasks; this revision covers **Task 1 (row storage)**. Memory-safety
> doctrine lives in the 9b design's section 6 (the copy bulkhead) — this doc
> is the *format*.

## Two memory worlds, one crossing rule

Rows store **no VM pointer**, ever. Values cross from VM heap to row storage
by copy on insert, and back by copy on read (`wo_row_read` allocates fresh VM
values from the shard's runtime). The engine's own allocations are plain
malloc — never the VM arena, so table growth cannot eat the program's heap
cap, and a heap-exhausted program can still read its data.

## Row format

```
row      := header slots
header   := id u64 | class_id u32 | flags u32          (16 bytes)
slots    := field_cnt × u64, declaration order          (the VM object shape)
```

One 8-byte slot per field, kind-driven — the same kind bytes the `.wob`
class table carries, walked the same way the VM walks them:

| kind | slot holds | engine-owned shape |
| --- | --- | --- |
| `SCALAR` | the 8 bytes themselves | — (`WO_NIL_SCALAR` spells a `?scalar` nil) |
| `TEXT` | pointer, 0 = nil | `db_text { len u32; bytes[] }` |
| `OWNED` | pointer, 0 = nil | `db_rec { class_id u32; slots[] }` — flattened by value, recursively through these same rules |
| `MULTI` | pointer, 0 = nil | `db_multi { elem_kind u8; len u32; items[] }`, elements encoded element-wise |
| `MAP` | pointer, 0 = nil | `db_map { key_kind, val_kind u8; len u32; kv pairs }` |
| `GCREF` | **never stored** | compile error upstream (the GC bulkhead); the engine refuses it defensively as an encode error |

`ref T` is a `SCALAR` at this layer — the target row's id. The engine learns
what it references only when the FK checks land (9b plan, Task 3).

## Storage

Per shard, per class, created lazily on first insert:

- **Slabs** of 256 rows (`DB_SLAB_ROWS`), malloc'd, **never moved or freed
  while the table lives** — a row's address is stable for its lifetime,
  which is the property 9b's loop-scoped row views stand on.
- An **occupancy bitmap** (one bit per slot, slab-major) and a LIFO
  **free-slot list**: removal recycles the slot; a recycled slot is always
  used before a new slab grows. Ids are never reused; slots are.
- The **primary index**: an open-addressing hash, id → slot, splitmix64
  finalizer, power-of-two capacity, 0.7 load, tombstoned deletes (ids are
  never 0 and never reused, so the all-ones sentinel cannot collide).

## Id discipline

Per table, per shard: shard S of N allocates `S+1, S+1+N, S+1+2N, …` — the
c-runtime plan's shipped interleave. Creation is coordination-free; a row's
owner shard is `(id-1) % N`. Milestone 1 runs at N=1 and everything
degenerates to `1, 2, 3, …`. Id 0 does not exist (it is the hash's "empty"
and the `?ref`'s nil).

## Choke points

`wo_row_insert` and `wo_row_remove` are the only functions that mutate a
table. Task 4's secondary indexes hook exactly these two sites (marked
`INDEX HOOK` in `database/src/table.c`); the WAL (Task 2) stages its record
beside the same calls. Anything else touching a slab is a defect by
definition — the doctrine the Rust engine learned and this engine enforces.

## Still to come in this document

- **Task 2**: WAL record framing (`length | crc | payload | commit-mark`),
  payload encoding for typed rows, group-commit ordering, replay rules,
  torn-tail handling, the `wal-check` oracle.
- **Task 3**: the `insert` statement's builtin ids (appended to
  `00-wob-format.md`'s builtin table) and execution contract.
- **Task 4**: secondary-index format, `@unique` trap code.
- **Task 5**: the select subset and its builtins.
