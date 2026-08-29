# databasev2 4 part A — WAL group commit (implementation plan)

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps
> use checkbox (`- [ ]`) syntax for tracking.
>
> **Style rule (user convention):** concept, reason, and required
> behaviour in words plus verification commands only — no implementation
> or test code blocks; the executor writes the code.

**Goal:** one durability barrier per drain instead of one per statement, so a
writer is acknowledged after the barrier that carried its record rather than
after a barrier of its own.

**Architecture:** the barrier moves up, not out. Applying to RAM and staging the
record stay exactly where they are in `db.c`; the request path stops committing
after each append and instead holds its reply envelope, and shard 0 issues one
commit when it runs out of queued requests, then releases every held reply. Any
failure between "RAM mutated" and "record durable" ends the process with a
diagnostic.

**Tech Stack:** C11, libc only. `pwrite` + `fdatasync` (unchanged — io_uring is
part B). The existing per-shard envelope inbox carries the requests.

**Spec:** [`../specs/2026-08-28-wal-group-commit-design.md`](../specs/2026-08-28-wal-group-commit-design.md)

## Global Constraints

- **Durability is unchanged.** Every guarantee iterations 9 and 22 proved holds
  identically: replay-whole-or-not-at-all, torn-tail drop, no acknowledged
  write ever lost. This changes when the barrier runs, never what the log holds.
- **A writer is released only after the barrier carrying its record.** Never
  before, and never on the strength of a different batch's barrier.
- **libc only.** No new dependency, no new syscall interface in part A.
- **The payoff metric is `durable.sN.mixwrite`** (today 480 ops/s, p99
  5888 µs). `durable.s1.*` and both `seed` legs are regression guards, not
  targets — a serial writer and an all-inline shard have nothing to batch with.
- **`WO_T_IO` leaves the write path.** A commit or staging failure is fatal, not
  catchable. Exit 1 is a trap and exit 2 is a refusal, so this takes a third
  status of its own.
- Gates run through `just`. Never commit on `master`; branch first.

---

## Task 1 — a failed barrier is detected, and fatal

**Files:**
- Modify: `database/src/wal.c` (the commit routine's failure returns; a new
  fatal-commit entry point beside it), `database/src/wal.h` (declare it).
- Test: `runtime/test/test_wal.c` (a new case in the existing suite).

**Interfaces:**
- Produces: a commit entry point that takes the WAL and the number of records
  in the batch, commits, and on failure writes one stderr line naming the
  failing operation, the `errno` text, the WAL path and the record count, then
  exits with the durability-failure status. Tasks 2 and 3 call only this.
- Consumes: the existing staging buffer and commit routine.

- [ ] Read the commit routine first and confirm what it already reports: it
  loops `pwrite` until the staged buffer is written, then `fdatasync`, and
  returns non-zero on either failing. Confirm the WAL struct carries its path,
  or add it — the diagnostic is worthless without it.
- [ ] Test first, RED: assert the commit routine reports failure when the
  descriptor is unusable (a closed descriptor gives `EBADF`). This proves the
  error is *detected*; it does not exercise the exit.
- [ ] Verify RED for the right reason — the case must fail because the
  assertion is unmet, not because the suite does not compile.
- [ ] Add the fatal entry point. It must distinguish the two operations in its
  message: a `pwrite` failure and an `fdatasync` failure are different
  operational problems and the operator needs to know which.
- [ ] GREEN: `just wovm-test`. The new case passes and no existing case moves.
- [ ] **Disclosed gap, record it in the commit message:** the exit path itself
  is not exercised. Forcing a real `fdatasync` failure needs a full or
  read-only filesystem, which the gate cannot arrange without mount
  privileges. Do NOT add a fault-injection switch to buy coverage — shipping a
  binary that can be told to kill itself is the worse trade, and the spec
  rejected it.
- [ ] Commit.

## Task 2 — the barrier moves to the drain point; replies are held

**Files:**
- Modify: `database/src/db.c` (the request-path arms only — the three commit
  calls inside the marshaled-statement executor), `runtime/src/vm.c` (the
  envelope drain loop's DB-statement branch and the end of that loop).
- Test: no new fixture; the existing durability battery is the test. It already
  covers exactly what could break.

**Interfaces:**
- Consumes: Task 1's fatal commit entry point.
- Produces: the invariant later tasks measure — at most one barrier per drain,
  and every held reply released only after it.

- [ ] Read the drain loop's DB-statement branch first. Today it executes the
  request, marks it done, then immediately pushes a reply envelope that unparks
  the requester. Note that it runs on shard 0's thread, serialized — that is
  why no locking is needed anywhere in this task.
- [ ] Remove the three commit calls from the request-path executor in `db.c`.
  Leave applying to RAM and staging untouched, and leave the **inline** path's
  three commit calls alone — Task 3 owns that path and conflating them is how
  this change breaks the single-shard configuration.
- [ ] In the drain loop, collect reply envelopes in a local list instead of
  pushing them as each request finishes. A local is correct and deliberate:
  nothing needs to survive the loop, and per-shard state would outlive the
  batch it describes.
- [ ] At the end of the drain loop, if anything was staged, call Task 1's fatal
  commit once, then push every held reply.
- [ ] Handle the empty case: a drain that executed no DB statements must not
  commit and must not touch the staging buffer.
- [ ] Verify the ack contract has not moved: `just wovm-test` — the WAL and
  table suites must be unchanged, since neither knows about batching.
- [ ] Verify durability end to end: `just db-bench --quick`. The restart-replay
  and `kill -9` crash legs are the ones that matter — a kill between staging and
  the barrier must lose only unacknowledged writes. **If a crash leg fails here,
  stop; do not adjust the test.** That leg failing means the ack contract broke,
  which is the one thing this task may not do.
- [ ] Commit.

## Task 3 — the inline path keeps its own barrier, and says why

**Files:**
- Modify: `database/src/db.c` (the inline path's three commit calls — replace
  with Task 1's fatal entry point), plus the comment above them.

**Interfaces:**
- Consumes: Task 1's fatal commit entry point.
- Produces: nothing new. This task exists to make the asymmetry deliberate and
  legible rather than accidental.

- [ ] Replace the inline path's three commit calls with Task 1's fatal entry
  point, batch size one. Behaviour is unchanged — this is the fatal-failure
  rule reaching the second path, not batching.
- [ ] Write the comment that explains the asymmetry, because the next reader
  will otherwise "fix" it: the inline path cannot hold a reply, because it
  returns into its own fiber rather than unparking a requester. Batching it
  would require parking that fiber on the barrier, which is part B's machinery
  and deliberately out of part A.
- [ ] Confirm the ordering assumption holds: because the drain loop always
  commits before it ends, nothing uncommitted is ever left staged when an
  inline statement runs. If that stops being true the inline path would commit
  another statement's record early — say so in the comment as the reason the
  drain must commit unconditionally.
- [ ] Verify: `just wovm-test` and `just db-bench --quick` both green, and
  `WO_SHARDS=1` in particular — the single-shard configuration takes this path
  exclusively.
- [ ] Commit.

## Task 4 — prove batches actually form

**Files:**
- Modify: `scripts/db-bench.py` (new metrics and their tolerances),
  `docs/examples/db-bench/main.wo` only if the batch figures cannot be observed
  without the sample reporting them.
- Test: the driver's own gate-bites check.

**Interfaces:**
- Consumes: the batching from Task 2.
- Produces: mean batch size, peak batch size and peak staged bytes as recorded
  metrics, so Task 5 measures a mechanism that is known to engage.

- [ ] Decide where the counters live and prefer the smallest surface: the
  runtime can report them at exit, or the driver can derive them. Do not add a
  builtin for this — the numbers are diagnostic, not part of the language.
- [ ] Record mean and peak batch size under the concurrent multi-shard write
  workload. **This is the task's real point:** if batches are always one, the
  feature is inert and any throughput change came from somewhere else, so the
  measurement in Task 5 would be attributing a win to the wrong cause.
- [ ] Record peak staged bytes. This settles whether the batch needs a cap with
  a number instead of a guess — the spec deliberately shipped no cap because the
  request queue is already bounded upstream by iteration 24's mailbox caps.
- [ ] Give the new metrics wide tolerances. Batch size is a function of arrival
  timing, so gating it tightly would gate the scheduler; what must be gated is
  that it is greater than one under contention.
- [ ] Verify the gate bites: doctor the recorded mean batch size to one and
  confirm the suite fails on exactly that metric.
- [ ] Commit.

## Task 5 — measure the payoff, gate it, write it down

**Files:**
- Modify: `bench/baseline.json` (refresh, with the reason in the commit
  message), `docs/plan/perf-targets.md` (a new section).

**Interfaces:**
- Consumes: Tasks 2 and 4.
- Produces: the before/after record every later optimization argues against.

- [ ] Capture the before numbers from the committed baseline rather than
  re-measuring them: `durable.sN.mixwrite` 480 ops/s, p50 538 µs, p99 5888 µs;
  `durable.s1.mixwrite` 1023 ops/s, p99 664 µs; `seed` ~4460 ops/s on both.
- [ ] Run the full campaign, not the quick one, and record after numbers for
  the same metrics on the same machine. A payoff measured across machines is
  not a payoff.
- [ ] Assert the scoped criterion: **`durable.sN.mixwrite` throughput up and
  p99 down**, with `durable.s1.*` and both `seed` legs not regressed. Do not
  report the s1 seed number as a disappointment — a serial writer has nothing
  to batch with, and the spec says so.
- [ ] Write the `perf-targets.md` section: the before/after table, the mean and
  peak batch size that produced it, and the peak staged bytes. State the
  inversion that motivated the work — multi-shard concurrent writes were 2×
  slower than single-shard with a 9× worse p99 — and whether it is now gone.
- [ ] If the payoff is absent or small, **say so and stop.** That is a finding,
  not a failure: it would mean the barrier was not the bottleneck the baseline
  implied, and part B must not be started on an unproven premise.
- [ ] Refresh the baseline and confirm `just db-bench` passes against it, then
  re-confirm the gate bites on a doctored write metric.
- [ ] Commit.

## Task 6 — closeout

**Files:**
- Modify: `docs/stories/databasev2/04-io-uring-commit.md` (progress, criteria
  split met/outstanding, the landing banner),
  `docs/stories/00-status.md` (standup entry, chain note),
  `docs/plan/oop-vm/01-error-catalog.md` (the `WO_T_IO` removal and the new
  exit status), `database/src/CODE-LOGIC.md` (a group-commit section).

- [ ] Story: record what landed and what did not. The outstanding items are
  single-shard concurrent batching (needs the inline park) and part B itself.
  Keep the corrected premise visible — this iteration was written as
  "fsync-per-commit" and the engine was fsync-per-statement.
- [ ] Error catalogue: `WO_T_IO` no longer reachable from a write, and the new
  durability-failure exit status documented beside the trap and refusal codes.
  A language-visible removal that is not written down is a trap for the next
  reader.
- [ ] `CODE-LOGIC.md`: the commit path as built — where the barrier runs, why
  replies are held, why the inline path is asymmetric, and the one rule for
  failure. Explain the reasoning, not the call graph.
- [ ] Board: the standup entry in the six-question shape, and the chain note —
  part B's go/no-go now rests on Task 5's number.
- [ ] Full battery after the doc edits: `just wovm-test`, `just woc-test`,
  `just oop-e2e`, `just db-bench`, `python3 scripts/linkcheck.py .`
- [ ] Commit.

## Self-review notes

- **Spec coverage.** Queue-drain boundary → Task 2. Fatal failure rule → Tasks 1
  and 3. Held replies and the ack contract → Task 2. No batch cap, settled by
  measurement → Task 4. Payoff and its scoping → Task 5. `WO_T_IO` removal →
  Task 6. The disclosed abort-coverage gap → Task 1's last step.
- **The riskiest task is 2**, and its risk is concentrated in one place: the
  crash legs of the durability battery. That is why the plan says stop rather
  than adjust if they fail.
- **Task 3 looks like a no-op and is not.** Without it the inline path keeps a
  catchable `WO_T_IO` while the request path aborts, which is precisely the
  per-path unevenness this spec exists to remove.
- **Task 4 before Task 5 is deliberate.** Measuring a payoff before proving the
  mechanism engages is how a win gets attributed to the wrong cause.
