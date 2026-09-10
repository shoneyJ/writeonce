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

**Where the log lives (databasev2 7, 2026-09-10).** `WO_DATA` is always a
path, never a sentinel (ephemerality is `WO_EPHEMERAL=1`, databasev2 2 task
6a), and it names the store in one of two forms, resolved by
`wo_wal_resolve_data_path` (`wal.{c,h}`) before anything is opened:

- **Directory form** — an existing directory, or any path ending in `/`: the
  log is `<dir>/shard-0.wal`, byte for byte what every deployment and gate
  before this iteration used (a trailing slash still yields the `//` the
  pre-7 driver produced, and a trailing slash on a missing directory still
  fails at open: `wovm: cannot open <dir>//shard-0.wal`, exit 2).
- **File form** — anything else: the path IS the log (`app.db`, `store.wo.db`
  — the name is the operator's). An existing regular file is opened; an
  absent path is created by `wo_wal_open`, but only when its parent directory
  already exists. Two refusals, each exit 2 and ONE stderr line: a parent
  that is not an existing directory — `wovm: WO_DATA=<path> — its parent
  <parent> is not an existing directory; create it first (wovm never runs
  mkdir -p).` — because a typo must not plant a store somewhere unexpected;
  and a path that exists but is neither a regular file nor a directory
  (fifo, socket, device). A result longer than the driver's path buffer is
  refused as well, never truncated.

One file is the whole store at any core count (shard 0 is the only WAL
writer since arc stage 3). Compaction (databasev2 3) and schema migration
(databasev2 12) rewrite through `<log path>.compact` beside the log and
fsync the log's parent after the `rename` — both derive that from the log
path, never from `WO_DATA`, so the file form inherits their crash safety
unchanged; the boot-time parent check and the post-rename fsync share one
derivation (`parent_dir_of`). `WO_EPHEMERAL` set together with either form
refuses exactly as 6a says. Pinned by `runtime/test/test_wal.c`
`test_resolve_data_path` and `test_file_form_temps_beside_log`.

## Insert (Task 3) — builtin 61, `database/src/db.c`

`insert Class { field: expr, … }` is a typed expression (statement position
included): fields validate like a constructor literal (defaults and `?`
fields omittable — an omitted `?scalar` gets `WO_NIL_SCALAR`, other omitted
optionals the zero word, declared defaults their value), and the result is
the new row's id. Lowering emits builtin **61**: R[B] = class-id constant,
R[B+1..] = one slot per declared field in declaration order (the literal's
order is irrelevant — slots are the class table's).

Execution: `wo_row_insert` (RAM, engine copies every value), then — when
durability is on — stage, then a barrier before the acknowledgment. **Updated
2026-08-28 (databasev2 4 part A): group commit landed, and the barrier's
location now depends on which path the statement takes.**

A statement arriving from a worker shard marshals to shard 0 and parks; shard 0
stages every such request, issues **one** barrier when its queue empties, and
only then releases the held replies — so each writer is acknowledged after the
barrier that carried *its* record. A statement already running on shard 0 takes
the inline path and still commits before the builtin returns, because it has no
reply to hold: it returns into its own fiber, and batching it would require
parking that fiber on the barrier (deferred to part B). The boundary is the
queue draining, **not** the tick this document previously anticipated — a tick
would add latency to a lone writer, taxing an idle system to serve a busy one.

Measured: ~2.9× durable write throughput and ~2.1× lower p50 on a
write-concurrent workload; unchanged for a serial writer, which has nothing to
batch with.

**Compaction (databasev2 3, 2026-08-29) may run only where NOTHING IS STAGED.**
That is a correctness requirement, not a scheduling preference: the staging
buffer holds records destined for a file that compaction is about to replace, so
compacting with a non-empty buffer would either write them into a file about to
be discarded or lose them with it. In practice the safe points are immediately
after a barrier — the drain's, and the inline path's — and both are wired.
`wo_wal_compact` refuses a non-empty buffer as a backstop rather than trusting
its callers.

**Recovery is unchanged by compaction.** The result is an ordinary log in the
ordinary record grammar, replayed from byte 0; there is no snapshot, no second
source, no cutoff offset and no control file. Crash safety comes from `rename`
being atomic: before it the live log is intact and the temp file is not
authoritative, after it the new log is complete, and no reader can observe a
mixture. A crash mid-rewrite leaves a temp file, which the next open removes.

A failed compaction is a **missed optimisation, not a durability event** — the
original log is left usable and the process continues. It must not take the
fatal path below.

A failed commit **no longer traps — it ends the process** (exit 74, with a
diagnostic naming the operation, log path, `errno` and batch size). So does a
failed staging. `WO_T_IO` is unreachable from a DB write. One rule: once a
statement has mutated RAM, the outcomes are durable or death. Engine failures
still trap `WO_T_DB`. Durability is opt-in: `WO_DATA=<dir>` makes the CLI replay
`<dir>/shard-0.wal` before the entry runs and commit every insert; without
it the engine is RAM-only (every corpus fixture runs that way).

Ownership: the engine copies at the row API, so an insert **borrows** its
field values — no transfer, no E304; freshly built values are dropped at the
site (emit.ml mirrors the push/set reap). The insert node is trap-capable
(unique violations arrive with Task 4) and carries a live-mask drop entry.

## Still to come in this document

- **Task 4**: secondary-index format, `@unique` trap code.
- **Task 5**: the select subset, update record semantics, and its builtins.
