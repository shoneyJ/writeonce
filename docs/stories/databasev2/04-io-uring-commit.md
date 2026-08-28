---
track: databasev2
iteration: "4"
was_language_iteration: "23"
status: in-progress
chain: 5
---

# databasev2 4 — io_uring group-commit write path

> **Moved 2026-08-26** from the language track, where this was iteration 23.
> Part of [Story — the database beyond RAM](../language-runtime-database/00-story.md). Content unchanged by
> the move; its dependencies are restated in that track index.

> Format: fiberloom `product/story-iteration-template`. Part of
> [Story — one language, one runtime, one database, one binary](../language-runtime-database/00-story.md).
>
> **Inserted 2026-08-15.** The write-path optimization, and deliberately the
> LAST database performance iteration: it only earns its complexity once
> there is a measured fsync-per-commit baseline to beat (iteration 22) and a
> multithreaded runtime to overlap against (iteration 8). Doing it earlier
> would optimize a number nobody had measured, against a runtime that
> couldn't use it.
>
> **No spec exists yet.** ~~The forks in *Info* are genuine decisions.~~
>
> **REFINED 2026-08-20: the four forks are SETTLED as their recorded
> leanings** (developer confirmation, no code): (1) drop-in behind
> `wo_wal_commit` first, an async variant only if the arc's scheduler
> proves the blocking boundary is the bottleneck; (2) raw
> `io_uring_setup`/`io_uring_enter` syscalls — libc-only doctrine holds,
> ring layout documented normatively; (3) the batch boundary is the shard
> tick (the 8+11 arc's quantum), single-writer fallback batches whatever
> accumulated; (4) startup auto-probe + an env override so CI proves both
> paths on one kernel — AMENDED: the override is the arc-wide
> `WO_IO=uring|epoll` (the arc's T4 owns the probe and the per-shard
> ring; `WO_WAL_MODE` is subsumed). Position — RE-SEQUENCED 2026-08-21:
> FIFTH in the concurrency chain (32, WAL checkpoint, follows it —
> added 2026-08-21), **stage 3 → 22 → 31 → 24 → 23 → 32**
> (supersedes the 2026-08-20 old-id ordering "9e → 8+11 → 9f"); the
> per-shard ring already exists (arc T4 landed 2026-08-20,
> `WO_IO=uring|epoll`) — this iteration adds the WAL's WRITE+FSYNC
> chains to it. AMENDED 2026-08-20 (io_uring-first
> directive): the WAL's WRITE+FSYNC chains ride the SAME per-shard ring
> T4 creates for fiber parking — one event loop per shard, readiness ops
> and durability ops together, exactly the linux reference project's
> "single event loop" card. One composition
> note added since iteration 18: a `transaction { }` already IS a staged
> batch — under io_uring it becomes exactly one submission, so the two
> features compose without either knowing the other.

> **BRAINSTORMED 2026-08-28 — and SPLIT IN TWO.** Spec for part A:
> [`2026-08-28-wal-group-commit-design.md`](../../superpowers/specs/2026-08-28-wal-group-commit-design.md)
> · plan: [`2026-08-28-wal-group-commit.md`](../../superpowers/plans/2026-08-28-wal-group-commit.md)
> (6 tasks).
>
> **The payoff metric is `durable.sN.mixwrite`, not the s1 numbers.** Worker
> shards hold no WAL — the runtime asserts it — so every statement on a worker
> marshals to shard 0 and parks, while a statement already on shard 0 runs
> inline. Batches form only where there is a queue, so concurrent multi-shard
> writes batch and a single-shard or serial workload does not. The baseline
> shows why that is the right target anyway: **multi-shard concurrent writes are
> 480 ops/s at p99 5888 µs against single-shard's 1023 at p99 664 — adding
> shards makes durable writing WORSE today**, because every marshaled statement
> still buys its own barrier on the owner.
>
> **The premise below needed correcting.** This story says "replace
> fsync-per-commit with io_uring group-commit", but the engine does not commit
> per commit — it commits per **statement**: `db.c` calls `wo_wal_commit`
> immediately after every append, at all six sites, so every row change is one
> `pwrite` plus one `fdatasync`. That splits the goal into two independent
> wins, and only the second needs io_uring:
>
> - **Part A — batching.** Let many statements share one barrier. The staging
>   buffer already holds any number of records; today it never holds more than
>   one because the caller commits immediately. Mostly a deletion of calls.
> - **Part B — async submission.** The shard submits and keeps working instead
>   of blocking in `fdatasync`. Deferred until A's measurement says whether the
>   blocking boundary is still the bottleneck.
>
> **A is where most of the number lives.** Iteration 22 measured durable writes
> at 4460 ops/s and mixed writes at 1023 ops/s (p99 664 µs) against 1.28M ops/s
> for durable reads — ~290× apart, essentially all of it the per-statement
> barrier.
>
> **Forks settled in the brainstorm:** batch boundary is **queue-drain** (not
> the tick this story recorded — a tick taxes an idle system to serve a busy
> one); a failure between "RAM mutated" and "record durable" is a **fatal,
> diagnosed abort**, replacing today's uneven rollback where `insert` undoes
> itself and `update`/`delete` admit in a comment that they leave RAM ahead of
> disk. **That removes `WO_T_IO` from the write path** — a language-visible
> change, recorded here deliberately.
>
> `status: in-progress` because the brainstorm is done and the spec is
> approved; the plan is next. (The `readiness` axis that would say this
> precisely lives on the unmerged `db-residency-doctrine`.)

## Progress — part A landed 2026-08-28

| # | Task | State |
| --- | --- | --- |
| 1 | a failed barrier is detected, and fatal | ✅ `d3ff03e` |
| 2 | one barrier per drain; replies held | ✅ `b9b8a45` |
| 3 | the inline path takes the fatal rule, asymmetry documented | ✅ `a6ccdbe` |
| 4 | prove batches form — the `wmix` write-concurrent leg | ✅ `40d029c` |
| 5 | measure the payoff, gate it, record it | ✅ `d52ea8a` |
| 6 | closeout | ✅ this change |
| — | **part B — io_uring submission** | ⬜ **not started; its premise changed, see below** |

### The payoff, measured two ways

| Measurement | Before | After |
| --- | --- | --- |
| controlled (same build, only `db.c`/`vm.c` swapped; `wmix 4000 32`) | 2213 · 2177 ops/s, p50 7183 · 7251 µs | **6216 · 6525 ops/s, p50 3458 · 3444 µs** |
| committed baseline: `s1` inline vs `sN` batched | 1467 ops/s, mean batch 1.0 | **5117 ops/s, mean batch 5.43, peak 57** |

**≈2.9× throughput, ≈2.1× lower p50**, and the two methods agree (2.9× and
3.5×). Batching scales with contention: mean batch **1.13 / 1.76 / 5.35** at
C = 4 / 16 / 64.

### The cost side, and a bug the battery caught

**Reads were being held behind the barrier.** The drain first held *every* DB
reply until the commit — including reads, which stage nothing. `mixread` p99 rose
from ~1043 µs to **4057 µs** until only staging statements had their replies
held. Caught by the gate, not by review.

**What remains is inherent:** a barrier blocks the owner shard longer (more
records per fsync) though less often, so anything queued behind one waits. Three
full runs of the same build gave `durable.sN.mixread.p99` of **1043 / 2318 /
4147 µs** — a 2–4× spread near idle. So part A buys ~3× write throughput at the
cost of a longer, noisier tail on the owner shard. `durable.sN.*.p99us` was
re-baselined at 100% tolerance for that reason, with the floor as the real guard
(`mixread`'s came within 25 µs of tripping).

**This is the strongest argument for part B** — submitting the barrier and
continuing to serve is exactly what removes this cost.

### What did NOT improve — and it was predicted

- **`durable.sN.mixwrite`: 480 → 492 ops/s, i.e. unchanged.** This was the
  spec's *original* payoff metric, and correcting it was part of the brainstorm:
  `mix` writes on one op in ten with C=4, so a quick run performs **20 writes**
  and measured mean batch **1.01**. A workload that never has two writes in
  flight cannot be helped by batching them.
- **`durable.*.seed`: unchanged.** A serial single writer has nothing to batch
  with, under any scheme.
- **This board's stated target was mis-stated.** It read "close the 66× gap
  iteration 22 measured (durable 4.5k vs ram 297k inserts/s)". Part A does not
  close that gap and structurally cannot: `seed` is serial, and one writer
  waiting on one barrier is a **latency** problem, not a batching one. Recorded
  rather than quietly renumbered.
- **The before-p99 is not a measurement.** `hist_add` clamps at 20000 µs and
  both before-runs pinned exactly there, so the true value is ≥20 ms and
  unknown. The gain is *at least* 2.3×.

## Goals

- **Replace fsync-per-commit with io_uring group-commit** on the WAL write
  path: batch a tick's committed records into one submission, let the kernel
  overlap the write and the durability barrier, and acknowledge each writer
  only after the barrier its record rode has completed — the same
  ack-after-durable contract, at a fraction of the syscall cost.
- **Overlap durability with work.** With the shard-actor runtime
  (iteration 8) the shard thread submits its batch and keeps executing ready
  statements while the ring drains, instead of blocking one thread on one
  fdatasync — the multithreading the throughput number has been waiting for.
- **Keep the durability promise byte-for-byte.** Every guarantee iterations 9
  and 22 proved — replay-whole-or-not-at-all, torn-tail drop, no
  acknowledged write ever lost — holds identically; io_uring changes HOW the
  bytes reach the platter, never WHETHER an ack means durable.

## Acceptance Criteria

Met:

- **Given** the io_uring write path under iteration 22's crash battery, **when**
  it runs, **then** every acknowledged write is present after replay. ✅ — the
  criterion applies unchanged to part A's batching. `crash.sN` (the batched
  path) recovered every acked row after `kill -9`, `crash.s1` likewise, and both
  restart legs replay byte-true. This was the one thing batching could break.
- **Given** the durable write benchmark before and after, **then** throughput is
  materially higher and p99 lower, recorded. ✅ ~2.9× and ~2.1× (p50); see
  `perf-targets.md` §6. **Scoped honestly:** on a write-concurrent workload
  only, and p99's "before" is at the histogram ceiling.
- **Given** batching, **when** it runs, **then** it is proven to engage rather
  than assumed. ✅ mean batch 5.43, peak 57 on the gated leg, and the live
  assertion fails the suite if the mean drops to 1.
- **Given** a durability failure, **when** it happens, **then** the engine does
  not continue with RAM ahead of disk. ✅ fatal, diagnosed, exit 74 — replacing
  three behaviours that disagreed.

Outstanding:

- **Given** a kernel without io_uring, **when** the runtime starts, **then** it
  falls back automatically. *(part B — part A adds no syscall interface, so
  nothing to fall back from yet.)*
- **Single-shard concurrent batching.** A statement on shard 0 commits inline
  and cannot batch; doing so needs the inline path to park its fiber on the
  barrier — the same machinery part B needs. So `WO_SHARDS=1` gets no batching
  at all, by design and measured (mean batch 1.0).
- **The abort path is not exercised.** Forcing a real `fdatasync` failure needs a
  full or read-only filesystem, which the gate cannot arrange without mount
  privileges. The unit test proves the error is *detected*; the exit three lines
  later is covered by inspection. Disclosed rather than papered over — iteration
  40 was exactly a fatal path nothing exercised.

## Part B — its premise changed

Part B was justified by "close the 66× durable gap". Part A shows that framing
was wrong: the gap is **two** problems. Concurrent write fan-in was a batching
problem and is now ~3× better. What remains is a **serial** writer waiting on a
single barrier, which no amount of batching can help — and io_uring does not
obviously help it either, since one writer still needs one durable barrier
before its ack. Part B's real candidates are overlapping the barrier with other
work on the shard, and the inline-path park that single-shard batching also
needs. **It should be re-brainstormed against that, not started on the old
premise.**

## Out Of Scope

- **io_uring for the network/accept path.** This iteration is the WAL write
  path only; the socket side is the shard-actor runtime's and the network
  layer's concern.
- **io_uring for reads.** RAM is authoritative — reads never touch a
  descriptor (phase-B doctrine), so there is nothing to accelerate on the
  read path. This is a write-durability optimization, full stop.
- **Registered buffers / fixed files / SQPOLL tuning** beyond what the
  benchmark shows is worth it. Start with the plain submit/complete model;
  add ring features only when 22's number says a specific one pays.
- **Replacing the WAL format or the commit contract.** The bytes on disk and
  the meaning of an ack are iteration 9's; this changes the syscall, not the
  format.

## Info

Forks the spec must settle:

**1. How much of the ring model, and behind what abstraction?** The write
path today is `pwrite` + `fdatasync` in `database/src/wal.c`; io_uring adds a
submission/completion queue and a durability barrier op
(`IORING_OP_FSYNC`/`IORING_FSYNC_DATASYNC` or `O_DSYNC` writes). The fork:
wrap it behind the existing `wo_wal_commit` boundary (drop-in, the engine
never learns) or expose an async-commit primitive the shard scheduler drives
(faster overlap, but couples the WAL to iteration 8's loop). Leaning:
drop-in behind `wo_wal_commit` first — it is the correctness-preserving
step and 22 can measure it standalone — then an async variant only if 8's
scheduler shows the blocking boundary is the remaining bottleneck.

**2. liburing or raw syscalls?** liburing is the ergonomic wrapper but is a
new external dependency, against the libc-only doctrine; the raw
`io_uring_setup`/`io_uring_enter` syscalls are a few hundred lines and keep
the doctrine. Leaning: raw syscalls (the doctrine is load-bearing and this is
a bounded surface), with the mmap'd ring setup written down in the binding
doc the way the WAL format is — normative, versioned.

**3. What is the batch boundary?** Per-statement commit (today) is the
simplest correct thing and the slowest; a group commit needs a boundary — a
tick (iteration 8's scheduler quantum), a count, or a short time window.
Leaning: the shard tick once iteration 8 lands (a batch is "everything
committed this tick"), with a single-writer fallback that batches whatever
accumulated between one `wo_wal_commit` call and the ring draining.

**4. How is the fallback chosen and tested?** A kernel probe at startup
(attempt `io_uring_setup`, fall back on ENOSYS/EPERM) is the mechanism; the
question is how CI proves BOTH paths without two kernels. Leaning: an
environment override (`WO_WAL_MODE=fsync|uring`) so the test matrix runs the
crash battery and the benchmark on both on any capable machine, and the
auto-probe is what production uses.

## Proposed Solution

- **Brainstorm the spec** after iterations 8 and 22 exist — this iteration is
  meaningless without a multithreaded runtime to overlap against and a
  measured baseline to beat, and its plan's acceptance is literally "22's
  durable number improved, 22's crash battery still green, fsync fallback
  still correct".
- Expected shape: a `wo_wal` write-mode switch (fsync vs uring), the raw ring
  setup + submit/complete in `database/src/wal.c` (or a `wal_uring.c`
  beside it), the startup probe + `WO_WAL_MODE` override, the binding doc's
  WAL section extended with the ring layout, and iteration 22 re-run on both
  paths with the delta committed.
