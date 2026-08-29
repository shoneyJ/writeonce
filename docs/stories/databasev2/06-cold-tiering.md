---
track: databasev2
iteration: "6"
status: hold
readiness: refine
---

# databasev2 6 — cold tiering: rows that leave RAM and come back

> Part of [Story — databasev2: the database beyond RAM](00-story.md).
> Needs [2](02-table-storage-modes.md) for the `cold` mode declaration,
> [3](03-wal-checkpoint.md) so the log this builds on does not grow forever,
> and [5](05-bounded-tables-eviction.md) for the policy machinery.
>
> **⚠ LARGELY SUPERSEDED 2026-08-27 by [iteration 2](02-table-storage-modes.md).**
> This iteration was written to implement a `cold` mode. That mode no longer
> exists: the brainstorm replaced it with `resident: all | keys`, and
> **`resident: keys` is the ceiling-raising mechanism** — indexes resident, rows
> read from the log by offset. It is iteration 2's tasks 5c/5d — which landed
> 2026-08-29 — not this file's.
>
> Its premise was also specifically *rejected*, not merely relocated. This
> iteration assumed a **user-space resident working set** with faulting and an
> eviction policy from [iteration 5](05-bounded-tables-eviction.md). The spec
> chose the opposite: no user-space row cache at all, because the kernel page
> cache already is one and a `pread` against a cached page is a memcpy — the
> position `exploration/postgresql/buffer-and-checkpoint.md` already argued and
> the reason the engine avoids `O_DIRECT`.
>
> **What may still be left:** if measurement after 5c/5d (landed 2026-08-29,
> still unmeasured — that is iteration 2's task 7) shows the page cache
> insufficient for some workload, a user-space working set becomes arguable
> again — but only with that number in hand, which is the opposite of how this
> file was written.
>
> **That question now has a reference point (iteration 1, 2026-08-27).** Random
> reads over a table larger than RAM measured **273× slower** than resident
> (1 851 166 vs 6 771 reads/s; p99 1 µs vs 487 µs) — but that is the *kernel's
> swap* path: demand-paged anonymous memory, 4 KiB per fault, no readahead. It is
> the number `resident: keys` must **beat**, since it `pread`s through the page
> cache, which gets readahead and a shared cache. So this file revives if and
> only if 5c/5d measures the page-cache path landing near 273× rather than well
> below it. Until that measurement exists, neither outcome is assumed. Until then treat the design questions below as answered
> elsewhere and the phases as void. Its genuinely durable contribution is its
> fork list, especially "does the language surface the fault cost at the *use*
> site" — still open, and still the largest question about what writeonce is.
>
> **The iteration that actually raises the ceiling, and the one most likely to
> go wrong.** Everything before it makes the limit visible, declared and
> managed. This one removes it — for tables that opt in — and in doing so
> touches the project's most load-bearing principle. It should be approached
> with more suspicion than enthusiasm.

## Goals

- **A `cold` table may hold more rows than fit in memory.** Recently-used rows
  are resident; the rest live on disk and are faulted back on access. This is
  the whole feature and every other goal is a constraint on it.
- **Do it without becoming a paged storage engine.** `discarded.md` records that
  the disk story is the WAL and that a paged B-tree engine was rejected. `cold`
  must not be a licence to rebuild SQLite inside `database/src`. The design that
  respects the doctrine reuses the log that already exists plus an index into
  it — a log-structured read path, not a page cache.
- **Keep the resident path exactly as fast as it is.** A `durable` or `ram`
  table must not pay one instruction for a feature it does not use. Iteration
  22's 1.3M ops/s read baseline is the regression gate, and a measurable read
  regression on non-`cold` tables is grounds to reject the design, not to tune
  it.
- **Be honest in the query surface about what a fault costs.** A scan over a
  `cold` table can touch disk per row. The engine currently answers every read
  from memory at p50 1µs; a `cold` scan is a different animal and the language
  should not pretend otherwise — see fork 3, which is the most important
  question in this iteration.
- **Never lose an acked write.** Every guarantee iterations 9 and 22 established
  holds byte-for-byte: ack-after-fsync, whole-or-nothing replay, torn tails
  dropped by CRC. A tiering layer that weakens any of those is a regression
  disguised as a feature.

## Phases

### Phase A — settle the design before writing any of it

- This iteration needs a spec more than any other in the track. The candidate
  shapes are genuinely different: (a) the WAL becomes the primary store with an
  in-memory id→offset index and a resident row cache; (b) a separate
  append-only row file per cold table, checkpointed by 3's machinery; (c)
  eviction to disk with a free-space map, which is the paged engine wearing a
  hat.
- Whichever wins must state its read amplification, its recovery story, and what
  happens when the index itself does not fit — an id→offset map for a billion
  rows is not free either, and a design that only moves the ceiling is worth
  knowing about before it is built.
- Verify: the spec names the shape, the amplification, and the failure modes.
  No code in this phase.

### Phase B — the resident/cold boundary

- Which rows are resident: reuse [5](05-bounded-tables-eviction.md)'s policy and
  accounting rather than inventing a second notion of "least valuable".
- Eviction becomes write-then-drop instead of drop, and it must be atomic with
  respect to a concurrent reader — a row that is being written out must not be
  briefly unreachable.
- The fault path: a lookup that misses resident memory reads from disk,
  materialises the row, and admits it under the resident policy.
- Verify: a table larger than the resident bound serves correct rows for every
  id; a row evicted and faulted back is byte-identical, including every heap
  value (`db_text`, `db_rec`, `db_multi`, `db_map` each round-trip).

### Phase C — indexes and constraints across the boundary

- **The hard part, and the reason this is late in the track.** A secondary index
  over a `cold` table either stays fully resident (bounding the table by index
  size rather than row size — which may be the honest answer) or is itself
  tiered. A `@unique` constraint must hold across rows nobody has in memory: the
  shadow check cannot scan a slab that is not there.
- Foreign-key restrict must also hold — a delete has to know whether any cold
  row references it.
- Verify: `@unique` refuses a duplicate whose only conflicting row is cold; FK
  restrict refuses a delete whose only referrer is cold. These two criteria are
  the correctness core of the iteration.

### Phase D — recovery

- Crash mid-eviction, crash mid-fault, crash mid-checkpoint-of-a-cold-table.
  Each must recover to a consistent state with no acked write lost and no row
  visible twice.
- Interaction with [3](03-wal-checkpoint.md)'s snapshot: a cold table's on-disk
  rows are part of the durable state a checkpoint must account for, not
  something it can truncate past.
- Verify: `kill -9` at each of the three points, replayed, with every acked write
  present and the resident/cold split re-derived correctly.

### Phase E — measure it, then decide whether to keep it

- Read/write throughput and p99 for a `cold` table at several resident ratios,
  and a **regression check that non-`cold` tables did not move**.
- Publish the amplification honestly in `perf-targets.md`: how much slower a
  cold fault is than a resident read, as a number.
- Verify: `just db-bench` green with new cold-path rows; the resident baseline
  unchanged; `just employee`, `just db-actor`, `oop-accept` green; ASan and TSan
  clean on the fault path.

## Acceptance Criteria

- **Given** a `cold` table with more rows than the resident bound, **when** any
  row is looked up by id, **then** it is returned correctly whether resident or
  faulted, byte-identical including every heap-valued column.
- **Given** a `cold` table under a read workload, **when** the resident set is
  smaller than the working set, **then** the process holds steady state without
  approaching the RAM ceiling iteration 1 measured.
- **Given** a `@unique` column on a `cold` table, **when** a duplicate is
  inserted whose conflicting row is **not resident**, **then** the insert is
  refused — the constraint holds across the boundary or it does not hold.
- **Given** a `ref` into a `cold` table, **when** the referenced row's owner is
  deleted and the only referrer is cold, **then** FK restrict refuses the delete.
- **Given** `kill -9` during an eviction, a fault, and a checkpoint, **when** the
  program restarts, **then** every acked write is present, no row appears twice,
  and the resident/cold split is re-derived correctly.
- **Given** a `durable` or `ram` table, **when** the read benchmark runs after
  this iteration, **then** its throughput and p99 are inside the existing
  baseline tolerance — no cost for a feature not used.
- **Given** a cold fault, **when** its latency is measured, **then** the
  amplification versus a resident read is recorded in `perf-targets.md` as a
  number a developer can plan around.

## Out Of Scope

- **A paged B-tree storage engine.** Explicitly rejected in
  [`discarded.md`](../../plan/discarded.md) and not reopened by this iteration.
  If the spec phase concludes that tiering *requires* one, the correct outcome is
  to reject tiering and say so — not to quietly build the thing the project
  decided against.
- **Making `cold` the default, or applying it to a table that did not ask.**
  Opt-in per table, forever.
- **Tiering to anything but the local filesystem.** Object storage needs
  outbound sockets (language
  [iteration 38](../language-runtime-database/38-content-platform-capabilities.md))
  and would change the latency story by orders of magnitude.
- **Compression of cold rows.** Composes with
  [porch 7](../porch/07-sse-and-compression.md)'s codec if that lands first;
  not a dependency either way and not this slice.
- **Cross-shard cold tables.** The owner shard owns the store and the WAL; a
  cold table is more of the same. Per-shard storage is a separate architectural
  question noted in [2](02-table-storage-modes.md)'s forks.
- **Tiering the query planner's behaviour.** If a scan over a cold table is
  expensive, the answer for now is that it is expensive and documented — not a
  cost-based planner.

## Info

Forks the spec must settle — this iteration is mostly forks, which is why phase
A produces no code:

1. **Which shape?** WAL-as-primary-store with an id→offset index and a row
   cache reuses machinery that exists and keeps the doctrine ("the disk story is
   the WAL") literally true. A separate per-table row file is cleaner to reason
   about and duplicates the log. Eviction with a free-space map is the rejected
   paged design. Leaning (a), with the caveat in fork 2.
2. **What if the index does not fit either?** An id→offset entry per row is far
   smaller than a row, so this moves the ceiling by a large constant — but it
   does not remove it. Say so plainly in the spec: `cold` buys an order of
   magnitude, not infinity. A design sold as unlimited will be deployed as if it
   were.
3. **Does the language surface the cost?** Three positions. Silent — a cold
   table reads like any other and the developer discovers the latency in
   production. Annotated — the mode is at the declaration, so an attentive
   reader knows, which is the status quo of this design. Or *explicit at the use
   site*, where a query over a cold table must acknowledge it somehow. The third
   is most in keeping with a language whose whole thesis is that the compiler
   tells you the truth — and it is also the most intrusive. This is the fork with
   the largest effect on what writeonce *is*, and it deserves the brainstorm more
   than any implementation detail here.
4. **Is `@unique` on a cold table simply refused?** Keeping a unique index fully
   resident is a bound on the table by index size, which is honest and simple.
   Refusing `@unique` on `cold` outright is even simpler and might be right for
   a first version — a constraint that silently only checks resident rows would
   be a correctness hole, and that is the one outcome that must not ship.
