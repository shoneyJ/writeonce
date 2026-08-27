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
workload. A random-read workload over a table larger than the cap is where the
collapse should appear, and it is **not yet measured** — which matters, because
that is exactly the access pattern databasev2 2's `resident: keys` creates.

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
