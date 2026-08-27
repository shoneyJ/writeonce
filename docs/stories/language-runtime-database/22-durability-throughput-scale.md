---
iteration: "22"
status: done
chain: 2
---

# Iteration 22 — durability proof, throughput, and scale under load

> Format: fiberloom `product/story-iteration-template`. Part of
> [Story — one language, one runtime, one database, one binary](00-story.md).
>
> **Inserted 2026-08-15.** The measurement backbone. Everything after the
> functional engine (9/9b) is an *optimization*, and an optimization without
> a number is a guess — this iteration is the number. It comes before the
> optimization iterations (7b GC, 8 shard-actor, 23 io_uring) reopen for
> performance work, because each of those must be gated by re-running THIS
> iteration's benchmark and showing the number moved the right way.
>
> **✅ LANDED 2026-08-21** — the measurement backbone exists and has
> run: `docs/examples/db-bench` (seed/read/query/write/mix/msgrate/
> wal/verify, per-op `time.ticks` µs timing, 1µs-histogram percentiles),
> `scripts/db-bench.py` (campaign driver + gates), `bench/baseline.json`
> (74 metrics, the first contract — tolerances tuned by a two-run
> repeatability check: mix* 50%, read/query 35%, rest 15%), `just
> db-bench` / `db-bench-quick`. Proofs: restart-persistence + 3× kill -9
> battery at BOTH shard counts, all acked rows present every time; the
> gate BITES (doctored results fail on exactly the doctored metric).
> Headline findings: durable seed ≈4.5k/s vs ram ≈297k/s (iteration
> 23's case, measured); point lookups are O(table) — reads ≈1.5k/s at
> p50 ≈600µs on 20k rows (the probe walks every slab); mixread 1,280
> ops/s single- vs 21 ops/s multi-shard (the arc's honest price);
> msgrate 13.4M same-heap vs 2.45M cross-shard (deviation 4's
> mutex-inbox number). The arc's delta table lives in story 8.
> Deviations, disclosed in the plan: histogram not reservoir; `all` +
> `wal` modes added (RAM store dies with the process; clean acked-line
> crash vehicle); one python driver; `time.ticks` routed via an explicit
> dispatch arm. Standing finding: hand-built `multi <TableClass>` SEGVs
> on drop (elements classed OWNED, refs are scalar ids) — own slice.
>
> **SPEC APPROVED 2026-08-21** — the four forks below are SETTLED as
> their recorded leanings (developer confirmation), plus two new
> decisions: the vehicle is a NEW sample `docs/examples/db-bench`
> (employee stays a teaching sample) and `time.ticks` (CLOCK_MONOTONIC
> µs) is the iteration's one runtime addition. Spec:
> [`2026-08-21-db-bench-design.md`](../../superpowers/specs/2026-08-21-db-bench-design.md)
> · plan: [`2026-08-21-db-bench.md`](../../superpowers/plans/2026-08-21-db-bench.md)
> — **in progress** (second slice of the chain).
>
> **RE-SEQUENCED 2026-08-21** (developer decision): runs AFTER the arc's
> stage 3 — the transparent DB actor is a correctness hole (a multi-shard
> program touching the database traps `WO_T_DB` today), and fixing it
> first lets ONE benchmark campaign cover single- and multi-shard
> honestly. The arc's stages 1+2 landed 2026-08-20 unmeasured; their
> delta is recorded retroactively against this iteration's first
> baseline. New measurement target since stage 2: the mutex-guarded
> inbox + eventfd (the plan's deviation — lock-free rings arrive only if
> this number says the mutex costs). Chain order:
> **stage 3 → 22 → 31 → 24 → 23 → 32**.

## Goals

- **Durability is proven by a restart, not asserted.** The employee program
  (iteration 9b) runs, writes rows, is stopped and restarted, and every
  acknowledged write is present after replay — the WAL's promise turned into
  a scripted acceptance on a real program, not just the unit-level crash
  battery.
- **Read and write throughput are measured, published, and defended.** A
  repeatable benchmark drives the engine through the language (not the C
  API): inserts/sec, point-reads/sec, indexed-query/sec, each with p50/p99
  latency, recorded in the tree so a regression is a diff.
- **The scale target is a gate, not a slogan.** "A million users can read and
  write" becomes a concrete load: a dataset of ~1M rows across the sample's
  tables, a mixed read/write workload at a stated concurrency, sustained for
  a stated duration, with throughput and tail latency inside a stated budget
  and RSS flat (the log-watcher soak discipline, at database scale).
- **The benchmark is the contract every later optimization signs.** 7b (GC),
  8 (shard-actor threads), and 23 (io_uring) each re-run this and record the
  before/after — no optimization lands without a measured delta.

## Acceptance Criteria

- What to achieve?
    - **Given** the employee program seeded with data and then stopped,
    - **when** it is restarted and queried,
    - **then** every acknowledged row is present with its exact contents,
      the ids continue past the persisted maximum, and a query that used an
      index before the restart uses it after (the index was rebuilt on
      replay).
- What to achieve?
    - **Given** the benchmark harness driving inserts, point reads, and
      indexed queries through compiled `.wo`,
    - **when** it runs to completion,
    - **then** it reports ops/sec and p50/p99 for each operation class, writes
      the numbers to a tracked results file, and fails if any number crosses
      a recorded regression threshold.
- What to achieve?
    - **Given** ~1M rows and a mixed read/write workload at the target
      concurrency held for the target duration,
    - **when** it runs,
    - **then** throughput stays above the floor, p99 stays under the ceiling,
      RSS is flat between a warmed baseline and the end (no growth beyond
      tolerance), zero descriptors leak, and — for a write-inclusive run under
      a durable configuration — a kill mid-load followed by replay loses no
      acknowledged write.
- What to achieve?
    - **Given** any later optimization iteration (7b, 8, 23),
    - **when** it claims a speedup,
    - **then** this benchmark's before/after numbers are in that iteration's
      record, and a claim with no measured delta is not accepted.

## Out Of Scope

- **The optimizations themselves.** This iteration MEASURES; 7b/8/23 change.
  A single-thread RAM-authoritative baseline is a legitimate first number —
  the point is to have one before anyone tunes.
- **Distributed / multi-machine load.** Same-machine, one process (or one
  process per shard once iteration 8 lands). Cross-host is the network layer's
  concern, much later.
- **Micro-optimizing the benchmark harness.** It must be honest and
  repeatable, not itself fast; if the harness is the bottleneck the spec says
  so and fixes that, but a perfect load generator is not the deliverable.
- **A cost-based query planner.** Index selection is 9b's; this iteration
  measures what 9b lowers, it does not make the planner smarter.

## Info

Forks the spec must settle:

**1. What generates the load, and in what language?** The doctrine is "the
sample is the test", so the honest generator drives compiled `.wo` — a
benchmark mode in the employee program (or a sibling sample) that loops
inserts/reads/queries and times them. The alternative — a C harness calling
the engine API directly — measures the engine but skips the compiler's
lowering, which is exactly the layer a language-integrated query has to pay
for. Leaning: `.wo` benchmark mode for the headline numbers (the number that
matters is end to end), with the C-API microbench kept only to attribute a
regression to engine vs lowering.

**2. What are the actual budgets?** Throughput floors and latency ceilings
have to be numbers, and the first run sets them — but the spec must decide
whether the gate is absolute (">= N ops/sec on the reference machine") or
relative ("no worse than the last recorded run by more than X%"). Absolute
gates rot across machines; relative gates need a committed baseline file.
Leaning: relative gates against a tracked `bench/baseline.json`, refreshed
deliberately with a commit that says why, plus a loud absolute floor so a
catastrophic regression fails even on a slow machine.

**3. What does "1M users read and write" concretely mean?** A million
long-lived idle connections is a different test from a million rows under a
churning read/write mix from a bounded connection pool. The sample's shape
(departments, employees) suggests rows, not connections, as the scale axis
for THIS iteration; the connection-scale test belongs with the shard-actor
runtime (iteration 8) and the eventual network layer. Leaning: ~1M rows +
a bounded concurrent read/write workload here; connection scale deferred to
8 with a cross-reference.

**4. Durable or RAM-only for the throughput headline?** fsync-per-commit
(the current per-statement durability) will dominate write throughput and is
the honest number for a durable workload; RAM-only (no `WO_DATA`) measures
the engine's ceiling. Both matter and mean different things. Leaning:
publish both, labeled — durable is the number an operator plans against, and
the gap between them is precisely what iteration 23 (io_uring group-commit)
exists to close.

## Proposed Solution

- **Brainstorm the spec**, settling the four forks; then a plan whose first
  task is the harness and the baseline file, because nothing downstream means
  anything without them.
- **Sequence the performance chain around this iteration** (rewritten
  2026-08-21 — the first version predated 7b and the arc landing first):
  1. Already landed unmeasured: 9b, **7b** (inferred GC + mark-sweep,
     2026-08-18), the arc's **stages 1+2** (fibers + shards, 2026-08-20).
     Their deltas are owed retroactively against the first baseline.
  2. Arc **stage 3** (transparent DB actor) lands → **22 runs**:
     restart-persistence proof + baseline benchmark, durable and
     RAM-only, single- AND multi-shard, plus the mutex-inbox number.
  3. **31** (actor lifecycle), then **24** (chat) → re-run the
     concurrency-facing numbers at the connection scale chat unlocks.
  4. **23** (io_uring group-commit) → re-run 22's durable write number,
     record the delta against the fsync-per-commit baseline — the payoff.
- The benchmark harness and its baseline live under `bench/` (or the existing
  `runtime/bench/`), and `just` gets a `db-bench` recipe kept off the fast
  path, exactly like `log-watcher::soak`.
