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

## WAL (Task 2) — `database/src/wal.{c,h}`

Record framing, replay-whole-or-not-at-all:

```
record  := len u32 | crc u32 | payload | mark u32
len      = payload bytes (never 0: a zero length is the preallocated tail)
crc      = CRC32 (poly 0xEDB88320) of the payload
mark     = 0x574F4C31 "WOL1", the last bytes of the record — a record
           without its mark is torn by definition
payload := kind u8 | class_id u32 | row_id u64 | body
kind     = 1 insert (body = fields), 2 remove (no body), 3 update (Task 5)
```

Body fields walk the class table's kinds: `SCALAR` 8 bytes; `TEXT` u32 len +
bytes (`0xFFFFFFFF` = nil); `OWNED` presence u8 then class id + fields
recursively; `MULTI` presence + elem kind + len + elements; `MAP` presence +
both kinds + len + pairs. Little-endian, same platform note as the loader.

**Commit order (doctrine, verbatim from the shipped phase-D pattern):** RAM
apply → stage record → `wo_wal_commit` (one pwrite of the batch + one
fdatasync) → only then acknowledge. Group commit = everything staged since
the last commit rides one sync.

**Replay** decodes payloads straight into engine-owned values — no VM heap
involved, boot cannot depend on a VM existing — and rows re-enter through
the choke-point row API, so Task 4's indexes rebuild for free. A torn tail
(short record, bad CRC, missing mark, zero length) ends the intact prefix:
everything from the tear on is dropped whole, and `wo_wal_open` positions
its write offset AT the tear so the next commit overwrites it. A record that
CRC-passes but does not decode is corruption, not a tear — replay fails
loudly. A missing file is a fresh boot, not an error. After replay each
table's `next_id` sits past every replayed id this shard owns.

**Oracle:** `wo_wal_check(path)` walks a file with no engine and reports the
intact record count and prefix end — the crash battery's verifier
(`runtime/test/test_wal.c`: five rounds of insert/commit/ack-over-pipe with
SIGKILL mid-stream; every acked row present and exact after replay).

## Still to come in this document

- **Task 3**: the `insert` statement's builtin ids (appended to
  `00-wob-format.md`'s builtin table) and execution contract.
- **Task 4**: secondary-index format, `@unique` trap code.
- **Task 5**: the select subset, update record semantics, and its builtins.
