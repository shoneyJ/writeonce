---
track: databasev2
iteration: "1"
status: pending
readiness: refine
---

# databasev2 1 — the RAM ceiling: measure the breaking point before designing for it

> Part of [Story — databasev2: the database beyond RAM](00-story.md).
>
> **First because the repo's own doctrine says so.** "Always inspect crashsites.
> Always measure. Never assume." Every later iteration in this track — the
> storage modes' defaults, the eviction policy, the tiering threshold — is a
> decision that should follow from a number. Right now nobody in this project
> can say what happens to a writeonce program at 90% of RAM, and designing
> tiering without that is guessing with extra steps.

## Goals

- **Find the curve, not the cliff.** Not "does it die" — it dies, everything
  does. What matters is the shape on the way down: at what fraction of RAM does
  p99 read latency leave its 1µs baseline, what does insert throughput do as
  slabs stop coming from a warm allocator, and how much warning is there between
  "fine" and "unusable".
- **Characterise all three exits.** The engine can leave the happy path three
  ways and they are not equally survivable: a checked `malloc` failure
  (`DB_ERR_OOM` → `WO_T_OOM`, a catchable trap — the clean one), swap thrash
  (no trap, no error, just latency collapse — the dangerous one because nothing
  reports it), and the external OOM killer (`SIGKILL`, skipping every shutdown
  path). Establish which arrives first under realistic limits, because the
  answer determines whether the fix is back-pressure or eviction.
- **Prove the durability floor holds at the ceiling.** Iteration 22's `kill -9`
  battery proved acked writes survive under load. Re-run it *at memory
  exhaustion*, which is a different and nastier state — an allocation failure
  mid-commit is exactly where an ack-before-durable bug would hide.
- **Publish numbers others can build on.** The output is a section in
  `perf-targets.md` and rows in `bench/baseline.json`, not a paragraph of
  prose. A measurement that only printed once is not a measurement.

## Phases

### Phase A — a workload that can actually reach the ceiling

- Extend `docs/examples/db-bench` with a growth mode: insert until a target RSS
  fraction, holding row shape and index count constant so the variable is size
  alone.
- Run it under an explicit memory limit (a cgroup or `ulimit`) rather than on a
  big box — "it survived on a 64 GB workstation" measures the workstation.
- Record RSS against row count so the per-row overhead is known: slab headroom,
  the id hash, the secondary-index multimaps and the per-row engine-owned values
  (`db_text`, `db_rec`, `db_multi`, `db_map` are each their own allocation).
- Verify: RSS growth is linear and its slope is written down; the run is
  reproducible twice within the tolerance policy iteration 22 established.

### Phase B — the latency and throughput curve

- Sample read p50/p99, query p99 and insert throughput at fixed fractions of the
  limit, so the result is a curve rather than two endpoints.
- Separate the two effects deliberately: allocator pressure (still resident) and
  swap (no longer resident). They have different fixes and conflating them would
  send iteration 6 after the wrong one.
- Include the DB-actor path, since a cross-shard statement's reply materialises
  a copy — memory pressure and the actor RPC interact and nobody has looked.
- Verify: the curve is recorded per metric class with iteration 22's per-class
  tolerances; the swap onset point is identified, not interpolated.

### Phase C — the three exits, deliberately triggered

- Drive a checked allocation failure and confirm `WO_T_OOM` is catchable, the
  insert is refused whole, no partial row or index entry is left, and the
  process continues serving.
- Drive swap thrash and record what a client sees. This is the case with no
  error signal at all, and naming it is most of the value of this iteration.
- Drive the OOM killer under a cgroup limit and confirm what survives: replay
  the WAL and check every acked write is present.
- Verify: the trap path leaves no torn state (row count and index agree after a
  refused insert); replay after `SIGKILL` at exhaustion loses no acked write.

### Phase D — write it down where decisions get made

- A `perf-targets.md` section with the curve, the swap onset, the per-row
  overhead and the exit characterisation.
- Baseline rows for the growth metrics so a regression is caught by the existing
  gate rather than by a person remembering.
- A short statement of what the numbers *imply* for iterations 2, 5 and 6 —
  which is the point of going first.
- Verify: `just db-bench` green against the extended baseline; the gate bites
  when a growth metric is doctored.

## Acceptance Criteria

- **Given** the growth workload under a fixed memory limit, **when** it runs
  twice, **then** RSS-per-row agrees within the tolerance policy and the slope
  is recorded in `perf-targets.md`.
- **Given** the workload at rising RAM fractions, **when** latency is sampled,
  **then** the fraction at which read p99 first leaves its baseline is
  identified as a measured point, not an estimate.
- **Given** a deliberately induced allocation failure, **when** an insert is
  attempted, **then** it traps `WO_T_OOM` catchably, the table's row count is
  unchanged, every index agrees with the slab contents, and the process keeps
  serving subsequent requests.
- **Given** swap thrash, **when** a client issues reads, **then** the observed
  degradation is quantified and the fact that **no error is surfaced** is
  recorded explicitly as a finding.
- **Given** a cgroup limit and a workload that exceeds it, **when** the OOM
  killer fires, **then** replaying the WAL shows every acked write present —
  ack-after-fsync holding in the one shutdown path that skips all cleanup.
- **Given** the extended baseline, **when** a growth metric is doctored, **then**
  `just db-bench` fails on exactly that metric.

## Out Of Scope

- **Any fix.** This iteration measures. Eviction is
  [5](05-bounded-tables-eviction.md), tiering is [6](06-cold-tiering.md),
  declared budgets are [2](02-table-storage-modes.md). Shipping a fix inside the
  measurement slice would remove the ability to tell whether it helped.
- **Changing the OOM behaviour.** The checked-`malloc`-to-catchable-trap path is
  good and should not be touched; if the measurement finds a hole in it, that is
  a bug fix, reported separately.
- **A memory profiler or allocator instrumentation.** Observability is language
  iteration 30. RSS from the OS and the existing `time.ticks` are enough for a
  curve.
- **Multi-machine or sharded-across-hosts scaling.** One binary owns its data;
  cross-process is [9](09-cross-program-tables.md).
- **Comparing against SQLite at the ceiling.** `bench/compare/go-sqlite` exists
  and the comparison would be interesting, but SQLite's whole architecture is
  the paged design this project rejected — the numbers would not inform any
  decision here.

## Info

Forks the spec must settle:

1. **What is the limit mechanism for the harness?** A cgroup v2 `memory.max` is
   the closest thing to how this would actually be deployed; `ulimit -v` is
   simpler but bounds address space rather than resident set, which for an engine
   that `malloc`s slabs is a materially different constraint. Leaning cgroup, and
   the campaign already runs off the fast path so the setup cost is acceptable.
2. **Which table shape is the reference?** Per-row overhead depends heavily on
   whether fields are scalars or heap values — a `Text` column is a separate
   `db_text` allocation per row, so a text-heavy table and an Int-only table
   will produce very different slopes. Probably both, reported separately,
   because "bytes per row" is meaningless without saying which row.
3. **Is swap even in scope for the target deployment?** If the intended answer
   is "run with swap off and let the OOM killer decide", the swap curve is
   informational rather than load-bearing — but that stance should be stated in
   the doctrine, not assumed. It also changes which exit iteration 5's
   back-pressure is defending against.
