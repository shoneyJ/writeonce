# Story — databasev2: the database beyond RAM

The third track. `language-runtime-database/` built the engine;
[`porch/`](../porch/00-story.md) is the framework on top; this track answers the
question v1 deliberately deferred: **what happens when the data does not fit in
memory.**

Numbering restarts at 1, local to this track. Frontmatter carries
`track: databasev2`, and iterations moved here keep their old id in
`was_language_iteration:` so a search for "iteration 32" still finds the WAL
checkpoint. Status stays where it belongs — the `status:` key, never a directory.

## The problem, stated honestly

Principle 7 says **RAM is authoritative; the WAL makes it durable.** That is a
real design, not a shortcut: reads never touch disk, so latency is predictable,
and durability is a sequential append rather than a storage engine bolted to the
side. Iteration 22 measured what it buys — reads at 1.3M ops/s after the index
probe landed, p50 1µs.

The bill comes due at the ceiling. Read from the engine as it stands:

- **Rows live in `malloc`'d slabs of 256, and their addresses are stable
  forever** (`database/src/table.c`, `DB_SLAB_ROWS`). Slabs are allocated as a
  table grows and freed only when the table is destroyed. The free-slot list
  recycles removed slots, so a delete-heavy table plateaus — but a growing table
  only grows.
- **The ceiling is process RSS, not a configured number.** `WO_HEAP_MB` (default
  64 MiB) bounds the VM object arena; table storage is separate `malloc`, so
  nothing in the system declares a maximum dataset size. There is no knob that
  says "this database may use at most N".
- **There is no eviction, no spill, no paging, no LRU.** Grep
  `database/src/` for any of them and nothing comes back. Every row ever
  inserted and not deleted is resident.
- **The WAL is append-only with no checkpoint.** Boot replays every record ever
  written, so startup time is O(all writes in the file's history) and disk grows
  without bound. That is databasev2 [3](03-wal-checkpoint.md).
- **Durability is process-global.** `WO_DATA` is one environment variable that
  turns on one `shard-0.wal` for the whole process (`runtime/src/main.c`). There
  is no way to say "this table matters, that one is scratch".

### What actually breaks first

Worth being precise, because the failure mode determines the fix — and the good
news is that the engine's own behaviour is clean:

**Corrected 2026-08-27 by measurement.** This section used to open "an
allocation failure is a catchable trap, not a crash". That is true only of the VM
object arena, whose `WO_HEAP_MB` ceiling is checked and does trap
(`trap 4 … out of memory`, exit 1). **Table storage has no ceiling at all** —
details below, measured. See [iteration 1](01-ram-ceiling-measurement.md).

Every `malloc` in
the row encoder is checked and jumps to an `oom` label; `DB_ERR_OOM` maps to
`WO_T_OOM`, which a program can `try`/`catch`. On paper a writeonce program that
runs out of memory *refuses the insert* rather than corrupting or dying.

**Measured 2026-08-27: that code does not run.** Under
`vm.overcommit_memory = 0` — the Linux default — `malloc` **succeeds** and the
kernel kills the process when it later *touches* the pages. So the checked path
never gets a NULL to check. It is not dead code in principle, just unreachable in
the configuration everything actually runs in. What a deployment gets instead,
both exits measured under a cgroup cap:

- **swap off: `SIGKILL`, signal 9.** No trap, no message. An external `SIGKILL`
  is the one shutdown path that skips every guarantee the WAL was written to
  provide — though ack-after-fsync holds: ~40 000 rows came back as a contiguous
  intact prefix, no holes, not read as corruption. Iteration 22's `kill -9`
  battery proved this for an external kill; iteration 1 proved it for the OOM
  killer.
- **swap on: exit 0.** The process finishes, returns success, and serves from
  disk. The price depends entirely on access pattern: appending pays **~1%**
  (148 s vs 150 s uncapped for 900 000 rows) because cold pages are written once
  and never re-read, while random reads across the table pay **273×** (1 851 166
  vs 6 771 reads/s; p99 1 µs vs 487 µs). "A RAM-authoritative database on swap is
  the worst of both worlds" is therefore true of the **read** path specifically,
  not of writes.

So the honest problem statement is not "malloc fails". It is: **there is no
declared budget, no back-pressure as the budget is approached, and no way to
distinguish data that must be resident from data that merely is.**

**Iteration [1](01-ram-ceiling-measurement.md) has now measured this
(2026-08-27), and it strengthened the statement rather than softening it.** A row
costs **96.5–100 B** Int-only and **320.6–324 B** text-heavy (3.3× apart, so no
single per-row number can bound RAM). At the ceiling the engine has exactly two
behaviours and **neither one tells anybody**: without swap the process is
**SIGKILLed on signal 9** — table storage has no checked ceiling, and under
`vm.overcommit_memory = 0` its `malloc` succeeds and the kernel kills on page
touch — and with swap it **keeps returning 0 while serving from disk**, finishing
900 000 rows in 148 s against 150 s uncapped. Durability is the one thing that
does hold: acked writes came back as an intact prefix across an OOM kill.

And when swap does absorb it, the price depends entirely on access pattern:
inserting pays **~1%**, while reading randomly across the table pays **273×**
(1 851 166 reads/s resident against 6 771 over-cap, p99 1 µs against 487 µs).
That second number is the one this track must respect, because it is the access
pattern [2](02-table-storage-modes.md)'s `resident: keys` creates by design.

That is why "back-pressure at exhaustion" is not a design option. Exhaustion
either kills without warning or never arrives — and the latency signal offers no
early warning either, since departure is a **step** (1 µs to 487 µs, nothing in
between) rather than a curve. Only a **declared threshold** can speak in time.

## The lever: per-table storage modes

The developer's ask, and the reason this track has a grammar iteration.

Today every `@table` is identical: resident, and durable if and only if
`WO_DATA` is set for the whole process. Real applications are not uniform —
a session table, a rate-limit counter and a page cache want *resident and
disposable*; an orders table wants *resident and durable*; an audit log wants
*durable and rarely read*. One global switch cannot express that, so it forces
either "everything is precious" or "nothing is".

Extending `@table` moves the decision into the language, where the compiler can
act on it. **Two keys, not one enum** — the developer is answering two
independent questions, and an enum would need a name for every combination:

- **`durable: true | false`** (default `true`). `false` skips the WAL append
  entirely: no record, no fsync, ack from RAM, table empty after restart. The
  compiler can then refuse a program that stores a durable `ref` into such a
  table, because that id would dangle across a restart (WO-E224).
- **`resident: all | keys`** (default `all`). `keys` keeps the id map, the
  secondary indexes and the unique shadows resident and reads rows back from
  the log by offset. This is the key that raises the ceiling — and the
  arithmetic is why it works: 240M rows × 16 B of index ≈ 3.8 GB resident for a
  120 GB table.

`durable: false` with `resident: keys` is refused: rows would be neither logged
nor resident, so there would be nowhere to read them from.

The grammar change was small, as predicted — `Ast.table_cfg` gained two fields
and the parser's argument match two arms. The *semantics* were the work, which
is why iteration 2 is 7 tasks rather than one.

**Does this break principle 7?** It amends it, deliberately, and the amendment
is applied: the **log** is authoritative and residency is a declared per-table
policy. Durability is untouched and unconditional — ack after fsync, replay
whole-or-nothing, torn tails dropped by CRC. What stays rejected is a *second*
engine: a paged B-tree with its own buffer pool. Reading rows from the log we
already write is not that.

An earlier draft of this section proposed a three-valued `mode:` enum including
`cold`. That name conflated durability with residency and could not be defined
before its mechanism existed; the history is in
[iteration 2](02-table-storage-modes.md).

## The sequence

| # | Iteration | Delivers | Needs |
| --- | --- | --- | --- |
| 1 | [RAM ceiling: measure the breaking point](01-ram-ceiling-measurement.md) | ✅ **measured 2026-08-27**: footprint per shape (3.3× apart), the two silent exits (SIGKILL vs swap-serving-from-disk at ~uncapped speed), and ack-after-fsync surviving an OOM kill. Also measured: the **273× random-read collapse** over an oversized table, and replay at **≈5.5 µs/record with a 1.9× history penalty** — iteration 3's "before" | nothing; extends iteration 22's harness |
| 2 | [per-table storage](02-table-storage-modes.md) | ✅ **closed 2026-09-10.** the grammar: `durable: true\|false` and `resident: all\|keys`, per table, replacing the global `WO_DATA` all-or-nothing. Grammar, the `durable` half, keys-resident CRUD and task 7's measurement landed by 2026-08-30; task 6a — refuse `durable: true` without `WO_DATA`, `WO_EPHEMERAL=1` as the whole-program escape, the `.wob` v8 table bit so the rule binds `@table` classes only — landed 2026-09-10; the byte budget (6b) moved to 5 | 3 (the offset map survives compaction — landed); no longer 1, since the budget moved to 5 on 2026-09-09 |
| 3 | [WAL checkpoint](03-wal-checkpoint.md) *(was language 32)* | snapshot + truncate: disk reclaimed, replay bounded | 4 composes |
| 4 | [io_uring group commit](04-io-uring-commit.md) *(was language 23)* | one barrier per DB-actor drain instead of one per statement — part A landed 2026-08-28 (≈2.9× concurrent durable writes); the "66× gap" this row used to name is a serial writer's latency, which part B must re-brainstorm. **Part B re-brainstormed 2026-09-10**: forks 1–5/8–10 settled (`review_pending`); forks 6/7 settled in substance (GO — tmpfs `mixread.p99` on the RAM figure, ext4 3902–4307 µs) but **fold pending** — see `.dev/zack/databasev2-4b.md`. `status: in-progress`, `readiness: refine` until folded | the arc (landed) |
| 5 | [Bounded tables and eviction](05-bounded-tables-eviction.md) | a capacity a `ram` table may not exceed, and what happens when it does; **since 2026-09-09 also the resident byte budget** (iteration 2's former task 6b) as its Phase A. **Brainstormed to `readiness: ready` 2026-09-10** (codd-shoney): twelve forks settled, `review_pending`; Phase A is engine-only and startable, a prebuild brief is recommended before Phase B. `status: pending` — not started | 1 (the budget default follows a measurement) and 2 (the mode a bound attaches to) |
| 6 | [Cold tiering](06-cold-tiering.md) | ⚠ **largely superseded by 2** — `resident: keys` is the ceiling-raiser. Its premise (a user-space resident working set) was rejected in favour of the kernel page cache. Revisit only with a measurement showing the page cache insufficient | — |
| 7 | [Single-file store](07-single-file-db.md) *(was language 33)* | ✅ **closed 2026-09-10.** `WO_DATA=<path>.db` — a file path IS the store: directory or trailing `/` stays byte-identical to today; otherwise the path IS the log, created if absent behind an existing parent, refused (exit 2, naming path + parent) on a missing parent or a non-regular/non-directory path. Compaction and migration temps land beside the file-form log, pinned by a test + a mutation control. Gate leg (task 4, codd-cyril): `just residency` **32 checks, 0 failures**; `db-bench --quick --wo-data-file` **181 checks, 5 failures**, the same 5 as the directory form (`residency.keys.fit` rc 74, databasev2 13's sibling bug, not this defect) | independent |
| 8 | [Query grammar from corpora](08-query-grammar-corpus.md) *(was language 27)* | whole-query `count` (landed 2026-08-16 from the skill-catalog corpus); `exists` waits for a corpus that forces it | independent |
| 9 | [Cross-program tables](09-cross-program-tables.md) *(was language 20)* | attach to a running program's database over local IPC | independent |
| 10 | [Keypair attach auth](10-keypair-attach-auth.md) *(was language 21)* | program identity as a keypair; mutual challenge–response | 9 |
| 11 | [Bounded delta chains](11-bounded-delta-chains.md) | cap a keys-resident row's delta chain in the update path, and give the compaction policy an absolute garbage term (`WO_CKPT_ABS_BYTES`; a separate ceiling was tried and removed) | 2 (fixes a limitation it shipped) |
| 12 | [Schema migrations](12-schema-migrations.md) | ✅ **landed 2026-08-31**: the log describes itself (`WO_WAL_SCHEMA` head record, kind 5); boot diffs by name, transcodes add/delete record by record, refuses everything else by name | 2 (the v7 descriptor, and the delta record it rewrites) |
| 13 | [Fresh-log keys-resident seed SEGV](13-fresh-log-keys-seed-segv.md) | ✅ **fixed 2026-09-10.** A `resident: keys` table's first insert on a fresh log SEGV'd (`wo_wal_fold_row_at` wrote an unguarded `*msg`; the schema head was staged after the first row's offset was captured). Fixed: `wo_wal_next_offset` stages the pending head before returning an offset (`6310078`), plus a NULL-`msg` guard in the fold (`1b6750d`); `test_wal` 6660/0, `make -C runtime test` 21 suites 8462/0, `just residency` 32/0. **Not** the same defect as `residency.keys.fit` rc 74 (compaction/replay of keys-resident offsets), which stays open under codd.md's "Next bugs" | 2 (the offset map), 12 (the schema head record) |
| 14 | [The shop workload](14-shop-workload.md) | what an order-taking web app needs from the store: ordered index + range probe, `skip`, composite unique + check rules, on-delete policy, export/import; group-by carried as a criterion (language track) | 2 (keys-resident index shape), 9 (attach), language 18 (implicit block for cascade) |

```
An arrow points AT the iteration that NEEDS the other.

      2 ◀── 3      2 needs 3 (the offset map survives compaction). Since
      │            5d, 3 also calls 2's row API — the coupling runs both
      ▼            ways. 2 no longer needs 1: its budget moved to 5
1 ──▶ 5            (2026-09-09). 5 needs 1 (the budget default follows a
                   measurement) and 2 (the mode a bound attaches to).
                   Nothing needs 5.
2 ──▶ 11, 12       both need 2: 11 bounds a chain 2 shipped, 12 transcodes
                   the descriptor and the records 2 defined.

4                  composes with 3 on the WAL commit path; NEITHER
                   needs the other. Executed 4 then 3 (chain 5, then 6).
6                  superseded by 2 — not sequenced.
7, 8               independent.
9 ──▶ 10
```

Order rationale: **1 before 5** — it read "1 before 2" until 2026-09-09, when
the budget moved to 5 — because the budget default should follow from a
measurement, not a guess. **3 and 4 matter to 2** for the same reason tiering
onto a never-truncating log would have: `resident: keys` rebuilds its offset map
by scanning the whole log at boot until 3's snapshot persists it.

Amended 2026-08-27: the original rationale sequenced **6** as the ceiling-raiser
after 3, 4 and 5. `resident: keys` took that role into iteration 2, so 6 is
largely superseded and 5 is no longer a prerequisite for anything on the
critical path.

**Corrected 2026-08-29 — the graph above used to say the opposite of this
prose.** It drew `2 ──▶ 3 ──▶ 4`, which reads as 3 needing 2 and 4 needing 3.
Both are backwards. 2 needs 3 (the offset map), and the execution order that
actually happened is **4 before 3** — 4's part A landed 2026-08-28, 3 landed
2026-08-29, which is also what the `chain` field says (4 is chain 5, 3 is
chain 6). The retired `2 ──▶ 5 ──▶ 6` path was still drawn as well. Arrows now
point at the dependency, not at the reader's guess.

**The coupling between 2 and 3 runs both ways as of 5d.** 3's compactor calls
2's row API — the iterator, the offset accessor and its setter — because
compaction moves every record and must re-point the map it invalidates. That
was the hazard 3 recorded; it is now discharged, and it means compaction is not
a pure file operation. Full review in
[`00-databasev2-chain-review.md`](../../00-databasev2-chain-review.md).

## What this track does NOT own

| Not databasev2's | Owner |
| --- | --- |
| `transaction { }` and `@table` feature flags | language [iteration 18](../language-runtime-database/18-memory-db-features.md) — approved spec, left whole on purpose |
| the TTL cache middleware | also language 18 (and [porch 1](../porch/01-store-backed-middleware.md) points there) |
| typed binding of rows into app classes | language [iteration 29 `@derive`](../language-runtime-database/29-compile-time-metaprogramming.md) |
| `fs` mutation verbs, outbound sockets | language [iteration 38](../language-runtime-database/38-content-platform-capabilities.md) |
| benchmark harness and CI | iteration 22 (landed) built the harness; per-change CI is language iteration 30 |
| a paged B-tree storage engine | **nobody, deliberately.** Recorded as rejected in [`discarded.md`](../../plan/discarded.md): the disk story is the WAL. `cold` tiering is not a licence to build SQLite. |

## Review protocol

The language track's, unchanged: one iteration read and approved before the next
starts; every iteration an unsplittable slice with phases, per-phase tasks,
Given/When/Then criteria and an out-of-scope list. Every engine change is gated
by `just employee`, `just db-actor` and `just db-bench` against
`bench/baseline.json` — and any iteration that claims a performance change must
move a number in that baseline, or it did not happen.
