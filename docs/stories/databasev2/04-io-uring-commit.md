---
track: databasev2
iteration: "4"
was_language_iteration: "23"
status: in-progress
readiness: refine
review_pending: "part B forks 1–5 and 8–10 decided under autonomy 2026-09-10 by codd-shoney; forks 6 (lintor) and 7 (cyril's tmpfs ceiling) are open and keep readiness at refine"
chain: 5
---

# databasev2 4 — group commit, and the async barrier (was: io_uring group-commit write path)

> **Moved 2026-08-26** from the language track, where this was iteration 23.
> Part of [Story — databasev2: the database beyond RAM](00-story.md). Content unchanged by
> the move; its dependencies are restated in that track index.

> Format: fiberloom `product/story-iteration-template`. Part of
> [Story — one language, one runtime, one database, one binary](../language-runtime-database/00-story.md)
> — the track this iteration was authored in before the 2026-08-26 move.
>
> **Inserted 2026-08-15.** The write-path optimization, and deliberately the
> LAST database performance iteration: it only earns its complexity once
> there is a measured fsync-per-commit baseline to beat (iteration 22) and a
> multithreaded runtime to overlap against (iteration 8). Doing it earlier
> would optimize a number nobody had measured, against a runtime that
> couldn't use it.
>
> **Supporting evidence for staying last (iteration 1, 2026-08-27):** the write
> path is *not* where memory pressure bites. Inserting 900 000 rows inside a
> 64 MiB cap with swap cost **~1%** (148 s vs 150 s uncapped), because appending
> never re-touches its cold pages. Random *reads* over the same oversized table
> cost **273×**. So the pressure is on the read path, and the io_uring question
> that may actually matter is the one iteration 2 deferred here — io_uring for
> `resident: keys` row reads — not group-commit for writes.
>
> **REFINED 2026-08-20** (developer confirmation, no code): the four original
> forks were settled as their leanings — drop-in behind `wo_wal_commit` first;
> raw `io_uring_setup`/`io_uring_enter`, libc only; the batch boundary the shard
> tick; a startup probe plus the arc-wide `WO_IO=uring|epoll` override. Position
> re-sequenced 2026-08-21 to fifth in the concurrency chain; the per-shard ring
> already exists (arc T4, `WO_IO=uring|epoll`) and the developer's io_uring-first
> directive of 2026-08-20 says the WAL's durability ops ride the SAME per-shard
> ring the fibers park on — one event loop per shard. Those settlements are
> recorded in *Info* below under "the 2026-08-15 forks, for the record"; the
> tick boundary was later replaced by queue-drain (part A) and "WRITE+FSYNC
> chains" by a single FSYNC (part B, fork 2).

> **BRAINSTORMED 2026-08-28 — and SPLIT IN TWO.** Spec for part A:
> [`2026-08-28-wal-group-commit-design.md`](../../superpowers/specs/2026-08-28-wal-group-commit-design.md)
> · plan: [`2026-08-28-wal-group-commit.md`](../../superpowers/plans/2026-08-28-wal-group-commit.md)
> (6 tasks).
>
> **The premise needed correcting.** This story said "replace fsync-per-commit
> with io_uring group-commit", but the engine committed per **statement**: `db.c`
> called `wo_wal_commit` immediately after every append, at all six sites. That
> split the goal into two independent wins, and only the second needs io_uring:
>
> - **Part A — batching.** Let many statements share one barrier. Landed
>   2026-08-28, ≈2.9× concurrent durable write throughput.
> - **Part B — the async barrier.** The shard submits the barrier and keeps
>   working instead of blocking in `fdatasync`. **Re-brainstormed 2026-09-10 on
>   the corrected premise below.**
>
> **Forks settled in the 2026-08-28 brainstorm:** batch boundary is
> **queue-drain** (not the tick — a tick taxes an idle system to serve a busy
> one); a failure between "RAM mutated" and "record durable" is a **fatal,
> diagnosed abort**, which **removed `WO_T_IO` from the write path** — a
> language-visible change, recorded here deliberately.
>
> **RE-BRAINSTORMED 2026-09-10 (part B, codd-shoney, autonomy).** The board's
> original target for part B — "close the 66× gap" — was wrong, and the story's
> own part-A closeout said so. What part A left behind is a **latency tail on
> shard 0**: everything queued behind a barrier waits for the device, and
> `durable.sN.mixread.p99` measured **1043 / 2318 / 4147 µs** across three
> identical runs. Part B is now FOR that tail, and nothing else. Ten forks,
> eight settled from code evidence, two open with the measurement and the
> kernel question that settle them named — see *Info — the forks, settled*.
> `readiness: refine` until those two return.

## Progress — part A landed 2026-08-28

| # | Task | State |
| --- | --- | --- |
| 1 | a failed barrier is detected, and fatal | ✅ `d3ff03e` |
| 2 | one barrier per drain; replies held | ✅ `b9b8a45` |
| 3 | the inline path takes the fatal rule, asymmetry documented | ✅ `a6ccdbe` |
| 4 | prove batches form — the `wmix` write-concurrent leg | ✅ `40d029c` |
| 5 | measure the payoff, gate it, record it | ✅ `d52ea8a` |
| 6 | closeout | ✅ this change |

## Progress — part B, the async barrier (brainstormed 2026-09-10)

Sizes: S ≤ half a day, M ≤ two days, L longer. B1 and B2 gate everything after
them; nothing below B2 starts until both have reported.

| # | Task | Size | State |
| --- | --- | --- | --- |
| B1 | **the ceiling measurement** (codd-cyril): `mix` at default shards, same build, `WO_DATA` on tmpfs vs on `bench/` (ext4), three runs each; report `mixread` ops/s, p50, p99 — settles fork 7 | S | ⬜ |
| B2 | **the kernel questions** (lintor): floor and shape of a bare `IORING_OP_FSYNC` on the 5.4 ring, io-wq behaviour on the floor kernel, teardown with a barrier in flight, how to detect the op at boot — settles fork 6 | S | ⬜ |
| B3 | the park-plane seam (owner: the runtime agent, not codd): submit one FSYNC SQE with a WAL sentinel, a non-blocking reap, a completion hook; the reap loop matches the sentinel before its fiber cast | S | ⬜ |
| B4 | `wal.c`: split the write from the barrier — the drain's `pwrite` advances `off` as today, the barrier becomes submit-or-queue with an in-flight mark and a synced-bytes counter; boot self-test of the op with the loud fallback notice; stats line names the path | M | ⬜ |
| B5 | `vm.c` drain: pwrite, then submit or queue behind the in-flight barrier; the two held-reply lists move from drain locals into shard state; the completion handler releases ITS list, then runs the compaction check; the hot path polls completions where it polls the inbox | M | ⬜ |
| B6 | the inline unification: a durable-table write on shard 0 takes the request path through its own inbox; reads and volatile writes stay inline; guarded by `durable.s1.seed.p50us` staying inside its 15% tolerance | M | ⬜ |
| B7 | shutdown: shard 0 reaps an in-flight barrier to completion (fatal rule applies) before `wo_wal_close`; never closes the fd with a barrier outstanding | S | ⬜ |
| B8 | gates and numbers (codd-cyril): durable legs and the crash battery under BOTH `WO_IO=uring` and `WO_IO=epoll`; a `test_wal` case that submits FSYNC on a bad fd and sees the failure detected; baseline re-run; `perf-targets.md` §6 addendum with the before/after against the B1 ceiling; re-tighten the 300% tolerance if the tail stabilises | M | ⬜ |
| B9 | contracts and closeout: `CODE-LOGIC.md` "Group commit" section extended with the async barrier, `04-db-binding.md` commit-contract paragraph, story/board/graph (codd + codd-pm) | S | ⬜ |

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
re-baselined at 300% tolerance for that reason, with the floor as the real guard
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

- **One barrier per drain, not per statement** (part A, met). Many statements
  share one `pwrite` + `fdatasync`; each writer is acknowledged only after the
  barrier that carried its record. The batch boundary is the queue going
  empty — no tick, no timer, nothing to tune.
- **Shard 0 never blocks in the barrier** (part B, rewritten 2026-09-10). The
  drain writes its batch into the page cache and submits the durability barrier
  to the shard's own ring, then goes back to serving. Reads arriving while the
  device flushes are answered before the flush completes; the next drain's
  writes are written and queued behind the in-flight barrier. The metric is the
  read tail on the owner shard: `durable.sN.mixread.p99us` (baseline 4050 µs;
  measured 1043 / 2318 / 4147 µs) and `durable.sN.mixread.ops_sec` (4733),
  against the no-barrier reference `ram.sN.mixread` (p99 81 µs, 44 874 ops/s)
  and the tmpfs ceiling B1 measures. Write throughput may rise as a side effect
  of the disk never idling between barriers (`durable.sN.wmix.ops_sec`, 6017);
  it is watched, not targeted.
- **One write path, not two.** A durable-table write on shard 0 takes the same
  request path a worker's does, so the inline path's private barrier — the
  asymmetry part A documented as a standing hazard — goes away, and
  `WO_SHARDS=1` gets batching and the async barrier with it.
- **Keep the durability promise byte-for-byte.** Every guarantee iterations 9
  and 22 proved — replay-whole-or-not-at-all, torn-tail drop, no acknowledged
  write ever lost, durable-or-process-death — holds identically. Part B changes
  WHEN shard 0 learns the barrier finished, never WHETHER an ack means durable.

## Acceptance Criteria

Met:

- **Given** the batched write path under iteration 22's crash battery, **when**
  it runs, **then** every acknowledged write is present after replay. ✅ —
  `crash.sN` (the batched path) recovered every acked row after `kill -9`,
  `crash.s1` likewise, and both restart legs replay byte-true. This was the one
  thing batching could break.
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

Outstanding (part B; rewritten 2026-09-10):

- **Given** the ceiling measurement B1, **when** `mix` runs on tmpfs (a free
  barrier) against ext4 on the same build, **then** the gap between the two
  `mixread` figures is recorded and is the go/no-go for everything below. If
  tmpfs `mixread.p99` is not materially below the ext4 figure, the tail is not
  the barrier and part B closes as "measured, not built" (fork 7).
- **Given** a barrier in flight on shard 0, **when** worker reads arrive,
  **then** they are answered before it completes: `durable.sN.mixread.p99us`
  within 2× of the B1 tmpfs figure across three runs, and
  `durable.sN.mixread.ops_sec` at least half of it; `durable.*.seed` and every
  `durable.s1.*` leg inside their 15% tolerance.
- **Given** the async path, **when** a writer's reply is released, **then** the
  barrier that carried its record has completed: the crash battery recovers
  every acked row after `kill -9` under `WO_IO=uring`, and again under
  `WO_IO=epoll`.
- **Given** `WO_IO=epoll`, or a ring that cannot run the op, **when** the
  runtime starts, **then** the drain commits synchronously exactly as part A
  does, one stderr line says so when the fall-back was not forced, and
  `WO_WAL_STATS=1` names the path a run took.
- **Given** `WO_SHARDS=1` with concurrent writers, **when** `wmix` runs,
  **then** batches form (`durable.s1.wmix.mean_batch` above 1.0, today exactly
  1.0) — the inline unification's proof.
- **Given** a completion that reports failure, **when** the handler sees it,
  **then** the process ends with the same diagnostic and exit 74 that part A
  gives a failed `fdatasync`. The unit test proves the failure is *detected*
  (an FSYNC submitted on a bad descriptor completes with an error); the exit
  stays covered by inspection, disclosed exactly as part A disclosed it.
- **Given** the process exits with a barrier in flight, **when** shard 0 tears
  down, **then** it waits for the completion first; the stats line's
  submitted and completed barrier counts agree at exit.

## Part B — its premise changed (2026-08-28), and what replaced it (2026-09-10)

Part B was justified by "close the 66× durable gap". Part A showed that framing
was wrong: the gap is **two** problems. Concurrent write fan-in was a batching
problem and is now ~3× better. What remains for a **serial** writer is a device
flush it must wait for before its ack — no submission model shortens that, and
acknowledging before the flush is the one thing principle 7 forbids. The tail
that IS addressable is shard 0's: while it sits in `fdatasync`, every read and
every write queued in its inbox waits for the device. The forks below settle
part B against that, and only that.

## Out Of Scope

- **io_uring for the network/accept path.** This iteration is the WAL write
  path only; the socket side is the shard-actor runtime's and the network
  layer's concern.
- **io_uring for reads.** For a fully-resident table reads never touch a
  descriptor. A table declaring `resident: keys` ([databasev2 2](02-table-storage-modes.md))
  *does* `pread` rows from the log, and accelerating that is a real, separate
  question — not this iteration's, whose acceptance is a durability tail.
- **Async commit** — acknowledging a writer before its barrier completes,
  PostgreSQL's `synchronous_commit = off` / `XLogSetAsyncXactLSN`. Refused:
  principle 7, "an ack means the commit reached disk … none of it is
  negotiable". It is the only thing that would help a serial writer's latency,
  and it is not on offer.
- **A linked WRITE→FSYNC SQE chain, registered buffers, fixed files, SQPOLL,
  `sync_file_range`.** Fork 2 explains why the chain buys nothing here; the
  rest are ring features to add only when a measurement names one.
- **Replacing the WAL format or the commit contract.** The bytes on disk and
  the meaning of an ack are iteration 9's; this changes when shard 0 learns the
  barrier finished, not what the log contains.
- **`transaction { }`** (language 18, hold). A transaction is a staged batch;
  under part B it is one drain's write and one barrier. The two compose without
  either knowing the other.

## Info — the forks, settled

**Part B — ten forks, 2026-09-10** (codd-shoney, under autonomy; the
frontmatter's `review_pending` asks for the developer's second look at the eight
decided ones, and names the two that keep readiness at `refine`).

1. **What part B is FOR: the read tail on shard 0 — and only that.** Three
   candidates were on the table. (a) The tail: reads and writes queued in shard
   0's inbox wait while the drain blocks in `fdatasync` (`runtime/src/vm.c`
   233–241 calls `wo_wal_commit_fatal`, which is `pwrite` + `fdatasync` at
   `database/src/wal.c` 965–981); measured `durable.sN.mixread.p99us` 1043 /
   2318 / 4147 µs, baseline 4050, where the same read path with no barrier in
   the way (`ram.sN.mixread`) sits at 81 µs and 44 874 ops/s against 4733. (b)
   Serial-writer latency (`durable.s1.seed` p50 212 µs): refused — one writer
   needs one completed device flush before its ack, and the only mechanism
   that shortens the wait is acknowledging early, which principle 7 forbids;
   io_uring moves the wait off the thread, it does not shorten it. (c)
   Throughput: part A already took the batching win; pipelining (the next
   drain writes while the previous barrier flushes) may raise
   `durable.sN.wmix.ops_sec` from 6017 because the disk stops idling during
   the drain's execution phase — recorded as a watched side effect, not the
   goal, because a goal needs one number and (a) has it. **Decision: (a).** The
   metric is `durable.sN.mixread.p99us` and `.ops_sec`; the bar is fork 7's.
   Evidence that the tail is shard 0's and not the requester's: reads are
   already released before the barrier (`vm.c` 207–214), so a read waits only
   when it ARRIVES during one — which is exactly what an async barrier ends.

2. **Mechanism: the drain `pwrite`s as today, then submits ONE bare
   `IORING_OP_FSYNC` (`IORING_FSYNC_DATASYNC`) on shard 0's existing ring; at
   most one barrier in flight; a drain that finds one in flight writes its bytes
   and queues behind it; the completion submits the queued one.** Three options
   were weighed. (i) A linked `IORING_OP_WRITE` → `IORING_OP_FSYNC` chain — the
   2026-08-20 wording and the linux card's advice
   (`docs/plan/exploration/linux/07-io_uring.md`, "essential for WAL"). It needs
   the staging buffer to stay stable until the WRITE completes (a second
   buffer, and every reader of `[off, off+len)` — `scan_record_staged` at
   `wal.c` 311–330, `wo_wal_next_offset` at `wal.h` 222–242 — would have to
   learn a second not-yet-visible region), two CQEs to check with a
   short-write rule (`io_uring/rw.c` 550–560 fails the request on a short
   write; `io_uring/io_uring.c` 1841–1848 cancels the link), and it buys no
   asynchrony the plain write lacks: a buffered write to a regular file is
   punted to an io-wq worker anyway unless the filesystem advertises
   `FOP_BUFFER_WASYNC` (`rw.c` 1146–1155). The card was written for a Rust-era
   buffer model that no longer exists; the "same ring" half of the directive
   stands, the "chain" half does not. (ii) A dedicated fsync fiber or thread of
   ours — a thread plus a second wake mechanism, against codd's "no locks,
   single-threaded by contract"; and redundant, because the kernel already runs
   the op on a worker thread it owns: `io_fsync_prep` sets `REQ_F_FORCE_ASYNC`
   and `io_fsync` warns if ever called non-blocking (`io_uring/sync.c` 69, 79).
   (iii) Moving the barrier off shard 0 entirely — there is no other owner of
   the WAL (principle 5; workers assert they hold neither `db` nor `wal`,
   `vm.c` 2362). **Decision: the bare FSYNC.** `pwrite` returning means the
   bytes are in the page cache, so every existing offset invariant holds
   unchanged — `off` advances at the write exactly as it does at `wal.c` 978,
   the staged region `[off, off+len)` exists only inside a drain, `pread` of any
   written offset returns data — and the WAL stays the only truth. What is
   new is bookkeeping about the file: an in-flight mark and a synced-bytes
   counter, both used only by compaction (fork 5) and the stats line. **Cost:**
   ~30 lines in `park.c` (a submit with a WAL sentinel — `uring_submit` is
   static at `park.c` 136; a non-blocking reap; a completion hook; and the reap
   loop's `else` branch at `park.c` 392–397 casts any unknown `user_data` to a
   fiber pointer, so the sentinel MUST be matched before it) — that seam is the
   park plane's, not codd's, and its owner is named in B3. PostgreSQL's shape,
   ported as behaviour: a backend that finds the flush lock held waits for the
   holder and re-checks whether its record was flushed for it (`xlog.c`
   2875–2889) — our "queue behind the in-flight barrier" is that, with the drain
   as the only backend. PG's `commit_delay` (`xlog.c` 2902–2906) is a timer
   knob and stays rejected for the reason part A rejected the tick.

3. **The ack contract across an async completion.** Today the held replies are
   drain locals — "nothing here needs to outlive the batch it describes"
   (`vm.c` 98–101). Under part B they must: **two FIFO lists in shard state,
   in-flight and next; each list is released by ITS barrier's completion, never
   earlier.** A completion with `res` below zero takes `wal_die` with
   "fdatasync" and `-res` as the errno — the same text and exit 74
   (`WO_EXIT_DURABILITY`, `wal.h` 304) part A gives a failed `fdatasync`; a
   `pwrite` failure stays synchronous at the drain and takes the existing
   "pwrite" arm (`wal.c` 1016). "Durable or process death" is unchanged. What
   widens is the WINDOW between RAM-mutated and durable, and it is the same
   exposure part A already accepted: a read arriving after a write in the same
   drain sees the write's effect and is answered before the barrier (`vm.c`
   207–214), so a process death loses nothing acknowledged and may have shown
   a reader an unacknowledged state — today's behaviour, longer. Rejected: a
   per-batch sequence number stamped on each reply (a third piece of state to
   keep consistent; FIFO-in-order completion makes two lists sufficient).

4. **The epoll fallback is part A, unchanged: synchronous `pwrite` +
   `fdatasync` in the drain.** No helper thread — a thread of ours doing
   `fdatasync` and signalling an eventfd would be a second mechanism to keep
   correct for the portability path, and principle 9 says the portability path
   is not where sharpness goes. The contract is identical on both paths; only
   where shard 0 waits differs. **Two consequences, both gated:** (a)
   `scripts/db-bench.py` never sets `WO_IO` today — its durable legs and the
   crash battery (lines 257–274) run whatever the probe picks — so cyril adds
   `WO_IO=uring` and `WO_IO=epoll` legs to the restart proof and the crash
   battery, the pattern `scripts/db-actor-accept.sh` 49–50 already uses; (b)
   the `WO_WAL_STATS=1` line (`runtime/src/main.c` 175–179) gains the barrier
   path and the submitted/completed counts, so no measurement can be
   misattributed. The baseline keeps the probe's default (uring on this box).

5. **Ordering with compaction, and the deferred drops and re-points.**
   Compaction renames a new file over the log and re-opens the descriptor
   (`wal.c` 1317–1327); with an FSYNC in flight on the OLD descriptor, its
   completion would certify an unlinked inode. **Decision: compaction runs only
   when staging is empty AND no barrier is in flight** — the refusal at `wal.c`
   1214 gains the second condition as its backstop, and the drain's compaction
   check (`vm.c` 250–267) moves into the completion handler, after that batch's
   replies are released, keeping today's order. Compaction itself stays
   synchronous; it is rare and measured at 2.7 ms. The keys-resident drops and
   re-points were deferred to "after the commit" because "the record is still
   in the staging buffer, so its offset would pread zeros" (`wal.h` 96–98,
   `wo_db_flush_drops` at `wal.c` 451–461); the reason is the staging buffer,
   not durability, and the drain's `pwrite` ends it. **Decision: flush drops
   and re-points right after the drain's write**, keeping one pending list and
   no per-batch split; a later barrier failure kills the process, so nothing
   observable depends on the difference. zack rewords the `wal.h` 339 comment
   ("Call ONLY after a commit has succeeded") to name the real precondition —
   written, not synced.

6. **Kernel floor and raw-syscall shape — OPEN, for the `lintor` agent, not
   answered from memory.** The reference tree is Linux 7.0
   (`.dev/reference/linux/Makefile`); the ring's floor is 5.4 (`park.h` 13,
   "ops restricted to the TIMEOUT floor"). Questions lintor settles, each with
   the kernel path it read: (a) the floor of `IORING_OP_FSYNC` with
   `IORING_FSYNC_DATASYNC` (`include/uapi/linux/io_uring.h` 257, 340) and
   whether a bare FSYNC SQE needs any `IOSQE_*` flag; (b) whether `park.c`'s
   hand-mirrored `io_uring_sqe` already covers the `fsync_flags` union member
   (it mirrors `poll32_events` at the same offset) or the mirror must grow;
   (c) how the 5.4-era io-wq executes a forced-async op — a kernel thread per
   ring, visible where, any `RLIMIT_NPROC` or cgroup consequence — versus
   `IORING_FEAT_NATIVE_WORKERS` kernels; (d) what happens to an in-flight FSYNC
   at ring teardown and at `close(fd)` (does exit wait for it; does the op
   hold its own file reference); (e) how to know at boot that the op is
   supported without `IORING_REGISTER_PROBE` (5.6) — the candidate is a
   self-test: one real FSYNC on the freshly opened, preallocated WAL at first
   use, judged by its `res`; (f) confirm that `fdatasync` from a worker while
   the issuing thread `pwrite`s the same file has no ordering hazard beyond
   "a later write may or may not be covered", which fork 2 already assumes.
   What is settled here regardless of the answers: forced `WO_IO=uring` with
   the op unavailable refuses at boot (mirrors `park.c` 187–189, forced and
   absent is fatal); unforced falls back to the synchronous path with one
   stderr notice, never silently.

7. **Go/no-go — OPEN, settled by one measurement, then one developer answer.**
   The measurement (B1, codd-cyril): the `mix` leg at default shards on the
   same build, `WO_DATA` on tmpfs (where `fdatasync` is free — the board
   recorded 195 000 vs 2200 ops/s for `wmix`, `00-status.md` 1010–1014) and on
   `bench/` (ext4), three runs each. tmpfs is the ceiling for part B: a barrier
   that costs shard 0 no wall time. Reading it: if tmpfs `mixread.p99` lands
   near `ram.sN.mixread`'s 81 µs and ops/s near its 44 874, the barrier is the
   whole tail and the bar is set from that figure (within 2× on p99, at least
   half on ops/s, three ext4 runs, `seed` and `s1` legs inside 15%). If tmpfs
   `mixread.p99` stays in the thousands of microseconds, the tail is not the
   barrier — the drain's execution phase or scheduling — and part B closes as
   "measured, not built", its Progress table recording the ceiling. The
   developer answer, after the number: is a 1–4 ms read p99 behind a write, with
   the 2–4× run-to-run spread that forced the gate to 300%, acceptable for the
   north-star app (a porch service that fits RAM)? If yes, the do-nothing
   option is taken with the measurement on record and the gate tolerance kept.
   Nothing after B2 starts before both answers exist.

8. **The inline path while a barrier is in flight: unify it.** A statement on
   shard 0 stages and commits its own barrier (`database/src/db.c` 56–84,
   100–115, 133–137; routed at `runtime/src/builtin.c` 188–189), and the
   `mix` leg puts one of its four mixers on shard 0 (round-robin placement,
   `vm.c` 1101), so its writes block the drain today and would still block it
   under an async drain. Options: (a) keep the inline barrier synchronous — a
   second `fdatasync` beside the in-flight one is harmless, but the tail comes
   back for every shard-0 write and `WO_SHARDS=1` gets nothing; (b) route a
   durable-table write on shard 0 through the request path — `wo_db_rpc` pushes
   to shard 0's own inbox (`inbox_push_to`, `vm.c` 59–73; the primary inbox
   exists at every shard count, `vm.c` 574–577 and `main.c` 234), the fiber
   parks on `WO_PARK_INBOX`, the drain executes it, holds its reply, and the
   completion releases it — one write path, and `WO_SHARDS=1` gets batching
   and the async barrier for free; (c) stage inline and park the fiber with a
   done-flag of its own — the request path minus the marshal, a third path.
   **Decision: (b).** It deletes the asymmetry `CODE-LOGIC.md` calls a standing
   hazard ("if that ever stops holding, the inline path would make another
   statement's record durable early and acknowledge it to the wrong writer")
   and the `maybe_compact` copy at `db.c` 32. Reads and `@table(durable:
   false)` writes stay inline — `ram.s1.seed` is 4 µs per op and two inbox
   hops would double it. **Guard:** the hops cost `durable.s1.seed.p50us`
   (212 µs, 15% tolerance) at most a few microseconds against a ~200 µs flush;
   if the gate says otherwise, (a) is the fallback and this fork reopens. The
   route condition needs `table_is_durable` visible to `builtin.c`; the codd
   doctrine that traps stay byte-identical between the two paths is what makes
   the unification safe to do.

9. **Completion delivery on a busy shard 0.** The reap loop lives in
   `wo_io_wait`, which runs only when nothing is runnable (`park.c` 330–400);
   a completed barrier's replies would otherwise wait for shard 0 to go idle.
   **Decision: poll the completion queue, non-blocking, at the two points the
   inbox is already polled** — `NEXT_RUNNABLE` (`vm.c` 1697) and the
   reduction-slice check (`vm.c` 1734–1736) — plus the sentinel branch in
   `wo_io_wait`. Two acquire-loads of the mmap'd head and tail per slice; the
   bound is one reduction budget, the same bound the DB RPC already carries
   ("a busy shard adopts its inbox once per reduction slice",
   `runtime/src/CODE-LOGIC.md`, "The transparent DB actor"). Rejected: routing
   the completion through the wake eventfd — the CQE already is the event.

10. **Shutdown with a barrier in flight.** `wo_engine_stop` (`vm.c` 713–770)
    joins workers and discards queued envelopes; it knows nothing of the WAL,
    and `wo_wal_close` (`wal.c` 463–474) closes the descriptor unconditionally.
    **Decision: before `wo_wal_close`, shard 0 reaps an outstanding barrier to
    completion with a blocking `io_uring_enter(GETEVENTS)`, applies the fatal
    rule to its result, then releases the held replies into inboxes the
    teardown discards.** One wait, no new mechanism; the invariant it buys is
    that exit 0 means every submitted barrier completed, which the stats
    line's two counts make checkable. A parked requester woken by STOP
    re-executes `wo_db_rpc` and re-sends (`park.c` 341–352, `vm.c` 309–357) —
    pre-existing behaviour for a request in flight at stop, noted, not part
    B's to change.

**Knob, dependency, mode check, applied to all ten:** no new environment
variable (`WO_IO` is the arc's), no library, no thread of ours, no rollback
path, no second copy of any record; the one behavioural difference between
backends is where shard 0 waits, and the stats line and the boot notice name
it.

**The 2026-08-15 forks, for the record** (superseded, kept so the trail reads):
(1) drop-in behind `wo_wal_commit` first, async only if the boundary proved the
bottleneck — part A was the drop-in; the boundary proved to be the tail above.
(2) liburing or raw syscalls — raw, and already landed for the ring in
`park.c` (arc T4); part B adds one op to it. (3) the batch boundary — the tick
was replaced by queue-drain in the 2026-08-28 brainstorm. (4) fallback and CI —
the startup probe plus `WO_IO=uring|epoll`, landed with the arc; fork 4 above
says what part B owes it.

## Proposed Solution

Sequence: B1 and B2 first, in parallel, both S; a GO on B1 and lintor's answers
on B2 unlock the rest. Then B3 (the park-plane seam, its owner assigned by the
developer since the plane is not codd's), B4 and B5 together (the WAL split and
the drain rewrite are one change to reason about), B6 (the inline unification,
measured against its guard), B7, then B8 and B9. Expected shape: no new file —
`wal.c` gains the submit-or-queue and the in-flight state, `vm.c`'s drain and
two poll sites change, `park.c` gains three small exports, `builtin.c` one route
condition. The contract paragraphs in `04-db-binding.md` and the "Group commit"
section of `CODE-LOGIC.md` say what an ack means under the async barrier — the
same thing, later observed — and the crash battery proves it under both
backends.

## History

**2026-09-10 — part B re-brainstormed by codd-shoney (autonomy).** The board's
"close the 66× gap" target was retired for good and the story re-aimed at the
shard-0 read tail. Ten forks enumerated; eight decided from code, kernel and
PostgreSQL evidence (file:line above); two left open — the kernel floor to the
`lintor` agent, the go/no-go to one tmpfs-vs-ext4 measurement and one developer
answer. Two findings worth keeping regardless of the outcome: the linux card's
"link WRITE→FSYNC, essential" advice does not apply to this engine's buffer
model (the kernel punts both ops to a worker anyway, and `pwrite` already keeps
every offset invariant); and the deferred keys-resident drops and re-points were
waiting on the staging buffer, not on durability. One inconsistency to hand to
codd: `CODE-LOGIC.md`'s "Group commit" section says the durability abort "exits
3" where `wal.h` 304 and this story say 74.
