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

## 6. WAL group commit: one barrier per drain (databasev2 4 part A)

**Measured 2026-08-28.** Before this, the engine committed per *statement*:
`db.c` called `wo_wal_commit` immediately after every append, so each row
change bought its own `pwrite` + `fdatasync`. Now shard 0 stages every queued
write request, issues one barrier, and only then releases the held replies.

### The controlled before/after

Same machine, same workload (`wmix 4000 32` — every op a durable update, 32
concurrent), same build except `db.c` and `vm.c`, two runs each, interleaved:

| | ops/sec | p50 | p99 |
| --- | --- | --- | --- |
| per-statement barrier | 2213 · 2177 | 7183 · 7251 µs | **20000 · 20000 µs** |
| group commit | **6216 · 6525** | **3458 · 3444 µs** | 11139 · 5971 µs |

**≈2.9× throughput, ≈2.1× lower p50.**

**The p99 "before" figure is at the histogram ceiling, not a measurement.**
`hist_add` clamps at 20000 µs, and both before-runs pinned there — so the true
before p99 is ≥20 ms and unknown. The improvement is *at least* 2.3×; the
honest statement is that the old p99 was off the end of the instrument.

### Confirmation from the committed baseline

The full campaign gives the same answer a second way. `s1` takes the inline
path, which commits per statement **by design**, so within one build the two
shard configurations are batching-off against batching-on:

| Leg | ops/sec | p50 | p99 | mean batch | peak batch |
| --- | --- | --- | --- | --- | --- |
| `durable.s1.wmix` (inline, unbatched) | 1467 | 455 µs | 721 µs | **1.0** | 1 |
| `durable.sN.wmix` (batched) | **5117** | 8208 µs | 12169 µs | **5.43** | 57 |

3.5× throughput, agreeing with the 2.9× above. Note `sN` latency is *higher*
while throughput is 3.5× better: 64 writers queueing behind one owner shard
trade per-op latency for barrier amortisation, which is what group commit is.

Batching scales with write concurrency exactly as designed — mean batch at
C = 4 / 16 / 64 was **1.13 / 1.76 / 5.35**, peak **3 / 10 / 39**.

### What did NOT improve, and why that was predicted

`durable.sN.mixwrite` went **480 → 492 ops/s** — unchanged. That is the metric
the spec *originally* named as the payoff, and correcting it was part of the
brainstorm: `mix` writes on one op in ten with C=4, so a quick run performs
**20 writes** and mean batch measured **1.01** over 3112 barriers. A workload
that never has two writes in flight cannot be helped by batching them.
`durable.*.seed` is likewise unchanged: a serial single writer has nothing to
batch with under any scheme.

**So the payoff is real but conditional: it appears exactly where concurrent
durable writes fan into the owner shard, and nowhere else.**

### Two traps worth recording

**Do not benchmark durability on `/tmp`.** It is `tmpfs` here, where
`fdatasync` is free — the same `wmix` run reported **195 000 ops/s at p50 1 µs**
there against **2200 ops/s at p50 7200 µs** on ext4. There is no barrier to
amortise on a memory filesystem, so a group-commit measurement taken there
measures nothing. `db-bench` gets this right by keeping its stores under
`bench/`.

**The record count is not the update count.** `wmix` staged 7755 records for
4000 updates because the histogram dump and the done-marker are themselves
durable inserts. They arrive as an end-of-run burst, which is batch-friendly,
so `mean_batch` is not purely update-driven. Peak staged bytes stayed small
(2793 B at C=64), which is what settled the decision to ship **no batch cap**:
the request queue's existing upstream bound is sufficient.
