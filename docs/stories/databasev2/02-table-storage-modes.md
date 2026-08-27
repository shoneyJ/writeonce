---
track: databasev2
iteration: "2"
status: in-progress
readiness: ready
---

# databasev2 2 — per-table storage: `durable` and `resident`

> Part of [Story — databasev2: the database beyond RAM](00-story.md).
> Spec: [`2026-08-26-table-residency-design.md`](../../superpowers/specs/2026-08-26-table-residency-design.md)
> · plan: [`2026-08-26-table-residency.md`](../../superpowers/plans/2026-08-26-table-residency.md).
>
> **The language enrichment this track exists for.** Before this, durability was
> one environment variable for a whole process: `WO_DATA` set and every `@table`
> is WAL-logged, or unset and none are (`runtime/src/main.c`, and `db.c` guards
> each append on the WAL pointer). Real applications are not uniform — a session
> table and a rate-limit counter are disposable, an orders table is precious, a
> 120 GB audit table does not fit in RAM at all. One global switch forces
> "everything is precious" or "nothing is", and the developer pays for the wrong
> one either way.
>
> **Rewritten 2026-08-27** to match what was designed and built. Two earlier
> drafts of this file described a three-valued `mode:` enum including `cold`;
> that design was replaced during the brainstorm and the history is at the
> bottom.

## The design, as built

Two optional `@table` arguments, because the developer is answering two
independent questions — *do I need this after a restart?* and *does it fit in
RAM?* A single enum would have forced a name for every combination, which is
what made the third value unwriteable before its mechanism existed.

| Argument | Values | Default | Meaning |
| --- | --- | --- | --- |
| `durable` | `true`, `false` | `true` | `false` skips the WAL append entirely: no record, no fsync, ack from RAM, table empty after restart |
| `resident` | `all`, `keys` | `all` | `keys` keeps the id map, secondary indexes and unique shadows resident; rows are read back from the log by offset |

Both default to the pre-existing behaviour, which is why all 28 `@table`
declarations in the repository compiled unchanged and no golden moved.
`durable: false` with `resident: keys` is refused — rows would be neither
logged nor resident, so there would be nowhere to read them from.

The engine stays **one log-structured store**. The WAL already held every row;
this iteration stops discarding the payload. No second engine, no user-space row
cache — the kernel page cache is the hot copy, which is the position
`exploration/postgresql/buffer-and-checkpoint.md` already argued and the reason
the engine avoids `O_DIRECT`.

Principle 7 was amended for this: the log is authoritative, residency is a
declared per-table policy. Durability is untouched and unconditional.

## Progress

| # | Task | State |
| --- | --- | --- |
| 1 | grammar: both arguments, defaults preserve behaviour | ✅ `69b7ce2` |
| 2 | WO-E224: refuse a durable `ref` into a volatile table | ✅ `753e6c4` |
| 3 | `.wob` v7: the class descriptor carries both properties | ✅ `7e68c99` |
| 4 | `durable: false` skips the WAL append and replay | ✅ `dd67e31` |
| 5a | `wo_wal_next_offset` — exact record offsets | ✅ `ac7d8af` |
| 5b | `wo_wal_read_row_at` — a row from a log offset | ✅ `d0c370c` |
| 5c | shared borrow/release accessor, then id→offset storage | 🔄 step 1 ✅ `2e347de` (pure refactor, `db-bench --quick` 85/0); offset storage next |
| 5d | rewire the readers: remaining `wo_row_ptr` sites (6 table.c, 2 db.c, 2 wal.c), slab scans, FK restrict, `@unique` across the boundary | ⬜ scope recorded |
| 6 | the two runtime refusals (no-`WO_DATA`, the byte budget) | ⬜ |
| 7 | measure, gate, document, close out | ⬜ |

**The `durable` half is complete and usable.** A volatile table is a full table
in-process — same indexes, same `@unique`, same FK restrict, same query surface
— and is simply empty after a restart. That is what
[porch 1–3](../porch/01-store-backed-middleware.md) need for sessions,
rate-limit counters and idempotency keys.

**The `resident: keys` half has its read path but no storage behind it.**
Offsets can be captured and rows can be read back from them; nothing yet stores
a table that way.

## Acceptance Criteria

Met:

- **Given** every existing `@table` declaration, **when** compiled, **then**
  behaviour is byte-identical. ✅ verified as `git diff` over
  `compiler/test/golden/` being empty after a `WOC_BLESS` run — a green test
  run alone proves nothing, since blessing rewrites every golden.
- **Given** `durable: false` with `WO_DATA` set, **when** rows are inserted,
  **then** the WAL does not grow and the table is empty after a restart while
  durable siblings replay. ✅ measured: 50 inserts wrote 1500 bytes durable and
  **0** volatile. Measured against the file's non-zero prefix, because the file
  is `fallocate`'d to 1 MiB and its size proves nothing.
- **Given** an unknown value, a repeated argument, a retired design word, or the
  refused combination, **when** compiled, **then** WO-E102 with a message
  naming what to write instead. ✅
- **Given** a `durable` table holding a `ref` into a volatile one, **when**
  compiled, **then** WO-E224 naming both classes and both escapes. ✅ The
  reverse direction and every `backlink` shape stay legal, pinned by a run
  fixture so the check cannot grow over-broad.
- **Given** a WAL holding records for a class the source now declares volatile,
  **when** the program starts, **then** it refuses, exits 2, names the class,
  and is **not** reported as corruption. ✅
- **Given** a v6 image, **when** loaded, **then** refused on version rather
  than misread. ✅

Outstanding:

- **Given** a `resident: keys` table larger than any plausible resident budget,
  **when** rows are read by id and scanned, **then** every row is byte-identical
  including heap-valued columns. *(needs 5c/5d)*
- **Given** `@unique` on a `resident: keys` table, **when** a duplicate arrives
  whose conflicting row is not resident, **then** it is refused. *(5d — the
  correctness core; a constraint that silently checks only resident rows must
  never ship)*
- **Given** `durable: true` and no `WO_DATA`, **when** the program starts,
  **then** it refuses. *(task 6 — today this combination silently discards
  every write)*
- **Given** the resident footprint crossing the budget, **when** it does,
  **then** a refusal naming the table and the annotation. *(task 6)*
- **Given** the `resident: all` read baseline, **when** re-measured, **then**
  inside tolerance — no cost for a feature not used. *(task 7)*

## Out Of Scope

- **Checkpoint and compaction** — [3](03-wal-checkpoint.md). Boot rebuilds the
  offset map by scanning the log until that lands, which is O(all history);
  3's snapshot should persist the map.
- **Eviction and a resident row cache** — [5](05-bounded-tables-eviction.md).
  This iteration's tables are either fully resident or keys-only.
- **io_uring on the read path** — a real question that only exists after this;
  noted in [4](04-io-uring-commit.md), deliberately not folded in.
- **`transaction { }` and `@table` feature flags** — language
  [iteration 18](../language-runtime-database/18-memory-db-features.md),
  approved spec, left whole.
- **Per-shard residency for volatile tables** — a volatile table has no WAL, so
  it arguably need not live on the owner shard at all. Faster, and a different
  consistency story. Recorded as a candidate, not decided.
- **Converting an existing dataset between settings.** Refuse on mismatch, do
  not convert — implemented in task 4.

## Info — the forks, settled

1. **Two keys, not one enum.** An enum needs a name per *combination*, and the
   brainstorm demonstrated the third name is unwriteable before its mechanism is
   decided.
2. **`keys`, not `index`.** `index:` is already an argument key, so
   `@table(index: [c], resident: index)` read badly. `all`/`keys` also put both
   values on one axis — what row data stays resident. `resident: none` was
   rejected as overclaiming, since the indexes are very much resident.
3. **Optional with `durable` defaulting true**, not mandatory. Mandatory would
   have touched 28 declarations, 13 corpus fixtures and 3 goldens; the README
   already says nothing is API-stable, so making it mandatory at 1.0 stays
   available.
4. **`@unique` on `resident: keys` is allowed**, with its index unconditionally
   resident. Roughly doubles the resident index; stated at the declaration so
   the cost is visible.
5. **The budget is bytes, not rows** — a text-heavy row and an Int-only row
   differ by 3.3× (measured, databasev2 1), so a row count cannot bound RAM.

## History — two corrections worth keeping

**The three-mode design was replaced.** Earlier drafts had
`mode: ram | durable | cold`. `cold` conflated two independent properties and
could not be named honestly before its mechanism existed, and the developer's
120 GB-on-32 GB case showed the real axis was residency. Replaced by two keys,
and principle 7 amended rather than worked around.

**The "one real rewrite" was fiction.** The spec claimed the on-disk record was
pointer-bearing and that re-encoding it was this iteration's substantive
engineering. That came from reading `table.c`'s `db_val_encode` — which builds
the *in-memory slot* — and inferring the file format from it. `wal.c`'s
`enc_val` has been flat since iteration 9. The task was deleted, not reduced.

**The opposite half then turned out to be genuinely deep.** With the format
fine, the plan's storage steps still read as plumbing. Measured instead:
`wo_row_ptr` returns a `db_row *` into a slab and has 11 call sites, `table.c`
has 37 slab references, `db.c:105-181` walks slabs for scans, `enc_val`
serialises *from* the slab, and **no operation exists that drops a row's payload
while keeping its index entries**. Hence the 5a–5d split. 5c and 5d need their
own write-ups, and the two open design questions for 5c are whether the id hash
stores offsets in place of slot indices or gains a parallel map, and what the
new operation does about the unique shadows, which currently point at slots.
