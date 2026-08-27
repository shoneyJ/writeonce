---
track: databasev2
iteration: "1"
status: done
readiness: ready
---

# databasev2 1 — the RAM ceiling: measure the breaking point

> Part of [Story — databasev2: the database beyond RAM](00-story.md).
>
> **Refined 2026-08-27; the three forks are settled below and the decisions are
> locked.** No spec document: the deliverable is numbers plus a harness leg, and
> the design fits in this file — the same call
> [7](07-single-file-db.md) makes.
>
> **First because the repo's own doctrine says so.** "Always inspect crashsites.
> Always measure. Never assume." Two other iterations already cite numbers this
> one was supposed to produce: [2](02-table-storage-modes.md)'s resident-footprint
> budget defaults to a fraction of host memory whose value comes from here, and
> [3](03-wal-checkpoint.md)'s before/after replay criterion has no "before"
> because `bench/baseline.json` carries 75 metrics and **zero** for replay,
> restart, boot or recovery. Iteration 22 proved restart *correctness*; it never
> timed it.

## The design, as settled

**Measure the curve, not the cliff.** Everything dies at the ceiling; what
matters is the shape on the way down — where p99 leaves its 1µs baseline, what
insert throughput does as slabs stop coming from a warm allocator, and how much
warning there is between "fine" and "unusable".

### Fork 1 — the limit mechanism: rootless cgroup v2 via `systemd-run --user`

`systemd-run --user --scope -p MemoryMax=N -p MemorySwapMax=M`. Verified on the
dev box: the `memory` controller is delegated to
`user.slice/user-<uid>.slice`, a scope's `memory.max` reads back exactly as set,
and no passwordless sudo is needed. Being cgroup-scoped also isolates the
measurement from whatever else the box is doing, which matters — the dev box was
at 22.9 of 31.7 GiB with 4.6 GiB of swap already in use when this was refined.

`ulimit -v` is **rejected**: it bounds address space, not resident set, which is
the wrong quantity for an engine that `malloc`s slabs — and it is actively
broken under ASan, whose huge virtual reservations trip it long before any real
memory pressure.

If the mechanism is unavailable (no systemd, no delegation), the harness **skips
the growth legs loudly and names why**. It must never silently fall back to
measuring an uncapped box, because "it survived on a 32 GiB workstation"
measures the workstation.

### Fork 2 — the reference shapes: both, reported separately

Per-row footprint differs substantially between an Int-only row and a text-heavy
one, because a `Text` column is a separate `db_text` allocation per row on top of
the slab slot. **Measured 2026-08-27: 96.5 B/row Int-only vs 320.6 B/row with
three Text columns — 3.3×.** An earlier draft of this section said "an order of
magnitude"; that was an unmeasured guess and this iteration exists to replace
exactly that kind of claim. 3.3× is still more than enough to make a single
"bytes per row" number useless, which is the decision it was supporting.

`db-bench` already supplies half of this: `items` (`k: Int`, `v: Int`, plus a
`bucket` ref) is the Int-only reference as it stands. The work is one text-heavy
shape beside it, with footprint reported per shape.

### Fork 3 — swap: in scope, as a controlled dimension

Not a confound to wish away — `MemorySwapMax` is the knob that separates the two
exits this iteration exists to characterise. Both were measured, and **both
turned out differently than this iteration predicted.**

| Leg | Predicted | Measured |
| --- | --- | --- |
| swap-off | catchable `WO_T_OOM` from checked `malloc` | **SIGKILL, signal 9** (shell rc 137). No trap, no message |
| swap-on | latency collapse | **no degradation at all**: 148 s vs 150 s uncapped |

**Prediction 1 was wrong because of overcommit.** With `vm.overcommit_memory = 0`
`malloc` succeeds and the process dies when it *touches* the pages, so table
storage never gets the chance to report failure. The trap path is real but
belongs to a different allocator:

| Allocator | Ceiling | Failure mode |
| --- | --- | --- |
| VM object arena | `WO_HEAP_MB`, checked | `trap 4` / `WO_T_OOM`, exit 1, reportable |
| table storage (slabs + heap values) | **none** | SIGKILL under overcommit |

**This is the strongest argument available for [iteration 2](02-table-storage-modes.md)'s
byte budget:** a declared budget is the only way table storage can acquire a
checked ceiling, because `malloc` under default overcommit will never tell it
there is a problem.

**Prediction 2 was wrong because of access pattern.** 900 000 Int rows under a
64 MiB cap with 256 MiB of swap finished in **148 s** with RSS pinned at 62 MiB;
the same workload uncapped took **150 s** at 165 MiB RSS. Swap cost
approximately nothing. The reason is that inserting is append-mostly: cold pages
are written out once and never read again, so paging is sequential and off the
critical path. The swap is a real disk file (`/swap.img`, no zram, zswap
disabled), so this is genuine disk paging, not compressed RAM.

**The correct generalisation is narrower than "swap is fine", and the narrow
claim was then measured too.** The 1% figure belongs to an append-mostly
workload. Reading *randomly* across a table larger than the cap collapses
**273×** — see below. Same cap, same swap, opposite access pattern.

## Progress

| Piece | State |
| --- | --- |
| `Wide` text-heavy reference shape (`db-bench/types.wo`) | ✅ |
| `growth N int\|text` — insert, per-decile RSS and read latency | ✅ |
| the sample reads its OWN RSS via `/proc/self/status` | ✅ — the driver polls every 250 ms and would miss the value *at* a decile boundary |
| `growth-verify` — the survivor is a contiguous intact prefix | ✅ |
| rootless cap wrapper, swap on/off legs | ✅ 4 footprint legs |
| footprint metric = **median of marginals**, doublings counted separately | ✅ |
| `ceiling` leg: dies at the cap, then replay must be intact | ✅ gated |
| `randread` leg: control vs over-cap, same key order | ✅ gated |
| baseline + tolerance policy | ✅ 148 checks; footprint at ±10%, kill-timing metrics at ±100% |
| `perf-targets.md` §5 | ✅ |
| **resident-footprint fraction for [iteration 2](02-table-storage-modes.md)** | ⬜ **not delivered — the premise it rested on is false**, see Outstanding |
| `boot` + `replayseed N M` + the `replay` leg — [iteration 3](03-wal-checkpoint.md)'s "before" | ✅ **≈5.5 µs/record, 1.9× history penalty** |
| `randread N R` + the `randread` leg — random reads over an oversized table | ✅ **273x collapse measured** |

## Measured

Footprint, reproducible inside 2% across runs:

| What | Int-only (`Item`) | Text-heavy (`Wide`) |
| --- | --- | --- |
| steady-state footprint | **96.5–100 B/row** | **320.6–324 B/row** |
| doubling steps | 3 (at ~24k and ~48k rows) | 2 |
| base process RSS | ≈ 3.9 MiB, excluded from the per-row figure | same |

Ratio **3.3×** — not the "order of magnitude" an earlier draft asserted. Enough
on its own to make a single "bytes per row" number useless, which is the decision
it was supporting ([2](02-table-storage-modes.md), fork 5).

The ceiling, 60 000 Int rows under an 8 MiB cap with swap off and `WO_DATA` set:

| Question | Answer |
| --- | --- |
| how does it die? | **SIGKILL, signal 9.** No refusal, no diagnostic |
| what survives? | **a contiguous intact prefix** — ~40 000 rows, every `v` correct, no holes, not reported as corruption |

**Ack-after-fsync holds through an OOM kill.** That is the one shutdown path
which skips every cleanup handler, and the durable prefix came back whole.

Random reads over an oversized table — 60 000 rows, same Weyl key order in both
legs, only the cap differs:

| Leg | Cap | Throughput | p50 | p99 | RSS after fill |
| --- | --- | --- | --- | --- | --- |
| control, all resident | 256 MiB | **1 851 166 reads/s** | 0 µs | **1 µs** | 13 508 KiB |
| over-cap, swap on | 6 MiB | **6 771 reads/s** | 128 µs | **487 µs** | 6 980 KiB |

**273× throughput collapse, ~480× on p99.** All 20 000 reads resolved correctly
in both legs, so this is the cost of faulting pages back in, not of failing
lookups. Swap off is not an alternative here: that configuration is simply
SIGKILLed.

So the two access patterns sit ~270× apart under identical memory pressure:

| Access pattern | Cost of exceeding RAM |
| --- | --- |
| append-mostly insert | **~1%** — cold pages written once, never re-read |
| random read across the table | **273×** — almost every read faults |

**The mechanism caveat matters for [iteration 2](02-table-storage-modes.md).**
This measures demand-paging of *anonymous slab memory* through swap: 4 KiB at a
time, on fault, with no readahead. `resident: keys` will instead `pread` rows
from the WAL, which goes through the **page cache** — the same physical
constraint (data larger than RAM means disk I/O) but a different mechanism, and
plausibly a better constant, because file reads get readahead and a shared cache
while swap-in does not. **That is a hypothesis, not a result.** The honest
reading is that 273× bounds what *swapping* costs, and iteration 2 must measure
its own read path rather than inherit this number.

Replay, and it is iteration 3's whole case — same live dataset, different
history length:

| Shape | Records | WAL | Replay | Per record |
| --- | --- | --- | --- | --- |
| N inserts | 20 000 | 980 035 B | **110 ms** | 5.5 µs |
| N inserts + N updates | 40 000 | 1 960 035 B | **211 ms** | 5.3 µs |

**20 000 live rows either way. 1.9× the boot cost.** Per-record cost is flat
(5.5 vs 5.3 µs), so replay is linear in **records, not rows** — boot replays
*history*. A row updated a thousand times costs a thousand records at every boot,
forever, because nothing ever collapses them. Process startup (3.5 ms on an
empty store) is subtracted, so these are replay, not spawn.

Extrapolated at 5.5 µs/record: **10M records ≈ 55 s of boot, 100M ≈ 9 minutes.**
That is the number [iteration 3](03-wal-checkpoint.md) exists to bound, and it
had no "before" until now.

**The finding that matters most is the swap leg succeeding.** It did not fail,
did not warn, and returned 0. A deployment in that state looks healthy while
serving from disk. That is the exit with no error signal, and it is why
[iteration 5](05-bounded-tables-eviction.md)'s back-pressure must act at a
declared threshold rather than at exhaustion — exhaustion either kills without
warning or silently does not arrive.

## Acceptance Criteria

Met:

- **Given** the growth workload under a fixed cap, **when** it runs twice,
  **then** RSS-per-row agrees inside tolerance and the slope is recorded per
  shape. ✅ inside 2%; `perf-targets.md` §5.
- **Given** the swap-off leg, **when** the cap is exceeded, **then** the exit is
  identified and recorded. ✅ **SIGKILL, signal 9** — not the catchable trap this
  criterion originally expected, which is the whole point of measuring. The
  "process keeps serving" half of the original wording is **void**: nothing
  survives a SIGKILL.
- **Given** a cap exceeded with `WO_DATA` set, **when** the process is killed at
  exhaustion, **then** replay shows the acked writes present. ✅ ~40 000 rows,
  contiguous, no holes, no corruption report. Gated as the `ceiling` leg.
- **Given** the swap-on leg, **when** the same point is reached, **then** the
  degradation is quantified **and the absence of any error signal recorded**.
  ✅ degradation is **nil** for this workload (148 s vs 150 s uncapped) and the
  silence is total. Both halves are findings; the first inverted the prediction.
- **Given** the extended baseline, **when** a growth metric is doctored, **then**
  the gate fails on exactly that metric. ✅ text footprint +20% →
  `FAIL gate.growth.text.noswap.bytes_per_row -- 388 vs baseline 324`, 1 of 104.
- **Given** a host without the cap mechanism, **when** the harness runs, **then**
  the legs are skipped with a named reason and the rest still passes. ✅
  `cap_wrapper` returns None unless the `memory` controller is delegated; there
  is no uncapped fallback.
- **Given** a table larger than the cap, **when** it is read randomly, **then**
  the degradation is quantified. ✅ **273× throughput, ~480× p99**, both legs
  reading the same key order with all reads resolving. This closes the gap the
  swap leg left, and it is the pattern `resident: keys` creates.
- **Given** a store with history, **when** it boots, **then** replay cost is
  recorded so iteration 3 has a before. ✅ **≈5.5 µs/record**, flat across
  shapes, and **1.9× boot cost for an identical dataset** once each row has been
  updated once. Startup subtracted via an empty store.

Outstanding:

- **The resident-footprint fraction for iteration 2's budget default. NOT
  delivered, and the premise is false** — a finding, not a gap. It was to be derived from the
  swap-onset point — but there is no onset: swap-off jumps straight from
  working to SIGKILL, and swap-on shows no degradation to detect an onset in.
  **Iteration 2 must pick its budget on other grounds** (host RAM fraction, or
  an explicit developer-declared figure) rather than waiting on a number this
  iteration cannot produce. This is the most important thing this slice learned
  and it removes a dependency rather than satisfying it.
- **Given** rising fractions of the cap, **when** latency is sampled, **then**
  the p99 departure point is recorded. Partially, and now with a real answer
  elsewhere: `p99_departure_decile` stays 0 because the footprint legs never
  approach their 512 MiB cap, but the departure itself is measured by the
  `randread` leg as a **step, not a curve** — 1 µs resident, 487 µs over-cap.
  There is no gentle departure to find; residency is close to binary.
- Nothing else. Both remaining gaps closed 2026-08-27.

## Out Of Scope

- **Any fix.** This measures. Declared budgets are [2](02-table-storage-modes.md),
  eviction is [5](05-bounded-tables-eviction.md), tiering is 2's `resident: keys`.
- **Changing the OOM behaviour.** The checked-`malloc` code is untouched. The
  measurement showed it is largely unreachable for table storage under default
  overcommit — a finding to hand to [2](02-table-storage-modes.md), not a bug to
  fix here, and emphatically not a licence to start setting
  `vm.overcommit_memory`.
- **A memory profiler or allocator instrumentation** — observability is language
  iteration 30. RSS from `/proc` plus `time.ticks` is enough for a curve.
- **Comparing against SQLite at the ceiling.** `bench/compare/go-sqlite` exists,
  but SQLite's paged architecture is the design this project rejected, so the
  numbers would inform no decision here.
- **Multi-host scaling** — one binary owns its data.

## Info — the forks, settled

1. **The limit mechanism is rootless cgroup v2** via
   `systemd-run --user --scope -p MemoryMax -p MemorySwapMax`. `ulimit -v` was
   rejected: it bounds address space, not resident set, and ASan's virtual
   reservations trip it long before real pressure. No sudo needed; it also
   isolates the run from the rest of the box, which mattered — the dev box sat
   at 22.9 of 31.7 GiB throughout.
2. **Both reference shapes, reported separately.** 3.3× apart; one number would
   be a fiction.
3. **Swap is a dimension, not a footnote** — settled by getting it wrong first.
   An early run looked like the cap was unenforced because the process held
   400 MiB inside a 64 MiB limit; it was swapping, which is the phenomenon under
   study.
4. **Footprint is read as the median of per-decile marginals**, not a two-point
   slope, so a slab doubling does not smear into the per-row figure. Doublings
   are counted as their own metric.
5. **Kill-timing metrics carry ±100% tolerance.** `rows_recovered` depends on
   where the SIGKILL landed; gating it tightly would be gating the scheduler.
   The invariant asserted instead is the *shape* of the survivor.
6. **`randread` gates the RATIO, not the absolutes.** The over-cap half is swap
   I/O, so its reads/sec belongs to the box; the collapse factor between two
   runs that differ only in their cap belongs to the engine. Both legs read the
   same Weyl key order (`i*2654435761 mod n` — no RNG in the language, and none
   needed) so residency is the only variable.
7. **The ceiling leg asserts `rc`, never records it.** When iteration 2's byte
   budget lands, death should become a checked refusal — the gate must not fail
   on that improvement.

## History — four corrections worth keeping

**"An order of magnitude" was a guess.** The per-shape difference is 3.3×. An
iteration whose purpose is replacing unmeasured claims had one in its own
premise.

**The clean-exit premise was wrong.** This file and the residency spec both
asserted the ceiling surfaces as a catchable `WO_T_OOM`. It is a SIGKILL.
Overcommit means the allocator never learns there is a problem.

**The latency-collapse premise was wrong too.** Swap cost ~1% on an
append-mostly workload (148 s vs 150 s). The prediction was not merely
imprecise, it had the wrong sign. The narrower claim that survives is that a
*random-read* workload over an oversized table is the one at risk, and that
remains unmeasured.

**A SIGKILL was once labelled a "checked refusal"** by the harness, because
`subprocess` reports signal death as a negative `returncode` (`-9`) while the
shell spells the same event `137`. The leg existed specifically to tell those
two apart. Fixed, and the distinction is now spelled out at the comparison.
