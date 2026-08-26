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

**An allocation failure is a catchable trap, not a crash.** Every `malloc` in
the row encoder is checked and jumps to an `oom` label; `DB_ERR_OOM` maps to
`WO_T_OOM`, which a program can `try`/`catch`. So a writeonce program that runs
out of memory *refuses the insert* rather than corrupting or dying. That is a
much better starting position than most engines have.

**But the trap is almost never what a real deployment hits first.** Long before
`malloc` returns NULL, the box starts swapping, and a RAM-authoritative database
on swap is the worst of both worlds: it has paid for in-memory data structures
and is now serving them from disk with no read path designed for that. On a
cgroup-limited host the OOM killer arrives instead, and an external `SIGKILL` is
the one shutdown path that skips every guarantee the WAL was written to provide —
though ack-after-fsync means acked writes still survive; iteration 22's `kill -9`
battery proves that much.

So the honest problem statement is not "malloc fails". It is: **there is no
declared budget, no back-pressure as the budget is approached, and no way to
distinguish data that must be resident from data that merely is.** Iteration
[1](01-ram-ceiling-measurement.md) exists to replace this paragraph with
numbers before anything is designed on top of it.

## The lever: per-table storage modes

The developer's ask, and the reason this track has a grammar iteration.

Today every `@table` is identical: resident, and durable if and only if
`WO_DATA` is set for the whole process. Real applications are not uniform —
a session table, a rate-limit counter and a page cache want *resident and
disposable*; an orders table wants *resident and durable*; an audit log wants
*durable and rarely read*. One global switch cannot express that, so it forces
either "everything is precious" or "nothing is".

Extending `@table` with a storage mode moves the decision into the language,
where the compiler can act on it:

- **`ram`** — resident, never WAL-logged, gone on restart. The compiler knows
  no durability code is needed; the engine knows these rows are the first
  candidates to shed under pressure; and — the part that matters — a program
  that expects a `ram` table to survive a restart is now stating something the
  compiler can refuse.
- **`durable`** — today's behaviour: resident and WAL-logged, ack after fsync.
- **`cold`** — durable, and *not* required to be resident. This is the mode that
  actually raises the ceiling, and it is the one with real design work behind it
  (iteration [6](06-cold-tiering.md)).

The grammar change is small and the surface is already the right shape:
`Ast.table_cfg` is `{ table_name; indexes }`, the parser's argument match
already rejects unknown keys with a catalogued diagnostic
(`unknown @table argument ... (supported: name, index)`), and adding one more
key follows the path `index` already cut. The *semantics* are the work, not the
syntax — which is exactly why it gets its own iteration
([2](02-table-storage-modes.md)) and why it comes after the measurement.

This is also the honest answer to "does this break principle 7?" It does not.
RAM stays authoritative **for the tables that say so**. `cold` is a declared
exception a developer opts into per table, with the trade written at the
declaration site rather than buried in an operations runbook.

## The sequence

| # | Iteration | Delivers | Needs |
| --- | --- | --- | --- |
| 1 | [RAM ceiling: measure the breaking point](01-ram-ceiling-measurement.md) | what actually happens from 50% RAM to OOM — swap onset, latency cliff, trap behaviour, `kill -9` survival | nothing; extends iteration 22's harness |
| 2 | [`@table` storage modes](02-table-storage-modes.md) | the grammar: `mode: ram \| durable \| cold`, per table, replacing the global `WO_DATA` all-or-nothing | 1 for its defaults |
| 3 | [WAL checkpoint](03-wal-checkpoint.md) *(was language 32)* | snapshot + truncate: disk reclaimed, replay bounded | 4 composes |
| 4 | [io_uring group commit](04-io-uring-commit.md) *(was language 23)* | close the 66× durable/RAM write gap (4.5k vs 297k inserts/s) | the arc (landed) |
| 5 | [Bounded tables and eviction](05-bounded-tables-eviction.md) | a capacity a `ram` table may not exceed, and what happens when it does | 2 |
| 6 | [Cold tiering](06-cold-tiering.md) | rows that leave RAM and come back — the iteration that raises the ceiling | 2, 3, 5 |
| 7 | [Single-file store](07-single-file-db.md) *(was language 33)* | `WO_DATA=<path>.db` — a file path IS the store | independent |
| 8 | [Query grammar from corpora](08-query-grammar-corpus.md) *(was language 27)* | whole-query `count`, `exists` | independent |
| 9 | [Cross-program tables](09-cross-program-tables.md) *(was language 20)* | attach to a running program's database over local IPC | independent |
| 10 | [Keypair attach auth](10-keypair-attach-auth.md) *(was language 21)* | program identity as a keypair; mutual challenge–response | 9 |

```
1 ──▶ 2 ──▶ 5 ──▶ 6
      │            ▲
      3 ──▶ 4 ─────┘
7, 8 independent
9 ──▶ 10
```

Order rationale: **1 before 2** because a mode's default should follow from a
measurement, not a guess. **3 and 4 before 6** because tiering onto a log that
never truncates would make the disk problem worse, not better. **5 before 6**
because eviction from a bounded resident table is the simpler half of the same
mechanism, and getting the policy right there de-risks the hard half.

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
