---
track: databasev2
iteration: "5"
status: refine
---

# databasev2 5 — bounded tables and eviction: a declared budget, and back-pressure before the cliff

> Part of [Story — databasev2: the database beyond RAM](00-story.md).
> Needs [2](02-table-storage-modes.md) for the mode a bound attaches to, and
> [1](01-ram-ceiling-measurement.md) for the numbers that set a sane default.
>
> **The simpler half of the hard problem, done first on purpose.** Evicting from
> a bounded resident table and evicting to disk are the same policy question with
> different destinations. Getting the policy right where the answer is "drop it"
> de-risks [6](06-cold-tiering.md), where the answer is "write it somewhere and
> be able to find it again".

## Goals

- **A table may declare a maximum.** Rows, bytes, or both — a `ram` table that
  is a cache or a session store has a size the application is willing to spend,
  and today it has no way to say so. Unbounded growth in a table nobody intended
  to be large is the most common route to the ceiling iteration 1 measured.
- **Something defined happens at the bound.** Today the answer is "grow until
  the process dies". The candidates are eviction (drop the least valuable row),
  refusal (trap, let the caller decide), and back-pressure (make the writer
  wait). Each is right for a different table, which argues for the policy being
  declared rather than chosen for the developer.
- **Back-pressure before the cliff, not at it.** The dangerous exit iteration 1
  characterises is swap thrash, which arrives with **no error signal at all**.
  A budget that is enforced at 100% has already lost; the value is in acting at
  a threshold, while there is still headroom to act.
- **Eviction that respects the engine's actual invariants.** Rows have stable
  addresses forever, the free-slot list recycles slots, ids are never reused, and
  every secondary index and unique shadow must stay consistent with the slab.
  Eviction is `wo_row_remove` with a policy in front — it must go through the
  same choke point, not around it.

## Phases

### Phase A — declaring the bound

- Extend the `@table` surface from [2](02-table-storage-modes.md) with a
  capacity and a policy. One new grammar arm, the same catalogued-diagnostic
  discipline, defaults that keep every existing table unbounded so nothing
  changes silently.
- Decide whether a bound is legal on a `durable` table (fork 1) — evicting a row
  that was acked as durable is a promise being broken, and the answer is
  probably "only with an explicit, differently-named policy".
- Verify: golden fixtures per policy; existing tables unchanged; illegal
  combinations refused at compile time with catalogued codes.

### Phase B — the accounting

- Track per-table size cheaply. Row count is free; bytes are not — per-row
  footprint includes the slab slot plus each heap value's own allocation
  (`db_text`, `db_rec`, `db_multi`, `db_map`), which iteration 1 will have
  quantified. Decide what is counted and be honest that it is an estimate of RSS,
  not RSS.
- Expose it, because a budget nobody can observe is a budget nobody can tune.
  How it is exposed is fork 3.
- Verify: accounting tracks a known workload within a stated error bound;
  deleting rows returns the accounting to its prior value (the free-slot list
  already makes this true of slots).

### Phase C — the policies

- **Refuse**: the bound is a hard ceiling, an insert past it traps catchably.
  The simplest correct behaviour and the right default for anything precious.
- **Evict**: drop the least valuable row via `wo_row_remove` so indexes, unique
  shadows and the free-slot list all stay honest. The recency metadata this needs
  is fork 2 — and note the engine currently stores no per-row access time, so
  true LRU is not free.
- **Back-pressure**: at a threshold below the bound, slow or park the writer.
  This composes with the fiber model (a parked writer blocks nobody) and is the
  only policy that addresses the swap-thrash exit rather than the allocation
  exit.
- Verify: each policy behaves at the bound; after eviction every index agrees
  with the slab; a parked writer resumes and does not deadlock the shard.

### Phase D — the pressure signal

- A process-level threshold, not just per-table: when total resident size
  crosses a configured fraction, tables with an eviction policy start shedding
  **before** the allocator or the OS gets involved. This is the iteration's real
  contribution — turning an invisible failure into a managed one.
- Decide precedence when several tables could shed (fork 4).
- Verify: under the iteration-1 growth workload with the signal enabled, the
  process holds a steady state instead of walking into swap; the latency curve
  stays inside its baseline.

### Phase E — gate it against the measurement

- Re-run iteration 1's growth workload with bounds and policies configured. The
  proof is a before/after on the same harness: previously the curve degraded and
  the process died; now it plateaus.
- Baseline rows for steady-state throughput under pressure.
- Verify: `just employee`, `just db-actor`, `just db-bench` green; the gate bites
  on a doctored pressure metric.

## Acceptance Criteria

- **Given** a table bounded at N rows with policy `refuse`, **when** the N+1st
  insert is attempted, **then** it traps catchably, the row count stays N, and
  every index agrees with the slab.
- **Given** a table bounded at N rows with policy `evict`, **when** the N+1st
  insert arrives, **then** exactly one row is evicted, the new row is present,
  the count is N, and no index or unique shadow references the evicted row.
- **Given** an evicted row's id, **when** it is looked up, **then** it is absent
  — and its id is never reused by a later insert, preserving the invariant the
  id hash's tombstone sentinel depends on.
- **Given** a bound expressed in bytes, **when** rows of a known shape are
  inserted, **then** the bound is honoured within the stated accounting error,
  and that error is documented rather than implied.
- **Given** back-pressure configured at a threshold, **when** the threshold is
  crossed, **then** writers are slowed or parked, reads are unaffected, and no
  shard deadlocks.
- **Given** the process-level pressure signal and the iteration-1 growth
  workload, **when** it runs to what previously exhausted memory, **then** the
  process reaches a steady state and read p99 stays within its baseline — the
  before/after that justifies the iteration.
- **Given** a `durable` table, **when** an eviction policy is applied to it,
  **then** either it is refused at compile time or it is a distinctly named
  policy that says out loud it discards acked data.

## Out Of Scope

- **Writing evicted rows anywhere** — that is [6](06-cold-tiering.md). Here
  eviction means the row is gone. Keeping the two apart is what makes the policy
  work reviewable on its own.
- **True LRU if it costs a write per read.** Touching per-row metadata on every
  read would turn the 1µs read path into a write path — the same trap iteration
  3's session touch has. An approximation (insertion order, a coarse clock, a
  sampled counter) is very likely the right answer and fork 2 should say so
  explicitly rather than defaulting to textbook LRU.
- **The TTL cache middleware** — language
  [iteration 18](../language-runtime-database/18-memory-db-features.md). Expiry
  by *time* is that; bounding by *size* is this. They compose.
- **Query-level result limits.** `take n` already exists in the query surface.
- **Shrinking slabs back to the allocator.** Slab addresses are stable forever
  by design and that invariant is load-bearing; reclaiming a slab whose rows were
  all evicted is a separate, delicate change with its own iteration if anyone
  wants it.

## Info

Forks the spec must settle:

1. **May a `durable` table be bounded?** Evicting an acked row contradicts the
   durability promise. But an audit table that must not grow forever is a real
   need, and the honest form of it is probably archival (iteration 6) rather than
   eviction. Leaning: bounds on `durable` are refused, and the need is redirected
   to 6.
2. **What is "least valuable"?** No per-row access time exists today, so LRU
   costs a write per read. Candidates: insertion order (free — ids are already
   monotonic per table), a coarse epoch stamped on write only, or sampled
   approximation. Insertion order is FIFO not LRU, which is wrong for a cache
   and fine for a queue — so the policy name should say which it is rather than
   claiming "LRU" and delivering FIFO.
3. **How is size observed?** Without observability (language iteration 30) there
   is no metrics endpoint to publish it on. Options: a builtin returning a
   table's current size, a `@table`-derived query, or stderr on threshold
   crossing. A builtin is the smallest thing that makes the feature tunable by
   the program that owns the budget.
4. **Precedence when several tables can shed.** Largest first is simple; the
   application's own priority order is more correct and needs a way to express
   it. Proportional shedding is fairest and hardest to reason about. This
   decides whether the pressure signal is predictable enough to trust.
