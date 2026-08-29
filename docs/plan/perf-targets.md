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

### The cost side: tail latency on the owner shard

Group commit is a trade, and the full battery made the other side of it visible.

**A bug first, caught by `durable.sN.mixread.p99`.** The drain initially held
*every* DB reply until the barrier — including **reads**, which stage nothing and
have no stake in durability. That parked readers behind an fsync for no reason
and pushed read p99 from ~1043 µs to **4057 µs**. Reads are now released
immediately; only a statement that actually staged a record has its reply held.

**What remains is inherent, not a bug.** A barrier now blocks the owner shard
**longer** (more records per fsync) even though it blocks **less often**, so
anything arriving during a barrier — reads included — waits behind it. Measured
across three full runs of the same build, `durable.sN.mixread.p99` came in at
**1043 / 2318 / 4147 µs** and `wmix.p99` at **8758 / 20000 µs**, a 2–4× spread
with the box near idle.

So the honest summary of part A on a single-threaded owner shard: **~3× write
throughput, at the price of a longer and noisier tail for everything queued
behind a barrier.** That is precisely what part B (async submission — submit the
barrier and keep serving) would undo, and it is a better argument for part B than
the "close the 66× gap" framing part B was originally given.

**Gating consequence.** `durable.sN.*.p99us` now carries a 100% tolerance,
because a 2–4×-variable tail gated at 50% gates the disk rather than the engine.
The **floor** is the real guard there, and it is not slack: `mixread`'s floor
(4172 µs) came within 25 µs of tripping on the worst observed run.

## 7. WAL checkpoint: compaction (databasev2 3)

**Measured 2026-08-29.** Before this the log grew forever: nothing ever removed
superseded records, so boot replayed all history and the file only ever got
bigger. Compaction rewrites it as one record per live row and swaps it in with
`rename`.

### Space and boot — the same workload, twice

Identical work, differing only in whether checkpointing may fire (an enormous
floor disables it). Full campaign:

| | checkpointing off | checkpointing on |
| --- | --- | --- |
| WAL used | 1 962 358 B | **907 094 B** |
| boot (median of 3, `boot` mode) | 114 ms | **64 ms** |
| compactions | 0 | 6 |

**2.16× space reclaimed, 1.78× faster boot.** Boot is measured with a mode that
does nothing at all: with `WO_DATA` set the runtime replays the whole log before
`main` runs, so a mode with no work of its own is the only honest way to price
replay. It is *not* measured through the driver's `run()` helper, which samples
RSS on a 250 ms poll — timings taken that way reported "251 ms" both with and
without checkpointing, which is the harness's clock rather than the engine's.

### The stop-the-world pause, and why it stopped being 8× worse

Compaction blocks the owner shard for its duration. The spec refused to assume
that was acceptable, so it is measured and gated against a stated **50 ms**
budget: a stall a serving process can absorb without a client seeing a timeout.

Measured **2 651 µs** on the full campaign — comfortably inside it.

It was not always. The first implementation flushed the dump through
`wo_wal_commit`, which `fdatasync`s, so a dump paid one barrier per 256 records:

| live set | pause, per-flush fsync | pause, one final fsync |
| --- | --- | --- |
| ~107 KB | 23 948 µs | **2 903 µs** |
| ~500 KB | 36 361 µs | **7 526 µs** |
| ~1.98 MB | 107 649 µs | **13 212 µs** |

Marginal rate went from **~22 MB/s to ~181 MB/s** — from sync-bound to
bandwidth-bound. Intermediate durability during a dump is worthless: the temp
file is not authoritative until the rename and is fsynced once immediately
before it, so those barriers bought nothing and cost 8×.

**The pause is O(live rows), and that is the number that eventually forces an
incremental design.** At ~181 MB/s a 1 GB live set implies roughly 5.5 s — well
past any interactive budget. The spec deliberately did not buy incremental
copying in advance; this is the measurement it is to be bought against.

### Gating

`ckpt.reclaim_x` is the feature's central claim and is gated tightly (15%).
Everything else in the leg — boot times, the pause, the byte counts — is
wall-clock or workload-shaped on a shared box and carries a wide tolerance,
because waiving them *all* would have left the leg ungated. The leg also
asserts two things directly rather than trusting a metric: that some compaction
actually ran (otherwise it proves nothing), and that the log really is smaller
with checkpointing on.

One direction bug worth recording: `reclaim_x` was first recorded as
lower-is-better by the default detector, which would have **passed "reclaimed
nothing" and failed an improvement** — the central claim gated backwards.

**Gate-tolerance corrections made while closing this iteration**, both recorded
because a widened tolerance that is not justified is indistinguishable from a
silenced regression:

- **`ckpt.pause_us_max` is no longer gated against a baseline.** The raw pause
  scales with the live set, and this workload's live set is not fixed —
  `wmix`'s `hist_dump` inserts a row per latency bucket, so a noisier box makes
  more buckets, more rows, and a longer pause. What belongs to the engine is the
  **rate**, so `ckpt.pause_us_per_mb` carries the real tolerance and the raw
  pause keeps the absolute 50 ms budget as its guard.
- **`ram.*.msgrate.msgs_sec` moved from 15% to 70%, and this one is
  pre-existing.** Across the ten full runs recorded on 2026-08-28/29 — several
  predating the checkpoint work — it ranged **10.7M to 17.9M msgs/sec, a 1.67×
  spread**. A 15% gate on a scheduling-bound throughput metric fails
  intermittently whatever the engine does.
- **`durable.sN.*.p99us` moved from 100% to 300%**, with more evidence than the
  first widening had: mixread p99 measured 1043 / 2318 / 4147 µs and mixwrite
  1623 / 4446 µs across runs of the same build. The floors remain the real
  guard, and they are not slack — mixread's came within 25 µs of tripping.
