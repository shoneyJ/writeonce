---
track: databasev2
iteration: "5"
status: pending
readiness: ready
review_pending: "forks 1–12 settled 2026-09-10 by codd-shoney under autonomy, no developer in the loop — second review before code. Look first at the cuts: fork 4 (no back-pressure, no cross-table shedding, no warn threshold), fork 2 (the per-table bound is rows only), forks 3/7 (the default budget is the binding limit minus boot RSS — no fraction, no invented reserve)."
---

# databasev2 5 — bounded tables and eviction: a declared budget, and back-pressure at a declared threshold

> Part of [Story — databasev2: the database beyond RAM](00-story.md).
> Needs [2](02-table-storage-modes.md) for the mode a bound attaches to, and
> [1](01-ram-ceiling-measurement.md), whose numbers landed 2026-08-27 and
> **corrected this iteration's framing** — see the third goal.
>
> **The simpler half of the hard problem, done first on purpose.** Evicting from
> a bounded resident table and evicting to disk are the same policy question with
> different destinations. Getting the policy right where the answer is "drop it"
> de-risks [6](06-cold-tiering.md), where the answer is "write it somewhere and
> be able to find it again".
>
> **Amended 2026-09-09:** iteration 2's resident byte budget — its task 6b,
> the "budget that exists by default, breached loudly" of the residency spec —
> moved here as Phase A. It is an accounting-and-pressure question, so it
> belongs with the pressure signal it feeds rather than with a storage mode.
> The design inputs it arrived with were notes for this iteration's brainstorm,
> not decisions.
>
> **Brainstormed to `ready` 2026-09-10** by codd-shoney, from the code, the
> two measured iterations and the reference trees, without a prebuild brief.
> Twelve forks are settled under "Info" below; the title's "back-pressure"
> survives only as the word for what a refusal *is* — the parked-writer form
> was cut (fork 4). Every decision that cut a goal is marked in the goal it
> cut, and the frontmatter's `review_pending` names the ones a developer
> should look at first.

## The design, as settled

Two independent things, because the developer is answering two independent
questions — *how much RAM may this process spend on tables?* and *how big may
this one table get?* — and neither answer is derivable from the other
(iteration 1: 96.5 vs 320.6 B/row, so rows cannot bound RAM; and a byte figure
cannot tell a session cache how many sessions it holds).

| Question | Answer | Unit | Where declared | On breach |
| --- | --- | --- | --- | --- |
| RAM for all tables | one process-wide **budget**, compared against the engine's own estimate of what its tables hold resident | bytes | environment: `WO_DB_MB=<MiB>`; unset means the **default**, never "no budget" | **refuse**: the crossing insert traps `WO_T_OOM` (static message); one stderr line names the largest table, its rows and bytes, the budget and its source, and the annotation to add. During boot replay the same crossing is exit 2 with the same line |
| Capacity of one table | a declared **`max_rows`** | rows | `@table(max_rows: N)`, optionally `on_full: refuse \| drop_oldest` (default `refuse`) | `refuse` traps `WO_T_DB` "table is full"; `drop_oldest` removes the smallest live id through `wo_row_remove`, then inserts — legal only on `durable: false` |

The default budget is not a fraction. It is **the limit the kernel will
enforce, minus what the process already holds when it boots**: the smallest
cgroup v2 `memory.high`/`memory.max` set on the process's cgroup or any
ancestor, else `MemAvailable` from `/proc/meminfo` — minus `VmRSS` read from
`/proc/self/status` before replay. Both terms are read, none is a constant; a
developer who wants a guarantee on a shared host puts the process in a cgroup,
and the budget reads it (principle 2: the kernel is the framework). Whether
that leaves enough headroom is **measured, not assumed**: task A4 re-runs
iteration 1's 64 MiB swap-off leg with the default in force and requires the
refusal to precede the kill; if it does not, the measured gap becomes an
itemised reserve with that run as its citation (Info 3).

Nothing here evicts from a durable table, sheds one table to make room for
another, warns at a second threshold, parks a writer, or adds a builtin. Each
of those was an option and each is recorded with the reason it lost.

## Goals

- **A table may declare a maximum — in rows.** *(Decided 2026-09-10, Info 2:
  rows only; the earlier "rows, bytes, or both" is settled.)* A `durable:
  false` table that is a cache or a session store has a size the application
  is willing to spend, and today it has no way to say so. Unbounded growth in a
  table nobody intended to be large is the most common route to the ceiling
  iteration 1 measured. The unit is rows because that is the exact,
  deterministic quantity a developer reasons about for such a table; RAM is
  bounded by the process budget, in bytes, which names the table to fix.
- **Something defined happens at the bound.** Today the answer is "grow until
  the process dies". Two policies ship: refusal (trap, let the caller decide)
  and `drop_oldest` (FIFO by id, volatile tables only). *Back-pressure as a
  parked writer was cut (Info 4)* — a refusal that the program catches and
  retries after a wait **is** back-pressure under the fiber model, composed by
  the program with primitives it already has, and needs no wake condition the
  engine cannot define.
- **Refusal at a declared threshold — because there is no cliff to be
  before.** This goal was written expecting a gradient to detect. Iteration 1
  measured (2026-08-27) that no such gradient exists, which makes the goal
  *stronger*, not weaker:
  - Exceeding RAM **without** swap is **SIGKILL, signal 9** — no trap, no
    diagnostic. Table storage has no checked ceiling, and under
    `vm.overcommit_memory = 0` its `malloc` succeeds and the kernel kills on
    page touch, so the checked path never runs.
  - Exceeding RAM **with** swap returns **exit 0** and keeps serving from disk.
    An append-mostly workload pays **~1%** (148 s vs 150 s uncapped for 900k
    rows), so "swap thrash" — which this goal previously named as the dangerous
    exit — is not what happens on the write path at all.
  - Read latency does not *depart*, it **steps**: 1 µs resident to 487 µs
    over-cap with nothing in between.

  So there is no early-warning signal anywhere to react to — not an error, not a
  latency knee. A budget enforced at 100% has not merely "already lost"; it can
  never fire, because the process is dead or silently fine. **Only a declared
  threshold can speak, and it must be declared in bytes** — footprint is
  96.5–100 B/row Int-only against 320.6–324 B/row text-heavy, **3.3× apart**, so
  a row count cannot bound RAM. The index doublings iteration 1 saw at ~24k and
  ~48k rows are transient RSS steps; the estimate counts the new arrays the
  moment they are allocated, so the budget fires on the counted figure, and
  the headroom the transient needs is what task A4 measures.
- **The budget's quality is decisive, not incidental.** *(Rewritten 2026-09-10.)*
  Iteration 1 measured random reads over an oversized table at **273× slower**
  than resident (1 851 166 vs 6 771 reads/s; p99 1 µs vs 487 µs). That spread
  is the cost of *not* speaking in time — the process either dies or slides
  into that regime. What ships must be shown, on iteration 1's own harness, to
  refuse before either exit, not merely shown to count correctly. The eviction
  that ships (`drop_oldest`) is FIFO for volatile caches and says so in its
  name; it is not a claim about hot-set quality, which is a `resident: keys`
  question and stays out of this iteration.
- **Eviction that respects the engine's actual invariants.** Rows have stable
  addresses forever, the free-slot list recycles slots, ids are never reused, and
  every secondary index and unique shadow must stay consistent with the slab.
  Eviction is `wo_row_remove` with a policy in front — it must go through the
  same choke point, not around it.

## Phases

### Phase A — the resident byte estimate and the process budget (moved from 2's task 6b, 2026-09-09; settled 2026-09-10)

- **A1 — the estimate.** Count what each table holds resident, in bytes, at
  the allocation sites inside the choke points — `wo_row_insert`,
  `wo_row_insert_slots`, `wo_row_update_field` and its `_slot` form,
  `wo_row_remove`, `wo_row_drop_payload`, `wo_row_create_raw` /
  `wo_row_raw_commit` — so the figure cannot drift from storage. What is
  counted and how is Info 6: whole slabs, the bitmap, the free list, the id
  map, index bucket arrays and id lists, the scratch buffer, and every
  engine-owned value by its requested size rounded to the allocator's chunk.
  Never the VM arena, never the WAL. It is an estimate of RSS, not RSS, and
  the measured error bound is printed beside the figure rather than implied.
- **A2 — the knob and the refusal.** `WO_DB_MB=<MiB>` sets the budget
  (Info 5); the sum over tables is compared after a row lands and, over
  budget, the row is un-applied exactly as a unique violation is today
  (`table.c:662-675`, and `:714-725` for the slots form) and the insert
  traps `WO_T_OOM`. One stderr line per
  offending table per process names the table, its rows and bytes, the budget
  and its source, and the annotation to add (Info 7). The same crossing
  during boot replay refuses with exit 2 and the same line. One exit line
  under `WO_WAL_STATS=1` reports the budget, its source, the peak estimate
  and each table's rows and bytes.
- **A3 — the default.** With `WO_DB_MB` unset, the budget is the binding
  limit minus boot RSS (Info 3): cgroup v2 `memory.high`/`memory.max`, the
  smallest set value walking from the process's cgroup to the root, else
  `MemAvailable`; minus `VmRSS` read once before replay. Printed on the exit
  line with its source named.
- **A4 — measure it (codd-cyril).** The estimate against RSS on iteration 1's
  two reference shapes, steady state and `VmHWM`; the `ceiling` leg becomes a
  checked refusal; iteration 1's 64 MiB swap-off leg with the default budget
  in force must end in a refusal, not a SIGKILL — that run is the headroom
  measurement; the `randread` over-cap leg and the growth footprint legs pin
  an explicit `WO_DB_MB` larger than their caps so they still measure what they
  measure.
- Verify: both reference shapes track inside the stated error; deleting rows
  returns the estimate to its prior value; a keys-resident table's estimate
  drops to its id map and indexes after the barrier; the breach line names the
  right table and the right annotation; the `ceiling` leg's `rc` is a checked
  refusal.

### Phase B — declaring the bound

- Extend the `@table` surface from [2](02-table-storage-modes.md) with two
  arguments: `max_rows: <integer literal ≥ 1>` and `on_full: refuse |
  drop_oldest`. Same catalogued-diagnostic discipline (WO-E102 for every
  invalid combination — Info 5), defaults that keep every existing table
  unbounded so nothing changes silently.
- `on_full: drop_oldest` on a `durable: true` table is refused at compile
  time (fork 1) — evicting a row that was acked as durable is a promise being
  broken, whichever record the log would carry. The audit-table need is
  archival, [6](06-cold-tiering.md)'s, not eviction.
- `on_full: drop_oldest` on a table some `ref` field targets — from another
  table or from itself — is refused at compile time (Info 12): eviction would
  dangle it, and the compiler already walks ref targets for WO-E224.
- The class descriptor carries `max_rows` and the policy bit (`.wob` v9, the
  contract in `00-wob-format.md` and `04-db-binding.md` moves in the same
  commit); the loader refuses the policy bit without `max_rows`, and either
  without `WO_CLASSF_TABLE`, as v8 refuses the storage bits.
- Verify: golden fixtures per policy; existing tables unchanged (empty `git
  diff` over the goldens after a bless run, as 2 proved it); illegal
  combinations refused at compile time with the catalogued code; a v8 image
  refused by version as v7 was.

### Phase C — the accounting

Folded into Phase A on 2026-09-10: the byte figure is A1's estimate and the
row count is `db_table.count`, which already exists (`table.h`). Nothing left
to build here; the heading stays so the phase letters in the Progress table
and the History stay stable.

### Phase D — the policies

- **Refuse** (`on_full: refuse`, the default): the bound is a hard ceiling.
  The N+1st insert traps `WO_T_DB` with the static message "table is full",
  before anything mutates — the check is on `count` at the top of both insert
  forms, so no un-apply is needed. Legal on every table, durable or not,
  `all` or `keys`. A replayed log holding more rows than the compiled
  `max_rows` allows refuses at boot, exit 2, by name — a shrunken declaration
  is a migration the engine will not perform silently (the schema-migration
  precedent, `main.c`'s "anything else refuses by name").
- **`drop_oldest`**: when `count` equals `max_rows`, remove the smallest live
  id via `wo_row_remove`, then insert. The oldest is found by a per-table
  cursor that starts at the shard's first id and advances past ids the
  program already deleted (ids are monotonic and never reused, so the cursor
  never goes backwards and never sees a reused slot as new). Volatile-only,
  so no WAL record is involved; indexes, unique shadows and the free-slot list
  are kept honest by the choke point it calls.
- **Back-pressure — cut** (Info 4). Recorded here because the goal named it.
- Verify: exactly one row leaves per over-capacity insert; after eviction
  every index agrees with the slab and no bucket names the evicted id; the
  evicted id is never handed out again; `refuse` leaves the count at N and
  the indexes untouched.

### Phase E — the pressure signal

Cut 2026-09-10 (Info 4, Info 11). The process-level signal **is** Phase A's
refusal and its message: the message names the largest table and the exact
annotation, and the developer's declaration — `max_rows` on the cache,
`resident: keys` on the big table — is the shedding. Automatic cross-table
shedding would be a second policy with a precedence rule nobody declared;
the doctrine prefers the refusal that names the fix.

### Phase F — gate it against the measurement

- Re-run iteration 1's growth workload with the default budget in force. The
  proof is a before/after on the same harness: previously the process was
  SIGKILLed at 360 000 rows under 64 MiB with swap off; now it refuses by
  name before the kill, and the durable prefix is intact either way.
- Baseline rows: insert throughput on a `drop_oldest` table at capacity (the
  eviction's per-insert cost), the estimate-to-RSS ratio per shape.
- Verify: `just employee`, `just db-actor`, `just db-bench`, `just residency`
  green; the gate bites on a doctored estimate (a table whose counted bytes are
  forced to zero must fail the shape check).

## Progress

| # | Task | Size | State |
| --- | --- | --- | --- |
| A1 | the per-table resident estimate: fields on `db_table`, maintained at every allocation and free inside the choke points, chunk-rounded; `CODE-LOGIC.md` section | M | ⬜ |
| A2 | `WO_DB_MB` parse-or-refuse; sum-over-tables check after the row lands, un-apply, `WO_T_OOM`; replay crossing → exit 2; the named stderr line; the `WO_WAL_STATS` exit line | M | ⬜ |
| A3 | the default: cgroup v2 ancestry walk, `MemAvailable`, boot `VmRSS`; source named on the exit line | M | ⬜ |
| A4 | codd-cyril: estimate vs RSS on both shapes (steady + `VmHWM`); `ceiling` → checked refusal; 64 MiB swap-off leg ends in a refusal with the default (the headroom measurement); `randread`/growth legs pin `WO_DB_MB` | L | ⬜ |
| B1 | grammar `max_rows`, `on_full`; WO-E102 causes; error catalogue row; goldens unchanged | S | ⬜ |
| B2 | types.ml: `drop_oldest` on a `ref` target refused (beside WO-E224) | S | ⬜ |
| B3 | `.wob` v9: descriptor carries `max_rows` + policy bit; loader refusals; `00-wob-format.md` + `04-db-binding.md` | M | ⬜ |
| D1 | `refuse`: count check at the top of both insert forms; replay over `max_rows` → exit 2 by name | S | ⬜ |
| D2 | `drop_oldest`: oldest-id cursor, `wo_row_remove` in front of the insert; unit checks for one-out-one-in, index agreement, id never reused | M | ⬜ |
| F1 | codd-cyril: gate legs for `refuse`/`drop_oldest`; two baseline rows; the doctored-estimate bite | M | ⬜ |
| P1 | codd-pm: board, graph §8, `codd.md` knob list, db-bench README | S | ⬜ |

Order: A1 → A2 → A4's shape and `ceiling` checks → A3 → A4's swap-off run
(the headroom measurement, which may add one itemised reserve to A3 with that
run as its citation) → B1–B3 → D1 → D2 → F1 → P1. Phase B is where the
blast radius lives (a grammar arm, a `.wob` bump, loader refusals); a
prebuild-feature brief before B1 is recommended, and none is needed for A.

## Acceptance Criteria

Outstanding — all of them, the iteration has not started:

- **Given** a table with `max_rows: N, on_full: refuse`, **when** the N+1st
  insert is attempted, **then** it traps `WO_T_DB` "table is full", the row
  count stays N, and every index agrees with the slab.
- **Given** a `durable: false` table with `max_rows: N, on_full: drop_oldest`,
  **when** the N+1st insert arrives, **then** exactly one row — the smallest
  live id — is removed, the new row is present, the count is N, and no index
  or unique shadow references the removed row.
- **Given** an evicted row's id, **when** it is looked up, **then** it is absent
  — and its id is never reused by a later insert, preserving the invariant the
  id hash's tombstone sentinel depends on.
- **Given** `on_full: drop_oldest` on a `durable: true` table, or on a table
  some `ref` field targets, or `on_full` without `max_rows`, or `max_rows: 0`,
  **when** compiled, **then** WO-E102 names the combination and the way
  forward, and no `.wob` is written.
- **Given** `WO_DB_MB` set to a whole number of MiB, **when** the sum of the
  tables' estimates would exceed it, **then** the crossing insert traps
  `WO_T_OOM`, the row is not present afterwards, and one stderr line names the
  largest table, its rows and bytes, the budget and `WO_DB_MB` as its source,
  and the annotation to add. **Given** any other value (`0`, a fraction, a
  suffix, empty is "unset"), **then** the program refuses at boot, exit 2,
  naming the accepted form.
- **Given** a replayed store whose tables cross the budget during replay,
  **when** the program boots, **then** it exits 2 with the same named line
  before serving anything — the 120 GB developer meets a diagnostic naming
  `@table(resident: keys)`, not the OOM killer.
- **Given** no `WO_DB_MB`, **when** the program starts, **then** a default
  budget is in force — the binding cgroup v2 limit or `MemAvailable`, minus
  boot `VmRSS` — and the `WO_WAL_STATS=1` exit line prints it with its
  source. "Unset" is never "no budget".
- **Given** iteration 1's 64 MiB swap-off growth leg with the default budget,
  **when** it runs to what previously killed the process at 360 000 rows,
  **then** it ends in the named refusal, not signal 9, and the durable prefix
  is intact — the before/after that justifies the iteration. If the kill wins,
  the measured gap between the estimate at death and the limit is recorded and
  becomes the one itemised reserve, with the run as its citation.
- **Given** the estimate on iteration 1's two reference shapes, **when**
  compared to the measured RSS delta per row (steady state and peak), **then**
  the ratio is recorded in `bench/baseline.json` at the footprint tolerance
  (±10%) and stated in this file and in `CODE-LOGIC.md` — a budget nobody can
  check is a budget nobody trusts.
- **Given** rows deleted from a table, **when** the estimate is read, **then**
  it returns to its value before those rows were inserted (slabs stay, as
  they do today; the values and the count do not).
- **Given** a `resident: keys` table, **when** its inserts have passed the
  barrier and their payloads dropped, **then** its estimate is its id map,
  indexes and scratch — the rows are counted only while they wait in slabs
  for the barrier.
- **Given** the `ceiling` leg, **when** the process dies at the cap, **then**
  `rc` is a checked refusal (a positive exit code), never a signal — the leg
  already asserts `rc` precisely so that this improvement passes.

## Out Of Scope

- **Writing evicted rows anywhere** — that is [6](06-cold-tiering.md). Here
  eviction means the row is gone. Keeping the two apart is what makes the policy
  work reviewable on its own.
- **Back-pressure as a parked writer** (cut 2026-09-10, Info 4). A parked
  writer needs a wake condition — "until the table shrinks" — that only the
  program can define, and a new wait-queue on the owner shard to carry it. The
  refusal is the primitive; a fiber that catches it, waits and retries is the
  back-pressure, built from what the language already has.
- **Cross-table shedding on process pressure** (cut, Info 11). The budget's
  message names the table and the annotation; the developer's declaration is
  the shedding policy. An automatic one would need a precedence rule nobody
  declared and would silently delete from one table because another grew.
- **A second, warning threshold** (Info 4). It would be a knob or an arbitrary
  fraction of the budget. The exit line under `WO_WAL_STATS=1` gives the
  developer the figure to tune from; the refusal line gives the projection.
- **A per-table byte bound** (Info 2). It would tie a developer-facing
  semantic ("when does my insert fail?") to an estimate with an error bound;
  rows are exact, and the RAM question is the process budget's.
- **A fraction knob beside the byte knob** (Info 5). The kernel needs both
  `dirty_bytes` and `dirty_ratio` because a sysctl author cannot compute the
  machine at boot; the runtime can, prints the derived default, and the
  operator who wants half of it writes the number.
- **A per-program budget declaration** (Info 5). The budget is a property of
  the host, not of the program; a program compiled once runs on many hosts.
- **A builtin that returns a table's size** (Info 10). Language surface with
  a contract change, for a figure the exit line already reports; the
  observability iteration (language 30) is where metrics get an endpoint.
- **True LRU** and any eviction that costs a write per read. Touching per-row
  metadata on every read would turn the 1 µs read path into a write path — the
  same trap iteration 3's session touch has. `drop_oldest` is FIFO and says so.
- **The TTL cache middleware** — language
  [iteration 18](../language-runtime-database/18-memory-db-features.md). Expiry
  by *time* is that; bounding by *size* is this. They compose.
- **Query-level result limits.** `take n` already exists in the query surface.
- **Shrinking slabs back to the allocator.** Slab addresses are stable forever
  by design and that invariant is load-bearing; reclaiming a slab whose rows were
  all evicted is a separate, delicate change with its own iteration if anyone
  wants it. The estimate therefore counts whole slabs, and a delete-heavy table
  plateaus rather than shrinks — stated, not hidden.

## Info — the forks, settled

Settled 2026-09-10 by codd-shoney, from the code and the two measured
iterations. Every pick cites where the code or the measurement is; the
reference trees are cited for behaviour, never code.

1. **Eviction is never legal on a `durable: true` table.** `on_full:
   drop_oldest` with the default durability is WO-E102 at compile time, in
   the parser's post-argument combination check beside the existing
   `durable: false, resident: keys` refusal (`compiler/src/parser.ml:403-406`).
   `on_full: refuse` and `max_rows` alone are legal on every table. *Why:*
   principle 7 — "every mutation is WAL-logged and fsynced before
   acknowledgment" — makes an ack a promise that the row exists until the
   program deletes it; the engine deleting it later is that promise broken,
   even though a kind-2 record would keep the log consistent. PostgreSQL never
   evicts rows: it spills (`work_mem`, `tuplesort.c:600`) and refuses only at
   `temp_file_limit` (`storage/file/fd.c:2270-2273`) — eviction is a cache
   concept, and a cache is volatile by definition. The audit-table-that-must-
   not-grow need is archival, redirected to 6. Alternative rejected: a
   distinctly named durable policy ("discard acked rows") — a second spelling
   of the same broken promise.
2. **"Bounded" is rows per table and bytes per process — never the other
   way, never both on one side.** `@table(max_rows: N)` is the per-table
   capacity; `WO_DB_MB` is the process budget. *Why rows per table:*
   `db_table.count` exists and is exact (`table.h`); a developer sizing a
   session store thinks in sessions; a per-table byte bound would make "when
   does my insert fail" depend on an estimate with a measured error. *Why bytes
   per process:* iteration 2's fork 5 and the residency spec's decision table
   (`2026-08-26-table-residency-design.md:22`) — 96.5 vs 320.6 B/row, a row
   count cannot bound RAM. A process-wide row cap has no meaning across shapes.
3. **The pressure signal is one budget in bytes, compared against the
   engine's estimate; the default is the binding limit minus boot RSS.**
   Binding limit: walk `/proc/self/cgroup`'s v2 path from the process's
   cgroup to the root, take the smallest `memory.high` or `memory.max` that is
   not `max` (`memory.high` is "the main mechanism to control memory usage"
   and throttles rather than kills, `cgroup-v2.rst:1350-1361, 1916`;
   `memory.max` is where the OOM killer is invoked, `:1376-1383`; an ancestor's
   limit binds a child exactly as `:1317` says of `memory.min`); with none set,
   `MemAvailable` — the kernel's own estimate of what userspace can take
   "without causing swapping or OOM" (`mm/show_mem.c:32-73`), which is exactly
   the line iteration 1 says we must speak before. Minus `VmRSS` from
   `/proc/self/status`, read once before replay (the sample already reads it,
   `docs/examples/db-bench/main.wo:567`). *What was rejected:* a fraction —
   iteration 1 §Outstanding says the fraction "cannot be measured; the
   premise is false" and the plan's "conservative placeholder" is exactly the
   invented default this brainstorm may not write; `MemTotal` — ignores every
   other process on the box; an itemised reserve (arena cap × shards, WAL
   staging) — unmeasured numbers, and the arena's cap is a reservation, not
   usage: subtracting 64 MiB × cores from an 8 MiB test cap computes a
   negative budget and would refuse at boot programs that run fine today.
   *What stays open inside the iteration, as a measurement not a fork:*
   whether limit-minus-boot-RSS leaves enough headroom for the estimate's
   error, the arenas' growth and the hash doublings' transient. Task A4 runs
   iteration 1's 64 MiB swap-off leg (SIGKILL at 360 000 rows today,
   `perf-targets.md` §5) with the default in force; if the kill still wins, the
   measured gap is the one itemised reserve, with that run as its citation.
   The code carries no placeholder at any point: until A3 lands, `WO_DB_MB`
   unset is the engine's present behaviour, which is HEAD's.
4. **On breach the budget refuses; a full table refuses or drops its oldest;
   nothing warns at a second line, parks a writer, or sheds another table.**
   Budget: the crossing insert traps; during replay, exit 2 (Info 7). *Why
   refuse over evict for the budget:* the doctrine prefers the option that
   refuses loudly over the one that guesses which table to shrink; the
   message names the fix. *Why no warn threshold:* it is a second knob or an
   arbitrary fraction (the kernel's `dirty_background_ratio` is a knob; PG has
   none for `temp_file_limit`); the `WO_WAL_STATS=1` exit line reports the
   peak against the budget so the developer can tune, and the refusal line
   carries the projection. *Why no back-pressure:* `memory.high` throttling is
   the kernel's back-pressure and its own documentation says it "should be
   used in scenarios where an external process monitors the limited cgroup"
   (`cgroup-v2.rst:1359-1361`) — throttling without an agent is slow death; a
   parked writer here would need a wait-queue and a wake condition the engine
   cannot define. The program composes waiting from the trap. *Durable vs
   volatile:* the budget treats them alike (both are RAM); `drop_oldest` is
   volatile-only (fork 1).
5. **Exposure: the budget is environment only, the capacity is grammar
   only; no overlap, so no precedence.** `WO_DB_MB=<MiB>`: a whole number
   ≥ 1 parsed to the end of the string as `WO_HEAP_MB` is (`main.c:219-224`);
   unset or empty means the default; any other value refuses at boot, exit 2,
   naming the accepted form — task 6a's rule for `WO_EPHEMERAL` (`main.c`,
   "the only accepted value"), not `WO_HEAP_MB`'s silent fallback, which is a
   pre-existing silent mode this iteration does not copy. MiB, not bytes,
   because a human sets it (`WO_HEAP_MB` precedent); `WO_CHECKPOINT_BYTES`
   is bytes because a gate sets it. Grammar: `max_rows: <integer literal>`
   and `on_full: refuse | drop_oldest`, both optional, `on_full` defaulting
   to `refuse`; given twice, `on_full` without `max_rows`, `max_rows: 0`, a
   non-integer, an unknown policy word, `drop_oldest` on a durable table or a
   `ref` target — all WO-E102 with a message naming the way forward, the
   convention `parser.ml:348-387` already follows (a retired word gets a
   message stating the real keys). The spec's "per-program declaration" for
   the budget is dropped: the budget is the host's property. *Observation:*
   one exit line under `WO_WAL_STATS=1` (pattern `main.c:175-186`), one
   refusal line; no builtin (Info 10).
6. **The estimate counts what the engine asked the allocator for, per
   table, chunk-rounded, at the sites that allocate.** Terms: slabs
   (`table.c:596`, whole slabs of 256 rows — the estimate leads RSS by at most
   one slab per table, conservatively), the slab pointer array (`:591`), the
   bitmap (`:599`), the free list (`:642`, `:1405`, `:1451`), the id map
   (`hgrow`, `:229-247`, two arrays of `hcap` words at load ≤ 0.7,
   `hput :253`), index bucket arrays (`idx_bucket :421-437`) and per-bucket id
   lists (`:477`, `:1124`), the scratch buffer (`:841`), and every
   engine-owned value — `db_text` (`:72`, `:933`), `db_rec` (`:82`, `:942`),
   `db_multi` (`:100`, `:958`), `db_map` (`:117`, `:974`) — added when
   encoded and subtracted when freed (`db_val_free :27-50` learns to report
   the bytes it frees; it already knows every size from the value and the
   class table). Each request is rounded to glibc's chunk — 8-byte header,
   16-byte multiple, 32-byte floor — because the target is Linux/glibc
   (principle 9) and the arithmetic says the headers are material: for the
   `Wide` shape (`db-bench/types.wo:32-38`, one Int and three Texts) the
   chunk-rounded sum comes to roughly 320 B/row against the measured 320.6,
   while raw requested bytes come to roughly 265 — an 18% undercount that a
   budget would inherit. *Where it lives:* two running fields on `db_table`
   (heap values, index id lists) plus terms derived from fields that already
   exist (`slab_cnt`, `row_size`, `hcap`, `bcap`, `free_cap`,
   `scratch_cap`); the process sum is a running total on `wo_db` updated at
   the same sites. *Where it is checked:* after the row has landed in
   `wo_row_insert` / `wo_row_insert_slots`, because only then are the row's
   heap values and any slab or hash growth known; over budget, the row is
   un-applied exactly as a unique violation is (`table.c:662-675`: bitmap bit
   cleared, `hdel`, count and `next_id` reclaimed, values freed, slot
   recycled; the slots form repeats it at `:714-725`) — precedent, not
   rollback: nothing has been staged, `db.c:46-85` stages only after the
   insert returns. Replay (`wo_row_create_raw :886`,
   `wo_row_raw_commit`) counts and checks the same way and returns a distinct
   code so `main.c` can name the budget, as `apply_record`'s `-2` names a mode
   mismatch (`wal.c:1736`). *Error bound:* measured by A4 on both shapes at
   steady state and at `VmHWM`, recorded in `bench/baseline.json` at ±10%,
   stated here and in `CODE-LOGIC.md`, printed on the exit line as the
   ratio; never implied.
7. **The default when nobody sets anything is Info 3's derivation; the
   refusal message contract is fixed here.** One stderr line, the shape of
   task 6a's refusal (`main.c:346-351`: the class, the ways forward, no
   "+N more"): the largest table by estimate with its rows and
   bytes, the process estimate, the budget and its source (`WO_DB_MB`,
   `memory.high`, `memory.max` or `MemAvailable`), and the annotation — for a
   durable `resident: all` table, `@table(resident: keys)`; for a `durable:
   false` table, `max_rows:` (the number is the developer's, not guessed);
   for a table already `resident: keys`, that its id map and indexes alone
   exceed the budget, so the budget or the memory limit must grow or rows must
   go. The trap's own message stays static ("resident byte budget exceeded")
   like every `*msg` in `table.c`; the named line is printed once per table
   per process. The trap class is `WO_T_OOM` through the existing
   `DB_ERR_OOM` mapping (`db.c:51-53`, `:101`, `:282-284`, `:310`) — no new
   trap, no new error kind, byte-identical on the inline and RPC paths
   because both run the same function. It is out-of-memory by declaration,
   and a program that already catches the arena's `WO_T_OOM` catches this.
8. **`resident: keys` and compaction.** A keys table's estimate is what is
   resident: its id map, its indexes and its scratch. Its rows are counted
   while they sit in slabs between the insert and the barrier
   (`wo_row_insert` then `wo_db_flush_drops` → `wo_row_drop_payload
   :1387-1413`, which frees the values and recycles the slot — the estimate
   falls there), because that memory is real. Compaction re-points offsets
   (`wo_row_set_offset :1470`) and touches no slab or value, so the estimate
   is unmoved; the compactor's staging buffer is WAL memory and is not counted
   (the estimate is the tables', stated in Info 6). `max_rows` on a keys table
   is legal with `refuse` only (a keys table is durable, fork 1). The budget's
   annotation for a keys table is Info 7's third form.
9. **"Least valuable" is the smallest live id, and the policy says so.**
   `drop_oldest`, not `evict`, not `lru`. No per-row access time exists
   (`db_row` is id, class, reserved flags, slots — `table.h`); ids are
   monotonic per table and never reused (`next_id`, `table.c:651-652`), so a
   per-table cursor that starts at the shard's first id and advances past ids
   `hget` no longer finds is FIFO in amortised O(1). The bitmap walk is slot
   order, not insertion order — slots recycle — and the id map is hash order,
   so neither is a substitute for the cursor. Language 18's cache class is
   where an LRU, if one is ever wanted, belongs (`18-memory-db-features.md:75,
   101`): pure `.wo`, no engine change.
10. **Size is observed on the `WO_WAL_STATS=1` exit line and in the refusal
    line; no builtin.** A builtin is a new id, a compiler arm, a marshalling
    case through the DB actor and a contract change, for a figure a gate and a
    developer can read from stderr today; language 30 (observability) is where
    metrics get an endpoint. The exit line: budget, source, peak estimate,
    estimate-to-RSS ratio, and per table its name, rows and bytes.
11. **Precedence when several tables could shed — moot.** No table sheds for
    another (Info 4). Largest-first, application priority and proportional
    shedding were the options; each is a policy the developer did not declare
    acting on data the developer did.
12. **Traps, codes and format.** Budget → `WO_T_OOM` (Info 7). `max_rows`
    with `refuse` → `WO_T_DB` with the static message "table is full", checked
    on `count` at the top of both insert forms before any allocation — the
    program handles a full cache as a normal condition, not as OOM.
    `drop_oldest` on a table that any `ref` targets → compile-time WO-E102 in
    `types.ml` beside WO-E224 (`types.ml:534`, `:802`), which already walks
    ref fields to their target classes — a compile-time refusal beats a
    runtime `WO_T_FK` on the eviction, which would leave the table
    permanently full, and beats skipping to the next-oldest, which is a
    guess. A replayed log with more rows than `max_rows` → exit 2 by name at
    boot. `.wob` v9: the descriptor gains `max_rows` and a policy flag bit;
    the loader refuses the bit without `max_rows` and either without
    `WO_CLASSF_TABLE` (`wob.h:660-670`, the v8 pattern); v8 images are refused
    by version as v7 were. Goldens are expected unmoved for the same reason
    v8 moved none — the dump prints flags by name and no golden declares a
    bounded table — and the bless-diff proves it.

## History

**2026-09-10 — brainstormed to `ready` by codd-shoney, under autonomy.**
Twelve forks settled above; three goals rewritten where a decision changed
them (rows-only capacity, back-pressure cut, the 273× goal re-aimed at the
budget's timing); Phases C and E folded and cut; a Progress table added with
sizes. What the developer should look at first is in the frontmatter's
`review_pending`. Two things were deliberately **not** invented: a fraction
for the default budget (iteration 1 showed there is no swap onset to derive
one from, and the plan's "conservative placeholder" was the contradiction
the notes flagged) and a reserve made of unmeasured items — both replaced by
a derivation from figures the kernel reports and a named measurement (A4)
that either confirms the headroom or supplies the one reserve with its
citation. A prebuild-feature brief is recommended before Phase B (the grammar
arm and the `.wob` bump are where the blast radius lives); Phase A is
engine-only and needs none.

**2026-09-09 — the byte budget arrived from iteration 2** as Phase A, with
five design notes (bytes XOR fraction; `MemAvailable` and the cgroup limits as
denominator; a fraction minus an itemised reserve with a floor; heap-only,
slab-granular estimate; the message names the largest table). Of those, the
denominator, the estimate's granularity and the message survive as decided;
the fraction, the reserve and the floor were replaced (Info 3).
