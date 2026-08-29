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

## 5. The RAM ceiling: footprint, and how the engine actually dies

**Measured 2026-08-27** (databasev2 1), rootless cgroup v2 via
`systemd-run --user --scope -p MemoryMax=N -p MemorySwapMax=M`, dev box.

### Per-row resident footprint, by shape

| Shape | Columns | Steady-state | Doubling steps |
| --- | --- | --- | --- |
| Int-only (`Item`) | 2× Int + 1 ref | **96.5 B/row** | at ~24k and ~48k rows |
| Text-heavy (`Wide`) | 1× Int + 3× Text | **320.6 B/row** | at ~24k and ~48k rows |

**3.3×**, not the "order of magnitude" an earlier doc asserted. Two shapes are
published, never one number: a `Text` column is a separate `db_text` allocation
per row on top of the slab slot, so a row count cannot bound RAM.

**Read the steady-state figure as the median of per-interval marginals, not a
two-point slope.** The id hash and index buckets are open-addressing pow2 and
double periodically; a two-point slope lands arbitrarily on or off a doubling
and swings 2× (96 vs 205 B/row measured for the same shape). The doublings are
reported separately because a **transient RSS step is exactly what a
resident-footprint budget must leave headroom for** — a budget without it fires
during a rehash rather than at a steady-state threshold. Direct input to
databasev2 2's budget design.

### How it dies — and it is not the way the docs claimed

| Allocator | Ceiling | Failure mode |
| --- | --- | --- |
| VM object arena | `WO_HEAP_MB`, checked | `trap 4 … out of memory`, rc=1, reportable. Verified at 4 and 16 MiB |
| table storage (slabs + heap values) | **none** | **SIGKILL, signal 9** (shell rc 137). Verified at 360 000 rows / 57 188 KiB under a 64 MiB cap |

Three docs asserted that an allocation failure surfaces as a catchable
`WO_T_OOM`. For table storage it does not: `vm.overcommit_memory = 0` means
`malloc` succeeds and the kernel kills the process when it *touches* the pages,
so the checked-`malloc` code never runs. The trap path is real, but it is the
arena's.

**Consequence, and the strongest available argument for databasev2 2's byte
budget:** a declared budget is the *only* way table storage can acquire a
checked ceiling, because `malloc` under default overcommit will never report a
problem. **Owner: databasev2 2.**

### Swap: the ceiling that does not announce itself

| Leg | 900 000 Int rows, 64 MiB cap | Wall | Final RSS |
| --- | --- | --- | --- |
| swap OFF (`MemorySwapMax=0`) | **SIGKILL at 360 000 rows** | — | 57 188 KiB |
| swap ON (256 MiB) | **completed, exit 0** | **148 s** | 62 264 KiB (rest paged out) |
| uncapped | completed, exit 0 | **150 s** | 169 416 KiB |

**Swap cost ~1%.** A prior draft predicted "latency collapse"; the prediction had
the wrong sign. Inserting is append-mostly, so cold pages are written once and
never re-read — paging is sequential and off the critical path. The swap device
is a real disk file (`/swap.img`; no zram, zswap disabled), so this is genuine
disk paging.

**Do not generalise this to "swap is fine".** It measures an append-mostly
workload — and the opposite pattern was then measured too, below.

The operational consequence is that the RAM ceiling has two shapes and neither
reports itself: without swap the process vanishes on signal 9, with swap it
keeps returning 0 while serving from disk. A budget that fires at a *declared
threshold* is the only one that can speak before either happens.

### Durability across the ceiling

60 000 Int rows, 8 MiB cap, swap off, `WO_DATA` set — the process is OOM-killed
mid-insert, then replayed:

| Claim | Result |
| --- | --- |
| the survivor is a contiguous prefix | ✅ ~40 000 rows, rows 1..M all present |
| every surviving row's payload is correct | ✅ every `v` matches `item_v(i)` |
| the truncated tail is not read as corruption | ✅ replay exits 0 |

**Ack-after-fsync holds through an OOM kill** — the one shutdown path that skips
every cleanup handler. Gated as `db-bench`'s `ceiling` leg, which asserts the
*shape* of the survivor rather than its size: where the SIGKILL lands is the
scheduler's business, so `rows_recovered` carries ±100% tolerance. The leg
asserts the exit but never records it as a metric, so that when databasev2 2's
byte budget turns the kill into a checked refusal, the gate does not fail on the
improvement.

### Random reads over an oversized table: 273×

60 000 Int rows, both legs reading the **same** Weyl key order
(`i*2654435761 mod n`), differing only in the cap:

| Leg | Cap | Throughput | p50 | p99 |
| --- | --- | --- | --- | --- |
| all resident | 256 MiB | **1 851 166 reads/s** | 0 µs | **1 µs** |
| over-cap, swap on | 6 MiB | **6 771 reads/s** | 128 µs | **487 µs** |

All 20 000 reads resolved in both legs, so this is the cost of faulting pages
back, not of failed lookups. Swap-off is not an option in this configuration —
it is SIGKILLed.

**The two access patterns are ~270× apart under identical memory pressure:**

| Pattern | Cost of exceeding RAM |
| --- | --- |
| append-mostly insert | **~1%** (cold pages written once, never re-read) |
| random read across the table | **273×** |

**Departure is a step, not a curve.** 1 µs to 487 µs with nothing in between —
`p99_departure_decile` looks for a gentle knee that does not exist. Residency is
close to binary, which is why a budget must fire at a *declared* threshold: there
is no early warning in the latency signal to react to.

**Mechanism caveat, and it is a design input for databasev2 2.** This is
demand-paging of *anonymous slab memory* through swap — 4 KiB per fault, no
readahead. `resident: keys` instead `pread`s rows from the WAL, through the
**page cache**: same physical constraint, different mechanism, plausibly a better
constant because file reads get readahead and a shared cache. **That is a
hypothesis.** 273× bounds what *swapping* costs; iteration 2 must measure its own
read path rather than inherit this figure.

Gated as `db-bench`'s `randread` leg, which gates the **ratio** — the absolute
reads/sec of the over-cap half is the box's swap device, while the factor between
two runs differing only in their cap is the engine's.

### Replay: boot cost tracks history, not data

Two stores with the **same 20 000 live rows** and different history lengths.
Process startup (3.5 ms, empty store) is subtracted, so these are replay:

| Shape | Records | WAL used | Replay | Per record |
| --- | --- | --- | --- | --- |
| N inserts | 20 000 | 980 035 B | **110 ms** | 5.5 µs |
| N inserts + N updates | 40 000 | 1 960 035 B | **211 ms** | 5.3 µs |

**1.9× the boot cost for an identical dataset.** Per-record cost is flat, so
replay is linear in **records**, not rows. An update appends a record and nothing
ever collapses it, so a row updated a thousand times costs a thousand records at
every boot, forever.

Extrapolated at 5.5 µs/record: **10M records ≈ 55 s of boot, 100M ≈ 9 minutes.**

This is the "before" databasev2 3 lacked — `bench/baseline.json` carried no
replay, restart, boot or recovery metric at all, because iteration 22 proved
restart *correctness* and never timed it. Gated as `db-bench`'s `replay` leg:
`replay.inserts.*`, `replay.history.*`, `replay.history_penalty_x`. Per-record
cost is stored in **nanoseconds** — as µs it rounded 5.5 and 5.3 to 6 and 5,
which is too coarse for the one number a checkpoint is meant to improve.

WAL bytes are measured as the file's **non-zero prefix**, never its size: shard
WALs are `fallocate`'d to 1 MiB, so an empty store reports 1048576.
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
