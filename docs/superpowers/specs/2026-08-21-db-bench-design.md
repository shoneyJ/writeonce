# Iteration 22 — durability proof, throughput, and scale under load (design)

**Date:** 2026-08-21
**Status:** proposed (story iteration 22) — awaiting developer review; the
plan follows approval. Board: [docs/00-status.md](../../stories/00-status.md)
**Scope:** the measurement backbone — a benchmark workload in `.wo`, a
campaign driver script, a tracked baseline contract, and the durability
proofs (restart persistence + crash battery), single- AND multi-shard.
**Relates to:** [story 22](../../stories/language-runtime-database/refine/22-durability-throughput-scale.md)
(the four forks settled below), the landed arc
([story 8's guarantee contract](../../stories/language-runtime-database/done/08-shard-actor-runtime.md)
— the stage-3 delta this iteration records), iteration 23 (the durable
write number it exists to beat), iteration 32 (the aged-store replay
number its policy wants), stage-2 deviation 4 (the mutex-inbox number).

## Decisions locked during brainstorming (2026-08-21)

The story's four forks are SETTLED as their recorded leanings (developer
confirmation, the iteration-23 precedent):

1. **The load generator is compiled `.wo`.** The headline numbers cross
   the whole stack — lexer to WAL — because that is the layer a
   language-integrated query pays. A C-API microbench exists ONLY to
   attribute a regression to engine vs lowering; it is never the quoted
   number.
2. **Gates are relative, against a committed baseline.** Fail when a
   metric is worse than `bench/baseline.json` by more than its recorded
   tolerance (default 15%, tunable per metric in the file). One loud
   ABSOLUTE floor per class — a deliberately low catastrophic-regression
   tripwire that fails even on a slow machine. The baseline refreshes
   only by a commit that says why.
3. **The scale axis is rows, not connections.** "A million users read
   and write" means ~1M rows seeded, a bounded-concurrency 90/10
   read/write mix sustained for a stated duration, throughput above the
   floor, p99 under the ceiling, RSS flat. Connection scale belongs to
   iteration 24's serving workload.
4. **Both durability flavors run and publish, labeled.** `ram` (no
   `WO_DATA`) is the engine's ceiling; `durable` (fsync-per-commit) is
   the number an operator plans against. The gap between them is
   exactly what iteration 23 exists to close — this iteration prices it.

New decisions (post-arc reality):

5. **The vehicle is a NEW sample, `docs/examples/db-bench`** — the
   employee sample stays a teaching sample; the multi-shard campaign
   needs actor writers, which employee is deliberately not.
6. **One runtime addition, disclosed: `time.ticks`** — CLOCK_MONOTONIC
   microseconds as an Int. `time.now` is wall-clock milliseconds and
   cannot rank microsecond-scale operations; honest per-op p50/p99 from
   inside `.wo` needs this clock. It joins the stdlib contract doc like
   every builtin; no other runtime change is in scope.

## 1. Shape — four artifacts

- **`docs/examples/db-bench`** — the load generator, pure `.wo`,
  mode-dispatched on argv exactly as the employee sample is.
- **`scripts/db-bench.sh`** — the campaign driver: environment setup
  (`WO_SHARDS`, `WO_DATA`, `WO_IO`), the kill -9 battery, RSS/fd
  sampling (the `LW_SOAK` discipline: resident growth beyond 256 KiB or
  any descriptor growth fails), result collection into JSON, and the
  relative/absolute gate evaluation.
- **`bench/baseline.json`** — the tracked contract: per metric, the
  baseline value, its tolerance, and the absolute floor. The first
  honest run writes it; every later run is judged against it.
- **`just db-bench`** — runs the campaign, off the fast path (like
  `log-watcher::soak`); `just db-bench-quick` runs a seconds-long smoke
  of the same modes for CI-shaped sanity, gated loosely.

## 2. The workload — db-bench's modes

Two related tables in the employee shape (a parent with a `@unique`
text column, a child with `ref` parent + two indexed columns) so reads,
indexed probes, FK checks, and unique maintenance are all priced.

Modes, each printing one machine-parsable line per operation class —
`<op> <count> <ops/sec> <p50us> <p99us>`:

- **`seed N`** — N child rows (parents amortized), timed inserts.
- **`read N`** — N point-reads by id over the seeded store.
- **`query N`** — N indexed probes (the `where` path).
- **`write N`** — N mixed inserts + field updates.
- **`mix N C`** — the sustained load: 90/10 read/write from C
  concurrent actors for N total operations. Single-shard, actors are
  fibers on the primary; multi-shard, placement spreads them and every
  DB statement rides the stage-3 RPC.
- **`msgrate N`** — the mutex-inbox number: two actors on different
  shards ping-pong N messages; reports msgs/sec. This is the number
  stage-2's deviation 4 waits on before lock-free rings earn their
  complexity (single-shard run included for the same-heap comparison).
- **`verify`** — recount + content checksum + one indexed probe against
  expected values; exit nonzero on any mismatch. The restart and crash
  proofs are `seed`/`write` runs bracketing a `verify` across process
  boundaries.

Timing is per-operation via `time.ticks`, aggregated in `.wo`
(count, ops/sec, p50, p99 from a fixed-size reservoir — the harness
must stay honest, not clever; if the reservoir ever dominates cost the
spec's answer is a bigger batch, not a fancier sampler).

## 3. The campaign — what one `just db-bench` run produces

For each flavor `ram` and `durable`, for each shard count 1 and
default-cores: seed, read, query, write, mix — plus `msgrate` once per
shard count. Results land in one JSON file (per-run, timestamped, in
`bench/results/`, gitignored except the baseline), then the gate
compares against `bench/baseline.json` and fails on any tolerance
breach. The stdout tail is the standup summary: one line per metric
with the baseline delta.

The FIRST full run on the reference machine writes the baseline and
sets the scale-target numbers (fork 3's floor/ceiling become recorded
values, not prose). The arc's owed before/after is recorded the same
way: the single- vs multi-shard columns of the same table ARE the
delta, stated in the results and copied into story 8's record.

## 4. Durability — proven by a restart, not asserted

- **Restart persistence:** `seed` under `WO_DATA`, clean stop, restart,
  `verify` — counts, contents, id continuation past the persisted
  maximum, and an indexed probe (the index was rebuilt on replay). The
  employee gate's existing seed-twice check stays as the second witness.
- **Crash battery:** kill -9 mid-`write` under `WO_DATA`, restart,
  `verify` in acknowledged-writes mode: every operation the bench
  recorded as acknowledged (it prints a running high-water mark for
  exactly this) is present; no partial row is visible. K repetitions
  (K recorded in the baseline file), run at BOTH shard counts — the
  multi-shard rounds fire the stage-3 obligation under load: a kill
  between a worker's send and the owner's commit must leave no ack and
  no partial state.
- **Sustained-run hygiene:** during `mix`, the driver samples RSS and
  descriptor counts; growth beyond the LW_SOAK tolerances fails the
  campaign regardless of throughput.

## 5. The C-API microbench — attribution only

A `runtime/test`-shaped harness driving `wo_row_insert`/`wo_row_ptr`/
probe loops directly, printing the same line format. Run manually when
a headline regression needs blaming (engine vs lowering); never gated,
never quoted. Kept deliberately minimal — one file, no options beyond
counts.

## 6. Acceptance criteria

- **Given** a fresh checkout on the reference machine, **when**
  `just db-bench` runs to completion, **then** it produces the JSON
  results, evaluates every gate against `bench/baseline.json`, and
  exits 0 with the summary tail.
- **Given** the restart proof, **when** seed/stop/restart/verify runs
  under `WO_DATA`, **then** verify exits 0 (counts, contents, id
  continuation, index probe).
- **Given** the crash battery, **when** kill -9 lands mid-write K times
  at each shard count, **then** every acknowledged write is present
  after replay and no partial state is ever visible — zero tolerance.
- **Given** the multi-shard campaign, **when** the same modes run at
  `WO_SHARDS=1` and default cores, **then** both columns publish and
  the arc's delta is recorded in story 8; a >tolerance regression in
  the single-shard column against baseline fails the gate.
- **Given** a deliberate engine slowdown (a manual smoke, documented in
  the plan), **when** the gate runs against the committed baseline,
  **then** it FAILS — the contract must be shown to bite before the
  iteration closes.

## Out of scope

- The optimizations themselves (23's group commit, 32's checkpoint,
  ring inboxes) — this iteration prices, they change.
- Cross-host / distributed load; connection-count scale (iteration 24).
- Micro-optimizing the harness; a perfect load generator is not the
  deliverable — an honest, repeatable one is.
- A cost-based query planner; group-by (parked with 9b).
- Any runtime change beyond the `time.ticks` builtin.
