# databasev2 2 — table residency: the log as row store (for review)

> **Status:** design approved in brainstorm 2026-08-26, pending review of this
> document. Implements [databasev2 2](../../stories/databasev2/02-table-storage-modes.md),
> which is rewritten to match once this is approved. Amends
> [principle 7](../../00-principles.md) — already applied.
>
> Driving requirement, from the developer: **a 120 GB order table on a 32 GB
> host.** Not a tuning problem; no eviction policy fixes it. Small data-driven
> applications are well served by today's resident default and must not regress.

## Decisions taken (the brainstorm's forks, settled)

| Fork | Decision |
| --- | --- |
| One enum or two keys | **Two keys.** `durable:` and `resident:` answer two different developer questions ("do I need this after a restart?", "does it fit in RAM?"). One enum forces a name for each *combination*, which is what made a third value unreadable. |
| Mode vocabulary | **`resident: all \| keys`** and **`durable: true \| false`**. No `cold`, `tiered`, `paged`, `mmap` or `buffer` in the grammar. |
| Optional or mandatory | **Optional, both default to today's behaviour** (`durable: true`, `resident: all`). All 28 existing declarations compile unchanged; no goldens reblessed. |
| Which storage architecture | **One engine, log-structured.** The WAL already holds every row; keep an in-RAM id→offset map and read rows back with `pread`. No second engine. |
| Row cache | **None in user space.** The kernel page cache is the hot copy — the repo's own stated position in `exploration/postgresql/buffer-and-checkpoint.md`: "`pread` against an fd that already has its page cached is a memcpy… the page cache is the one cache we want", and the reason the engine avoids `O_DIRECT`. |
| `@unique` on a non-resident table | **Allowed; its index is unconditionally resident.** Settled here rather than deferred — see Constraints. |
| Budget unit | **Bytes** (estimated resident footprint). Rows is the meaningless unit: a text-heavy row and an Int-only row differ by 3.3× (measured, databasev2 1), so a row count cannot bound RAM. |
| Rejected architectures | `mmap` and a buffer pool stay out — see Alternatives rejected. `discarded.md`'s paged-engine rejection is amended to *partly revisited*, not reversed. |

## The problem, read off the engine

Facts, each verified in source rather than assumed:

- Rows live in `malloc`'d slabs of `DB_SLAB_ROWS` (256), allocated as a table
  grows and freed only at table teardown. The free-slot list recycles removed
  slots, so a delete-heavy table plateaus; a growing table only grows.
- **The ceiling is process RSS and nothing declares it.** `WO_HEAP_MB`
  (default 64 MiB) bounds the VM object arena; table storage is separate
  `malloc`. No knob says "this database may use at most N".
- No eviction, spill, paging or LRU exists anywhere in `database/src`.
- **Durability is process-global.** `main.c` opens one `shard-0.wal` when
  `WO_DATA` is set. `db.c` guards every WAL append with a null check on
  `vm->rt.wal`, so with no `WO_DATA` **every table is silently volatile** — a
  program can declare nothing and lose everything.
- An allocation failure is clean *in principle*: every `malloc` in the row
  encoder is checked and `DB_ERR_OOM` maps to `WO_T_OOM`. **Corrected 2026-08-27
  by measurement (databasev2 1): that path does not fire in practice.** With
  `vm.overcommit_memory = 0`, `malloc` succeeds and the process is SIGKILLed
  when it touches the pages — measured rc=137 at 360 000 rows under a 64 MiB
  cgroup cap. The checked-trap path belongs to the VM arena (`WO_HEAP_MB`,
  verified `trap 4 ... out of memory`), not to table storage, which has no
  ceiling at all. This makes the byte budget below the ONLY mechanism by which
  table storage can acquire one.

The measurements that bound the design, from iteration 22: durable inserts
≈4.5k/s against RAM ≈297k/s (the 66× fsync gap); reads 1.3M ops/s at p50 1µs
after the index probe landed. The read number is what a resident table buys and
what must not regress for tables that keep it.

### Why the arithmetic works

A 120 GB table at ~500 B/row is ≈240M rows. An id→offset entry is 16 bytes, so
the resident index is **≈3.8 GB** — comfortable inside 32 GB with room for two
secondary indexes of similar cost. That ratio is the whole design: **indexes
stay resident, rows do not.** It buys roughly two orders of magnitude of table
size, not infinity, and the spec says so plainly because a design sold as
unlimited gets deployed as if it were.

**What the trade costs, bounded by measurement (databasev2 1, 2026-08-27).**
Buying table size with disk reads is not free, and the price is large: random
reads across a table larger than RAM measured **273× slower** than resident ones
(1 851 166 reads/s against 6 771; p99 1 µs against 487 µs), and the transition is
a **step, not a curve** — there is no gentle region to operate in. That figure is
an *upper bound on the mechanism this spec does not use*: it is demand-paging of
anonymous memory through swap, 4 KiB per fault with no readahead, whereas
`resident: keys` `pread`s from the WAL through the page cache, which gets
readahead and a shared cache. The constant should therefore be better — **but
that is a hypothesis and task 7 must measure it, not inherit it.** Two things
follow regardless: `resident: keys` must stay opt-in per table (it is), and the
hot-set question is not deferrable decoration — it is
[iteration 5](../../stories/databasev2/05-bounded-tables-eviction.md) and it
decides whether this design is usable for anything read-heavy.

## The design

### Grammar

Two new `@table` arguments, both optional. The parser's argument loop already
matches `name` and `index` and rejects anything else with a catalogued
diagnostic; these are two more arms and an updated message. `Ast.table_cfg`
grows two fields beside `table_name` and `indexes`.

| Argument | Values | Default | Meaning |
| --- | --- | --- | --- |
| `durable` | `true`, `false` | `true` | `false` skips the WAL append entirely: no record, no fsync, ack from RAM, table empty after restart. |
| `resident` | `all`, `keys` | `all` | `keys` keeps the id map, every secondary index and every unique shadow in RAM; rows are read from the log by offset. |

`true`/`false` are already keyword tokens; `all`/`keys` are parsed as the same
bare identifiers the `index:` argument's column list already accepts. No lexer
change.

**Named `keys`, not `index`, on review (2026-08-26).** An earlier draft used
the value `index`, which put `index` in value position while `index:` is also a
key — `@table(index: [customer], resident: index)` read awkwardly (that line is
the rejected spelling, quoted). `keys` also
puts both values on one axis: `all` and `keys` each answer "what row data stays
resident", where `all`/`index` mixed a quantity with a structure name. Neither
`all` nor `keys` is a keyword or a builtin (`key_at`/`val_at` exist; bare `keys`
does not). `resident: none` was considered and rejected as overclaiming — the
indexes are very much resident.

### The four combinations

| `durable` | `resident` | Status |
| --- | --- | --- |
| `true` | `all` | Today's behaviour. The default. Reads at memory speed. |
| `false` | `all` | Volatile scratch: sessions, rate-limit counters, idempotency keys. Skips the 66× fsync cost. What porch 1–3 need. |
| `true` | `keys` | The 120 GB case. Rows in the log, indexes resident, `pread` on read. |
| `false` | `keys` | **Refused at compile time.** Rows would have nowhere to be read from. |

### Read, write and recovery paths

- **Insert** — unchanged for `resident: all`. For `resident: keys`: encode and
  append the record as today, then record id→offset in the resident map instead
  of retaining the row in a slab. The WAL append is already the durable write;
  this stops discarding its payload.
- **Read by id** — resident map lookup, then `scan_record` at the offset (which
  already `pread`s and verifies the CRC), skip the record header, and `dec_val`
  each field. Both functions already exist in `wal.c` and are already exercised
  by replay; the change is that they are called on demand rather than only at
  boot.
- **Update** — ~~append a new record, repoint the offset~~. **SUPERSEDED
  2026-08-30** by
  [`2026-08-30-keys-resident-delta-updates-design.md`](2026-08-30-keys-resident-delta-updates-design.md).
  This line was written before any of the mode was built and describes a
  full-row append, which the motivating workload rejects: a product catalogue
  changes one narrow field of a wide row on every order, so a full-row append
  rewrites every field to move one integer. Updates append a **delta** carrying
  a back-pointer, folded on read, with compaction doing the fold that keeps
  chains short. The superseded-record-becomes-garbage half is still true.
- **Delete** — append a tombstone, drop the id from the map and every index.
- **Scan** — a sequential walk of the log, which is the case log-structured
  storage is best at. Cost changes from memory-speed to sequential-disk; the
  query surface is unchanged in *meaning* and materially different in *cost*.
- **Boot** — replay rebuilds the offset map by scanning the log. Correct, and
  O(entire history), which is the honest cost of shipping this before
  [databasev2 3](../../stories/databasev2/03-wal-checkpoint.md).

### Row encoding: nothing to build — corrected 2026-08-26

**An earlier draft of this section was wrong and claimed the opposite.** It said
the on-disk record was pointer-bearing and that re-encoding it was "the one real
rewrite" and the substantive engineering of this iteration. That came from
reading `table.c`'s `db_val_encode`, which builds the **in-memory slot**, and
inferring the file format from it. The file format is a *separate* encoding in
`wal.c`, and it has been flat since iteration 9.

What is already there, verified:

- `wal.c`'s `enc_val` inlines every kind recursively with no pointer anywhere —
  text and bytes as length-then-bytes, owned as class id then fields, multi as
  element kind, length, items, map as key kind, value kind, length, pairs.
  GCREF is never stored and never logged.
- `dec_val` reads that back and allocates fresh engine-owned values.
- A record is `WO_WAL_INSERT | class_id | id | <value per field>`, wrapped in
  the `len|crc|payload|mark` frame.
- `scan_record(fd, off, …)` already `pread`s the record at an arbitrary offset
  and verifies its CRC.

So the record is already position-independent, already carries the class id and
row id, and is already randomly addressable. The in-memory slot representation
needs **no change at all**, because it was never what reached the file.

**Where the real work is instead: capturing the offset.**
`wo_wal_append_insert` calls `stage()` into a buffer (opened at 1 MiB in
`main.c`), so a record's final file offset is not known at append time — only
when that buffer flushes. Threading an accurate offset back to the caller
through a buffered writer, and keeping it correct across a partial flush and a
torn tail, is the delicate piece of this iteration. It is a much better-defined
problem than the rewrite this section used to describe, and it is bounded to
`wal.c`'s staging path plus the map that consumes it.

Consequence for the plan: this iteration is cheaper and lower-risk than first
estimated. The task that was to perform the rewrite is deleted rather than
reduced.

### Constraints across the residency boundary

- **`@unique` is allowed, and its index is unconditionally resident.** A shadow
  check cannot scan slabs that are not there, so correctness requires the unique
  column's index in RAM. Cost is one hash entry per row — the same order as the
  id map, so a unique column roughly doubles the resident index. Stated at the
  declaration so the developer can see what they bought. Silently checking only
  resident rows is the one outcome that must never ship.
- **Foreign-key restrict works unchanged.** It is a secondary-index probe, and
  secondary indexes are resident.
- **`ref` navigation works unchanged**, at the cost of a `pread` per hop.
- **A `resident: all` table may hold a `ref` into a `resident: keys` table**
  and vice versa — both are durable, so neither evaporates. This is the case
  that a `durable: false` table genuinely breaks, below.

### What the compiler enforces

Four refusals, each a catalogued `WO-E1xx` diagnostic added in the same change
as the code — not afterwards:

1. `durable: false` with `resident: keys` — the meaningless combination.
2. An unknown value for either argument, or either argument given twice.
3. **A `durable: true` table holding a `ref` into a `durable: false` table.**
   A persistent row cannot reference one that evaporates on restart; FK restrict
   cannot save it and the dangling reference is provable from the class table.
   This is the highest-value check in the iteration.
4. A `cold`, `tiered`, `ram` or other retired mode word — refused with a message
   naming the two real arguments, so the vocabulary explored during the
   brainstorm does not become folklore.

### What the runtime enforces

- **`durable: true` with no `WO_DATA` is a startup refusal.** Today this
  combination silently loses everything, which is the worst failure mode in the
  current engine. A program that declares durability and is given nowhere to put
  it must not start. This is independent of residency and is arguably the most
  valuable single line in the spec.
- **A byte budget that exists by default, breached loudly.** The budget bounds
  estimated resident footprint across all tables. On breach the program refuses
  with a message naming the largest offending table and the exact annotation to
  add, so the 120 GB developer meets a diagnostic at 32 GB rather than the OOM
  killer.

  **The default must not be "no budget"** — that was a contradiction in the
  first draft of this spec: a budget nobody sets cannot produce the diagnostic
  that is this design's main deliverable, and the ERP developer would still meet
  the OOM killer. So the default is a **fraction of host-detected available
  memory**, overridable by an environment variable and by a per-program
  declaration. Choosing that fraction is the one number this spec cannot supply:
  it comes from [databasev2 1](../../stories/databasev2/01-ram-ceiling-measurement.md)'s
  swap-onset measurement, which is why 1 sequences before 2. Until 1 lands,
  implement the mechanism with a conservative placeholder fraction and treat the
  value as unset rather than settled.

  Consequence to accept deliberately: a program that today grows past that
  fraction and survives on a large host will now refuse. That is the intended
  behaviour change — it converts an invisible slide into swap into a startup
  error — but it *is* a behaviour change, and it is the second of the two
  breaks listed under Migration.
- `WO_DATA` remains the data-directory location. It stops being the durability
  switch; the declaration is.

### Format

The class descriptor carries both properties, so the runtime never re-derives
them. This moves `WOB_VERSION` (currently 6) and the format contract in the same
commit as the code. An older image is refused on version rather than misread.

## Why this shape

- **It keeps one engine and one source of truth.** The log *is* the database.
  Nothing here adds a second storage system, which is what the 2026-08-18
  rejection was actually about.
- **It costs nothing for tables that do not use it.** A `resident: all` table
  takes the same path it takes today. The 1.3M ops/s read baseline is the
  regression gate, and a measurable regression there is grounds to reject the
  implementation rather than tune it.
- **The failure mode becomes a diagnostic.** Both exits **as measured** in
  databasev2 1 (2026-08-27) are replaced by a refusal that names the fix — and
  the measurement made this argument stronger than the draft that named "swap
  thrash and the OOM killer". What actually happens is **SIGKILL signal 9** with
  swap off (no trap: overcommit lets `malloc` succeed, the kernel kills on page
  touch, so the checked path never runs) or **exit 0 while serving from disk**
  with swap on. Neither is a diagnostic; one is silence and the other is a
  corpse. A declared budget is the only way this engine can say anything at all
  before either.
- **It is declared, not automatic.** No threshold heuristic, no performance
  cliff the compiler cannot explain. Consistent with a language whose thesis is
  that the compiler tells you the truth.

## Alternatives rejected

| Alternative | Why not |
| --- | --- |
| **`mmap` the row file** | Gives up precise ack-after-fsync for the kernel's flush schedule, which is the one guarantee this design will not trade. The repo's own mmap study only ever proposed it read-only for segment lookups. Since records are *already* flat and position-independent, mmap remains available later as a pure read-path optimisation over the same file — recorded, not adopted. |
| **Buffer pool with dirty-page tracking** | This is the Rust-era phase-12 design in `exploration/postgresql/buffer-and-checkpoint.md` (`CachedRow { bytes, dirty }`, `WO_CACHE_ROWS` LRU) that died with that track. It duplicates the kernel page cache, and the page cache is explicitly the cache this project wants. |
| **Paged B-tree engine** | Rejected 2026-08-18 and still rejected. Reading rows from the log we already write is not this. |
| **A three-valued `mode:` enum** | Needs a name per combination. The brainstorm demonstrated that the third name is unwriteable before its mechanism is decided. |
| **Automatic spill at a threshold** | No annotation needed, but unpredictable cliffs and magic the compiler cannot explain. |
| **Disk-backed by default** | Costs every application the 1µs read, including the small ones explicitly said to be well served today. |

## Proof plan

Acceptance is the story's Given/When/Then list; this is how each is exercised.

- **Compatibility** — all 28 existing `@table` declarations compile untouched;
  `just employee`, `just db-actor`, `just web-app`, `just site`, `oop-accept`
  unchanged; no golden reblessed.
- **Volatility** — a `durable: false` table produces no WAL growth (measured,
  not asserted) and is empty after restart while durable siblings replay intact.
- **Residency correctness** — a `resident: keys` table larger than the
  configured budget returns every row correctly by id, byte-identical including
  every heap-valued column, and scans in full.
- **Constraints across the boundary** — `@unique` refuses a duplicate whose
  conflicting row is not resident; FK restrict refuses a delete whose only
  referrer is not resident. Corpus fixtures, both.
- **Compiler refusals** — a `compile-fail` fixture per diagnostic, each pinning
  the exact code.
- **Runtime refusals** — `durable: true` with no `WO_DATA` fails at startup;
  a budget breach names the table and the annotation.
- **Crash safety** — `kill -9` mid-append and mid-checkpoint on a
  `resident: keys` table; replay loses no acked write and no row appears twice.
- **Performance** — new baseline rows for the `resident: keys` read path with
  its amplification versus resident, published in `perf-targets.md` as a number
  a developer can plan around; and a regression check that resident tables did
  not move.
- **Sanitisers** — ASan on the new decode path, which is where the bugs are.

## Out of scope

- **Checkpoint and compaction** — [databasev2 3](../../stories/databasev2/03-wal-checkpoint.md).
  This spec's boot cost is O(history) until 3 lands, and 3's snapshot should
  persist the offset map so boot stops rescanning. That coupling is stated in
  both documents.
- **Eviction policy and a resident row cache** — [databasev2 5](../../stories/databasev2/05-bounded-tables-eviction.md).
  This design needs neither: rows are either all resident or none are.
- **io_uring on the read path** — a real question that only exists after this
  lands; noted in [databasev2 4](../../stories/databasev2/04-io-uring-commit.md),
  not folded into it.
- **Per-shard residency** — the owner shard owns the store and the log. A
  `durable: false` table arguably need not live on the owner at all, which would
  be dramatically faster and a different consistency story. Recorded as a
  candidate, deliberately not decided here.
- **Migrating an existing dataset between settings**, and a WAL written when a
  table had different settings: refuse clearly on mismatch, do not convert.
- **`transaction { }` and `@table` feature flags** — language iteration 18,
  approved spec, left whole.

## Migration

Nothing to convert. Both arguments default to present behaviour, so every
existing program, manifest, corpus fixture and golden compiles and runs
unchanged.

**Two deliberate behaviour changes**, both converting a silent failure into a
loud one, and both listed here so neither arrives as a surprise:

1. `durable: true` (the default) with no `WO_DATA` becomes a startup refusal.
   Today it silently discards every write.
2. Total estimated resident footprint crossing the default budget fraction
   becomes a startup or insert refusal. Today the program slides into swap with
   no signal and is eventually killed.

Both are opt-out-able by explicit declaration. Neither is a data-format change,
so a rollback is a binary swap with no migration.
