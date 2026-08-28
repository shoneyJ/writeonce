# db-bench — iteration 22's load generator

The measurement backbone (spec:
`docs/superpowers/specs/2026-08-21-db-bench-design.md`). Pure `.wo`;
every measured mode prints one machine-parsable line per operation
class:

    <op> <count> <ops/sec> <p50us> <p99us>

Timing is per-operation via `time.ticks` (CLOCK_MONOTONIC µs).
Percentiles come from a 1µs-bucket histogram clamped at 20000µs — exact
to the microsecond below the clamp; a p99 AT 20000 means "clamp or
worse". (A histogram, not the spec's reservoir: the language has no
container element-write or sort, and the histogram's tail fidelity is
strictly better. Recorded as a plan deviation.)

## Modes

| mode | what it prices |
| --- | --- |
| `all N` | the throughput campaign in ONE process: seed N, read N/2, query N/10, write N/2. Without `WO_DATA` the store is RAM and dies with the process, so the measured modes must share the seeding run. |
| `seed N` | timed inserts: one parent per 100 children (FK probe each insert, unique-index maintenance per parent), k non-unique (10 rows/key), deterministic v. Writes `Meta` expectation rows. |
| `read N` | indexed take-1 point lookups, LCG-spread keys. |
| `query N` | full equality probes on the k index (≈10 rows each), materialized and counted. |
| `write N` | alternating inserts (disjoint k range 2e6+) and updates through query results. Corrupts the checksum by design — durability legs run on a fresh store. |
| `wal N` | the crash battery's vehicle: insert-only (k range 1e6+), `acked <i>` printed AFTER each insert returns — the return IS the ack (RAM applied, WAL record staged, ONE commit done). |
| `wmix N C` | **databasev2 4:** every op a durable write (update through a query result), C at once. Exists because `mix` writes on one op in ten with C=4 — 20 writes in a quick run, measured mean batch **1.01** — so no existing leg could show whether group commit engages. Histogram kind 2, because a replayed store still holds the seeding run's kind-0/1 `Hist` rows. Seed first. |
| `verify` | store vs its own Meta rows: count, checksum, one unique probe. Exit 3 on mismatch. |
| `verify-acked M` | after kill -9 mid-`wal`: rows 1..M exist with the right v; rows beyond M allowed (acked after the last print flushed). Exit 3 on mismatch. |

## Env knobs

| var | effect |
| --- | --- |
| `WO_DATA=<dir>` | durability on: replay `<dir>/shard-0.wal` at boot, log every write. Without it the store is RAM-only |
| `WO_SHARDS=<n>` | shard count. **`1` means every statement runs inline on shard 0 and group commit cannot engage** — batches form only where writes queue from other shards |
| `WO_WAL_STATS=1` | **databasev2 4:** print one line at exit — `walstats batches=… records=… peak_batch=… peak_staged=…`. Opt-in so it does not pollute every durable program's output. Mean batch is `records/batches`; **mean 1.0 means group commit is not engaging**, which is expected for a serial writer or `WO_SHARDS=1` and a bug anywhere else |

**Do not put `WO_DATA` on `/tmp`.** It is `tmpfs` on the reference machine,
where `fdatasync` is free: the same `wmix` run measured **195 000 ops/s at p50
1 µs** there against **2200 ops/s at p50 7200 µs** on ext4. There is no
durability barrier to price on a memory filesystem. The driver keeps its stores
under `bench/` for exactly this reason.

## Coordination idiom (this side of iteration 31)

There is no request/response surface yet: concurrent modes drive
completion the db-actor way — actors write rows, main polls the store
until the expected count, then settles. Retired when 31 lands.

## Standing finding (2026-08-21, first run)

A hand-built `multi Bucket` of insert results SEGVs on drop: the
compiler classifies the elements OWNED while table refs are scalar ids.
Query-built multis are runtime-typed and safe. Worked around here
(single ref local, bucket-major seeding); the compiler fix is its own
slice.

## Reference-machine numbers (first campaign, 2026-08-21)

`bench/baseline.json` is the contract; headline readings:

- ram seed 245–290k inserts/s; **durable seed ≈4.5k/s** (fsync-per-commit
  ≈220µs each — the gap iteration 23 exists to close).
- reads/queries ≈1.1–1.3M ops/s at p50 1µs since the read-path index
  slice (2026-08-22, engine `wo_idx_probe` + emitter index selection) —
  up from ≈1.5k/s at p50 600µs when point lookups walked every slab
  (~×850). mixread 89k ops/s single-shard, ~1.9k multi-shard (was
  1,280 / 21): the RPC round-trip is now the visible cost, as designed.
- msgrate ≈13M msgs/s same-heap vs ≈2.4M cross-shard (the mutex-inbox
  number, stage-2 deviation 4).
- Tolerance policy lives in the DRIVER (`tolerance_for`), not hand-edits
  — a baseline refresh regenerates it: mix*/sN/read/query 50%
  (scheduling + µs-scale jitter), rest 15%; latency floors
  `max(4×value, 100µs)` — the tripwire means "µs became ms".

## The gate must bite (proven 2026-08-21)

`scripts/db-bench.py --check <results.json>` evaluates a recorded run:
the real results pass 74/0; a doctored copy FAILS on exactly the
doctored metrics — use a 15%-class metric (seed) halved plus a
50%-class metric (read) quartered, so both tolerance classes prove they
bite. Re-run the smoke after any gate or policy change.
