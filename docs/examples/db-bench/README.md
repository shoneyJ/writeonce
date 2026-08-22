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
| `verify` | store vs its own Meta rows: count, checksum, one unique probe. Exit 3 on mismatch. |
| `verify-acked M` | after kill -9 mid-`wal`: rows 1..M exist with the right v; rows beyond M allowed (acked after the last print flushed). Exit 3 on mismatch. |

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

- ram seed 257–298k inserts/s; **durable seed ≈4.5k/s** (fsync-per-commit
  ≈220µs each — the gap iteration 23 exists to close).
- reads ≈1.5k/s at p50 ≈600µs on a 20k-row store: point lookups are
  O(table) — the probe walks every slab; index selection never reaches
  the lookup path. THE read-path finding.
- mixread 1,280 ops/s single-shard vs **21 ops/s** multi-shard: RPC
  round-trip × O(table) probes × owner serialization — the arc's honest
  price until reads index properly.
- msgrate 13.4M msgs/s same-heap vs 2.45M cross-shard (the mutex-inbox
  number, stage-2 deviation 4).

## The gate must bite (proven 2026-08-21)

`scripts/db-bench.py --check <results.json>` evaluates a recorded run:
the real results pass 74/0; a doctored copy (one ops/sec halved) FAILS
on exactly that metric. Re-run the smoke after any gate change.
