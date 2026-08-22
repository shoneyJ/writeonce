# Performance targets — measured, named, waiting

A register like [`discarded.md`](discarded.md)/[`learnings.md`](learnings.md):
optimization candidates that exist because a NUMBER says so, not a
hunch. Every row cites its measurement (the db-bench campaign,
`bench/baseline.json`, or the [go-sqlite comparison](../../bench/compare/go-sqlite/README.md))
and names an owner iteration when one exists. A target leaves this file
by landing (delta recorded in the baseline) or by being rejected into
`discarded.md` with its reason.

## 1. The write path — update-through-query re-probes, insert re-encodes

**Measured 2026-08-22** (go-sqlite comparison, N=20k, same machine,
ext4): ram mixed writes 195,465 ops/s vs SQLite's 380,069 (×1.9
behind); durable mixed writes 2,324 vs 3,257 (×1.4 behind) — while
writeonce WINS durable seed ×1.4 and reads ×2.6–6.4. The write gap is
specifically the UPDATE half of the mix.

Suspected costs, in probable order (attribute before optimizing — the
C-API microbench the 22 spec reserves exists for exactly this):

1. **Update-through-query runs a whole query statement per update**:
   probe (now O(1)) + materialize an id `multi` (arena alloc) +
   `DB_GET_FIELD`/`DB_UPDATE_FIELD` builtin round-trips per touched
   field. SQLite's equivalent is one page write inside one statement.
2. **`wo_row_update_field` walks every index three times** (shadow
   unique check, old-entry removal, new-entry add — three
   `touches`-loops over `t->indexes` per update; see
   `database/src/table.c`).
3. **Insert encodes per field with a malloc per text/owned value**
   (`db_val_encode`) — visible as ram seed ×1.2 behind SQLite (245k vs
   297k) even though the durable flavor wins.
4. **A WAL update record re-encodes the whole row**
   (`wo_wal_append_update` writes the row image, not a delta).

**Owner:** none yet. Sequence note: iteration 23 (io_uring
group-commit) rewrites the durable write path's syscall story anyway —
re-measure after 23 lands, then decide whether the RAM-side costs
(1–3) earn their own slice. Acceptance shape: ram write ops/s closes
on SQLite's number with reads unharmed; baseline refreshed with the
delta recorded.

## 2. Cross-shard DB RPC halves concurrent read throughput

**Measured 2026-08-22** (db-bench campaign): ram mixread 89,538 ops/s
single-shard vs 44,918 at default cores; durable 9,211 vs 4,324. The
DB actor serializes every statement on shard 0 and each op pays an
envelope + park/unpark round-trip.

**Owner: by design, priced deliberately** (story 8's settled decision
1 — rejected alternatives: engine lock, partitioned tables "wait for a
measured need"). THIS is the measured need's first data point; the
recorded escalation path is partitioned/replicated read state, only if
a real workload (iteration 24's chat) hurts. Not actionable before 24.

## 3. The mutex inbox costs ~6× on cross-shard message rate

**Measured 2026-08-22**: 16.7M msgs/s same-heap vs 2.85M cross-shard
(`msgrate`). **Owner: iteration 31** (mailbox/backpressure decisions
consume this number) and stage-2 deviation 4 (lock-free rings arrive
only if the mutex is the measured bottleneck — at 2.85M msgs/s it is
not the limiting factor for any current workload).

## 4. Durable write throughput is fsync-bound at ~4.5k/s

**Measured 2026-08-21**: durable seed 4,460 inserts/s vs ram 245k —
the ~55× gap is one fdatasync per statement (~220µs each).
**Owner: iteration 23** (io_uring group-commit) — its acceptance is
literally this number moving while the crash battery stays green.
