---
track: databasev2
iteration: "2"
status: done
readiness: ready
review_pending: "forks 1–7 auto-approved 2026-09-09/10 for autonomous execution — developer second review before close"
---

# databasev2 2 — per-table storage: `durable` and `resident`

> Part of [Story — databasev2: the database beyond RAM](00-story.md).
> Spec: [`2026-08-26-table-residency-design.md`](../../superpowers/specs/2026-08-26-table-residency-design.md)
> · plan: [`2026-08-26-table-residency.md`](../../superpowers/plans/2026-08-26-table-residency.md)
> · runnable example: [`docs/examples/residency`](../../examples/residency/README.md),
> gated by `just residency`.
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
| 5c | shared borrow/release accessor, then id→offset storage | ✅ `2e347de` (accessor, pure refactor, `db-bench --quick` 85/0), `125bd09` (offset storage), `08abd09` (insert + boot wiring) — hashes as on `dev`; the pre-merge `18ce4d5`/`f9c36ef` this row used to name are unreachable there |
| 5d | rewire the readers: remaining `wo_row_ptr` sites, slab scans, FK restrict, `@unique` across the boundary | ✅ `0c97fa4` (db.c), `f606fc9` (table.c, wal.c, compaction) — the pre-merge `11a92df` is unreachable on `dev`. Updates were **refused** here, then lifted 2026-08-30 — see below |
| 6a | refuse `durable: true` (the default) with no `WO_DATA`; `WO_EPHEMERAL=1` is the escape hatch | ✅ 2026-09-10 — with the **v8 table bit** (`WO_CLASSF_TABLE`), so the rule applies to `@table` classes only; forks 1–7, see Info |
| 6b | the resident byte budget | ➡ moved to [5](05-bounded-tables-eviction.md) Phase A, 2026-09-09 |
| 7 | measure, gate, document | ✅ `a310496`, 2026-08-30 |

**The `durable` half is complete and usable.** A volatile table is a full table
in-process — same indexes, same `@unique`, same FK restrict, same query surface
— and is simply empty after a restart. That is what
[porch 1–3](../porch/01-store-backed-middleware.md) were written to use for
sessions, rate-limit counters and idempotency keys — though `store.wo` in fact
declares both tables default-durable, which is why fork 6 (below) bites and
the porch gates set `WO_DATA`.

**The `resident: keys` half is fully wired for CRUD.** Storage, reads, scans,
`@unique`, deletes and updates (a WAL delta record, folded back to a value on
every read) all work, and survive both a restart and a WAL checkpoint. Task 7
measured and gated it on 2026-08-30 (`a310496`). Task 6a — the no-`WO_DATA`
refusal and its `WO_EPHEMERAL=1` escape hatch — landed 2026-09-10, and with it
the iteration closes; the byte budget (6b) moved to
[5](05-bounded-tables-eviction.md) on 2026-09-09.

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

- **Given** a `resident: keys` table, **when** rows are read by id and scanned,
  **then** every row is byte-identical including heap-valued columns. ✅ 5d.
  Every read path goes through `wo_row_borrow`/`wo_row_release`, and the scans
  go through `wo_row_next_id` — deliberately the id map for a keys table and
  the bitmap for a resident one, since hash order would reorder every
  unordered query.
- **Given** `@unique` on a `resident: keys` table, **when** a duplicate arrives
  whose conflicting row is not resident, **then** it is refused. ✅ 5d. The
  shadow probe borrows each bucket candidate, so the check costs one `pread`
  per candidate — bounded by the bucket, not the table — and never silently
  narrows to the resident subset.
- **Given** a `resident: keys` table and a WAL checkpoint, **when** the log is
  compacted, **then** every such row survives and still reads correctly. ✅ 5d,
  and this is the obligation databasev2 3 left behind. Two independent ways to
  fail it, both pinned by `test_keys_resident_survives_compaction`: compaction
  walked the *bitmap*, which a keys row has no bit in, so every one of them
  would have been dropped from the new log; and the id map would still have
  named offsets into the replaced file. Rows are rewritten in hash order, so
  offsets genuinely move and a missing re-point cannot pass by luck.
- **Given** a `delete` of a row on a `resident: keys` table, **when** it runs,
  **then** the row is gone and nothing else is touched. ✅ **fixed 2026-08-29,
  and it was memory corruption before the fix.** `wo_row_remove` read the id
  map's value as a slot, but on a keys table that value is a LOG OFFSET, and
  `slot_row` does no bounds check — so a delete indexed the slab array with a
  byte offset and then freed whatever it landed on. Pinned by
  `test_keys_resident_delete`, which SEGVs against the old code. The same
  latent trap in `wo_row_ptr` is closed too: it now returns NULL rather than a
  wild pointer when the value is an offset.

  **This is why the loader refusal earns its keep.** The gap was not one
  missing operation but a second one that corrupted memory silently, found
  only by auditing every reader of the id map.
- **Given** a logged `delete` on a `resident: keys` table, **when** the process
  restarts, **then** the tombstone replays. ✅ **fixed 2026-08-30 — and it was
  broken by the delete fix itself.** `wo_row_remove`'s keys arm borrows the row
  out of the log to find its index entries, and a borrow reads through
  `db->rt->wal`. At boot that pointer is not wired yet — `main.c` replays first
  and assigns `rt.wal` afterwards — so the borrow found no log, the remove
  failed, and replay reported a valid tombstone as CORRUPTION. Replay now lends
  the runtime a read-only view over the fd it already has open. Pinned by
  `test_keys_resident_delete_then_replay`, which fails against the unfixed code.

  Found by asking whether the read-modify-append plan was ready, not by a gate —
  it is unreachable today only because the loader refuses the annotation.
- **Given** an `update` to a row on a `resident: keys` table, **when** it runs,
  **then** it is applied. ✅ **lifted 2026-08-30.** Read-modify-**append**: a
  WAL delta record chains off the row's previous offset, and `wo_wal_fold_row_at`
  — the ONE fold every reader, replay and compaction call — walks the chain
  back to a value. Verified four ways: the fold itself, on a chain built by
  hand (databasev2 2 tasks); the request path stages the delta under group
  commit and defers the id-map re-point to the post-barrier flush, so a hot
  row costs one fsync per DRAIN, not per update; replay and compaction fold
  delta chains the same way an ordinary read does; and the oracle test
  (`test_oracle_all_vs_keys_same_update_sequence`, `test_wal.c`) drives the
  SAME sequence of updates against a `resident: all` table and a
  `resident: keys` table and asserts the rows read byte-identical at every
  step — the strongest available check that the fold agrees with ordinary
  storage, since the resident table IS the oracle. `docs/examples/residency`'s
  `Product` table is genuinely `resident: keys` now; `place_order`'s stock
  decrement survives a restart, gated end-to-end by
  `scripts/residency-accept.sh`.

  **A second gap surfaced auditing the request path before lifting the
  refusal — the same audit class that caught the `delete` memory corruption
  below.** `idx_hash`, `idx_cols_equal` and `wo_idx_probe` (`table.c`) read a
  TEXT column's slot as an engine `db_text*`, but the keys-resident fold was
  handing back VM-decoded `wo_str*` — a different struct layout. Reproduced
  as a genuine ASan heap-buffer-overflow, not merely wrong values, and present
  too in `db.c`'s `GET_FIELD` and `PROBE` arms (inline and request-path
  alike) — nobody had audited those against a keys-resident row because
  nothing could reach one while the annotation was refused. Fixed at the
  root rather than patched at each reader: a keys-resident borrow now hands
  back engine values, exactly `wo_row_ptr`'s contract for `resident: all`
  (`table.h`'s own "a row stores NO VM pointer" doctrine) — no index function
  needed to change. Pinned by `test_keys_resident_update_indexed_text`, which
  reproduces the heap-buffer-overflow against the pre-fix code.

  **Three limitations shipped, not fixed — documented, not papered over:**
  1. *Mid-drain stale reads.* A request reading a row inside the same
     uncommitted drain, while an earlier request in that drain has an
     in-flight update to it, may see the last durable value — read-your-writes
     holds within a request, not across requests in one drain. Closing it
     needs the fold to consult the WAL staging buffer generally, which is
     materially bigger.
  2. *Replay is O(N²) in a row's delta-chain length.* Each replayed delta
     re-folds the whole chain back to its base record, so boot cost for one
     long chain is quadratic.
  3. *Compaction cannot see chain length.* `wo_wal_should_compact` triggers on
     a byte ratio only, with no per-row delta-count trigger, so one hot row
     taking many small updates — a single popular SKU, this feature's own
     motivating workload — can grow a long chain without moving the aggregate
     ratio enough to fire a checkpoint. The delta-updates design's decision
     not to cap chain length rests on compaction bounding it instead; for
     this shape it does not.

     **Answered by [iteration 11](11-bounded-delta-chains.md)** (spec written
     2026-08-30): the update path already folds the row and the fold already
     walks hop by hop, so it reports the depth for free — past a fixed K the
     update writes a full row instead of a delta, and the chain resets. Read
     cost becomes at most K+1 reads and replay O(K²) per row, independent of
     when a checkpoint fires. Limitations 2 and 3 above both fall to it.

- **Given** the `resident: all` read baseline, **when** re-measured, **then**
  inside tolerance — no cost for a feature not used. ✅ Verified as a
  by-product of the residency leg: `resident: all` is unchanged at 1 354 554
  reads/sec uncapped, and every other db-bench leg still runs, which the
  keys-resident classes had briefly broken by forcing `WO_DATA` module-wide.
- **Given** a `resident: keys` table larger than RAM, **when** read randomly,
  **then** its read cost is measured against the resident baseline on its own
  read path. ✅ Measured 2026-08-30 and the answer is qualified: **1.53×**
  faster than letting the kernel swap under a cap that binds one and not the
  other — real, but nowhere near iteration 1's 273× swap figure would suggest,
  because cgroup limits charge the page cache, so moving rows to a file does
  not escape a container memory limit. The unambiguous win is footprint:
  **2.55×** smaller resident set. Full method, numbers and the failed first
  attempt below; gated by `residency.*`.

Task 6a — met 2026-09-10 (`scripts/residency-accept.sh` section 7, six checks;
`runtime/test/test_loader.c` `test_storage_flags_need_table`):

- **Given** a `@table` that is `durable: true` (the default) and no `WO_DATA`,
  **when** the program starts, **then** it refuses: exit 2 and ONE stderr line
  naming the first default-durable table and all three ways forward —
  `WO_DATA=<dir>`, `WO_EPHEMERAL=1`, or `@table(durable: false)` on that
  class. No "+N more". *(Before, this combination silently discarded every
  write.)*
- **Given** a program whose classes carry no `@table` at all, **when** it
  starts with no `WO_DATA` and no `WO_EPHEMERAL`, **then** rc 0 and nothing on
  stderr — `durable:` is a table property, told apart by the `.wob` v8 table
  bit; the loader refuses storage bits on a class without it.
- **Given** `WO_EPHEMERAL=1` and no `WO_DATA`, **when** a program with
  default-durable tables starts, **then** it runs (rc 0), prints one boot
  notice on stderr saying the sentinel is in force, and every write takes
  today's RAM path byte for byte — `db.c`'s guards are untouched.
- **Given** `WO_EPHEMERAL` set together with a non-empty `WO_DATA`, **when**
  the program starts, **then** exit 2 with one stderr line naming the
  conflict. Any `WO_EPHEMERAL` value other than `1` is the same refusal.
- **Given** `WO_EPHEMERAL=1` and a `resident: keys` class, **when** the program
  starts, **then** exit 2 with the EXISTING keys-need-a-log message — the
  sentinel does not bypass that loop.
- **Given** the harness after 6a lands, **when** the gates run, **then** the
  goldens move at most once, for the `.wob` v8 version byte and the `table`
  flag (measured: none moved — the bytecode dump prints flags by name, no
  `bc/` golden declares a table, and the header version is not printed);
  `just oop-e2e` is green with exactly one harness line changed (the corpus's
  `export WO_EPHEMERAL=1`); `db-bench --quick`'s wired RAM legs (`ram`,
  `msgrate`, `growth`, `randread` under `WO_EPHEMERAL=1`) sit inside their
  floors; every gate that sets `WO_DATA` is unchanged; gates whose programs
  declare no `@table` (fibers, subprocess, log-watcher) run untouched; and the
  two that turned out to carry durable tables after all opt in by measurement
  — chat through porch's store (`RateLimitCounter`, fork 6) and wmux's client
  legs, which run the default-durable server image with no `WO_DATA`.

The resident byte budget (task 6b until 2026-09-09) is no longer this
iteration's: it is [5](05-bounded-tables-eviction.md)'s Phase A, with the
brief's design inputs recorded there as notes for 5's own brainstorm.

### Task 7 — measured 2026-08-30, and the answer is qualified

**The question**, in the words this file has carried since the iteration was
written: `pread` through the page cache should beat the **273×** collapse
iteration 1 measured for demand-paged anonymous memory, "and the whole value of
`resident: keys` rests on how much better."

**Method.** Two tables identical except the annotation, so any difference is the
storage mode's doing: 200 000 rows, 40 000 reads in the same Weyl key order,
`WO_SHARDS=1`, WAL on **ext4** (not `/tmp`, which is tmpfs here and would have
put the "log" in RAM), memory capped with a rootless cgroup v2 scope.

**First attempt measured the wrong thing, and is worth recording.** With
Int-only rows the two modes were indistinguishable — 6 061 vs 5 599 ops/s, RSS
15.0 MB vs 14.3 MB. The cause is structural: `wo_row_drop_payload` frees each
field's *value* and returns the slot to a free list, **but never releases the
slab**, and an `Int`'s value IS its inline slot word. So dropping an Int-only
row frees nothing at all. The mode cannot help that shape, and a benchmark built
on it would have condemned the feature for the wrong reason.

**The wide shape (one `Int`, three `Text`) is where the mode can act.**

| 200k rows, 40k reads | ops/s | p50 | p99 | RSS |
| --- | --- | --- | --- | --- |
| `resident: all`, no pressure (256 MB) | 1 354 554 | 1 µs | 2 µs | 87.5 MB |
| `resident: keys`, no pressure (256 MB) | 320 053 | 3 µs | 5 µs | **34.4 MB** |
| `resident: all`, 48 MB cap | 12 854 | 67 µs | 231 µs | 48.1 MB |
| `resident: keys`, 48 MB cap | **19 635** | 65 µs | 227 µs | 34.4 MB |

The 48 MB cap is chosen to sit between the two resident sets: `resident: all`
needs 87 MB and must page, `resident: keys` needs 34 MB and fits.

**What it buys.**

- **2.55× smaller resident set** — 34.4 MB against 87.5 MB. This is the real,
  unambiguous win, and it is the thing the mode was built for.
- **A far gentler degradation curve**: under the cap `resident: all` collapses
  **105×** from its own uncapped throughput, `resident: keys` only **16×**.
- **1.53× faster than swapping at the same cap** — 19 635 vs 12 854 ops/s.

**What it costs.**

- **4.2× slower reads when memory is not tight** (320k vs 1.35M ops/s). A
  `pread` and a fold per row against a pointer dereference.
- **Writes are markedly slower**, uncosted by any design document so far: the
  keys fill of 200 000 rows did not finish inside two minutes where the resident
  fill plus 40 000 reads did. The per-insert drop-and-re-point work is the
  difference; both tables are `durable: true`, so the WAL is not.

**The finding that matters most, and it was not anticipated.** `resident: keys`
is only 1.53× faster than swapping under the cap, not the order of magnitude the
design implies — because **cgroup memory limits charge the page cache**. The WAL
here is 37 MB; the resident set is 34 MB; a 48 MB cap cannot hold both, so the
log's pages are evicted and every `pread` reaches the disk. Moving rows out of
the heap and into a file does **not** escape a container memory limit — the
cache the design leans on is charged to the same cgroup. The mode's premise,
"the kernel's page cache will hold the hot rows", fails in precisely the
containerised deployment it targets.

**Verdict.** The feature is worth keeping, but for a narrower reason than
claimed: it lets a given amount of RAM hold ~2.5× more data, and degrades far
more gracefully than swapping. It is **not** a way to make an
over-capacity table fast — under a hard memory cap it is within 1.5× of simply
letting the kernel swap. The honest guidance is "use it to fit more, not to go
faster", and the docs should say so.

**Gated 2026-08-30.** `scripts/db-bench.py` grew a `residency` leg driving
`docs/examples/residency-bench` — its own program, because declaring a
`resident: keys` table is a WHOLE-PROGRAM constraint: the runtime refuses to
start without `WO_DATA`, for every mode in the module. Putting those classes in
db-bench's shared types made `growth`, `ceiling` and `randread` — which
deliberately run without `WO_DATA` — refuse to start. That regression was caught
by running the leg, not by reading it.

What is gated, and what deliberately is not, follows `randread`'s existing
split: the absolute ops/sec under a cap is swap and disk I/O and belongs to the
box, so it is recorded and waived; the RATIOS are the engine's property.

| metric | baseline | floor | tolerance |
| --- | --- | --- | --- |
| `residency.rss_ratio` | 2.55 | 2.0 | 10% |
| `residency.overcap_vs_swap_x` | 1.53 | 1.0 | 100% |
| `residency.in_ram_cost_x` | 4.23 | 8.0 (ceiling) | 50% |
| `residency.all_collapse_x` | 105.4 | 2.0 | 100% |

`rss_ratio` carries the tight tolerance because footprint is structural — the
same class of number as `bytes_per_row`. The two throughput ratios are guarded
by their FLOORS rather than their bands, which is this harness's established
answer to a metric whose absolute value belongs to the disk. `all_collapse_x`
exists only to assert the cap actually binds; a leg whose "over-cap" half is not
over cap silently measures nothing, which is exactly what the first run of this
leg did.

Verified by feeding the gate a breaching run: `rss_ratio` 1.4,
`overcap_vs_swap_x` 0.6 and `in_ram_cost_x` 12.0 are all rejected.

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

**Task 6a — forks 1–6 locked 2026-09-09, fork 7 added 2026-09-10** (auto-approved
for autonomous execution; the frontmatter's `review_pending` asks for the
developer's second review before close):

1. **The escape hatch is the environment sentinel `WO_EPHEMERAL=1`, exact
   value.** Not `WO_DATA=:memory:` — `WO_DATA` stays a path and only a path,
   which also keeps [7](07-single-file-db.md)'s file-vs-directory parse free
   of sentinels — and no fixture edits. `@table(durable: false)` remains the
   per-table declaration; the sentinel is the whole-program one.
2. **Sentinel rules.** Honoured only when `WO_DATA` is unset or empty.
   `WO_EPHEMERAL` set together with `WO_DATA` is a startup refusal, exit 2,
   naming the conflict. Any value other than `1` is the same refusal. A
   `resident: keys` class still refuses through the existing loop — the
   sentinel does not bypass it. One stderr boot notice when the sentinel is
   in force.
3. **The refusal contract** for `durable: true` (the default) with no
   `WO_DATA`: exit 2, one stderr line naming the first default-durable class
   and all three ways forward — `WO_DATA=<dir>`, `WO_EPHEMERAL=1`,
   `@table(durable: false)`. A second loop in `runtime/src/main.c`'s existing
   startup-refusal block, beside the `resident: keys` one. No "+N more".
4. **Startup-only.** `db.c`'s guards are untouched, so the ephemeral path is
   byte for byte today's RAM path; nothing on the data path learns a flag.
5. **The byte budget (6b) leaves this iteration** for
   [5](05-bounded-tables-eviction.md), as a new Phase A — a per-table
   resident byte counter feeding its pressure signal. 5 stays
   `readiness: refine`; the design inputs go there as notes for its own
   brainstorm, and the spec's three budget obligations become 5's acceptance
   criteria.
6. **Library-owned durable tables bind every consumer.**
   [porch's store](../../examples/porch/middleware/store.wo) declares its
   tables with the default, so any program that `use`s it needs `WO_DATA` or
   `WO_EPHEMERAL=1` as a whole-program requirement. No consumer-side
   override: the library author's declaration is the declaration. Settled.
7. **The table bit in the `.wob` (v8), so durability rules apply to `@table`
   classes only** — added 2026-09-10 after the first cut refused every
   class-bearing program (fibers' `Tick`, subprocess's `ConnMsg`, log-watcher's
   `CronEntry` are plain classes, and v7 spelled `durable: true` as the mere
   absence of the volatile bit). `WO_CLASSF_TABLE` 0x08 is set from the
   emitter's `cr_is_table`; the loader refuses the two storage bits without
   it; `main.c`'s two refusal loops skip classes without it; a program with
   no durable table does not consult `WO_EPHEMERAL` at all (the
   `WO_DATA`+`WO_EPHEMERAL` conflict still refuses regardless). A v7 image is
   refused by the version check, as v6 was by v7. The alternative — teaching
   the runtime to infer "table" from the presence of indexes or an `insert`
   site — was rejected: a fact the compiler already holds belongs in the
   image, not re-derived.

## History — three corrections worth keeping

**Task 6a's first cut refused every class-bearing program (2026-09-09→10).**
The refusal keyed on "flags lack `WO_CLASSF_VOLATILE`", and the v7 image had
no bit saying "this class is a `@table`" — so `class Tick` in fibers looked
exactly like a default-durable table, and gates that never touch a table
(fibers, subprocess, log-watcher) had to export `WO_EPHEMERAL=1` to start.
Corrected the next day by recording the missing fact in the image (`.wob` v8,
`WO_CLASSF_TABLE`, fork 7), then re-measuring every gate without its export
and keeping the sentinel only where the program really refused: the corpus,
db-bench and db-actor (their own tables), chat (porch's store, fork 6) and
wmux's client legs (the server image, no `WO_DATA`). The lesson: "does this
gate run a table program" is answered by running it, not by reading the
example's own source — a `use`d library's declaration counts.

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
while keeping its index entries**. Hence the 5a–5d split. Both 5c questions
are settled by what landed (2026-08-29): the id hash stores the LOG OFFSET in
place of the slot index — one map, no parallel one — which is also why the
`delete` corruption above was possible, since a reader that trusts that value
as a slot indexes a slab with a byte offset; and the unique shadows keep their
bucket candidates and resolve them through the same borrow, one `pread` per
candidate, never a slot dereference. The write-ups are the 5c/5d acceptance
bullets above and the 2026-08-29/30 board entries.
