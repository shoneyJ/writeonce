# databasev2 3 — WAL checkpoint (implementation plan)

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps
> use checkbox (`- [ ]`) syntax for tracking.
>
> **Style rule (user convention):** concept, reason, and required
> behaviour in words plus verification commands only — no implementation
> or test code blocks; the executor writes the code.

**Goal:** reclaim disk and bound replay by rewriting the log as one record per
live row and swapping it in with `rename`, so boot replays a short log instead
of all history.

**Architecture:** compaction writes the live store into a temporary file using
the existing record grammar and the existing append path, fsyncs it, renames it
over the live WAL, fsyncs the parent directory, and reopens the descriptor.
Recovery is untouched — boot still opens one file and replays it — and every
crash point is safe because `rename` is atomic.

**Tech Stack:** C11, libc only. `pwrite`, `fdatasync`, `rename`, `open`,
`unlink`. No new dependency and no new file format.

**Spec:** [`../specs/2026-08-28-wal-checkpoint-design.md`](../specs/2026-08-28-wal-checkpoint-design.md)

## Global Constraints

- **Recovery must not change.** No second source, no cutoff offset, no control
  file. If a task finds itself editing the replay path, something has gone
  wrong with the design and it should stop rather than proceed.
- **Every crash point falls back.** Before the rename the live log is untouched;
  after it the new log is complete. There must be no window in which a reader
  could observe a mixture.
- **The record grammar is frozen.** The whole argument for this design is that
  it already suffices. A compacted log is INSERT records for live rows, ids
  preserved exactly.
- **Bounded memory.** `stage()` grows the staging buffer by doubling and never
  shrinks it, so dumping a whole store through one buffer would hold the entire
  store in RAM — the unbounded growth databasev2 1 identified as how this engine
  dies. The dump must flush periodically.
- **Compaction may run only where nothing is staged** — in practice immediately
  after a barrier. Anywhere else, a staged record lands in a file about to be
  replaced.
- **libc only**, no new syscall interface. Gates run through `just`. Never
  commit on `master`; branch first.

---

## Task 1 — `wo_wal_compact`: rewrite, fsync, rename, reopen

**Files:**
- Modify: `database/src/wal.c`, `database/src/wal.h`.
- Test: `runtime/test/test_wal.c`.

**Interfaces:**
- Produces: a compaction entry point taking the live WAL and the store, which
  replaces the log with one INSERT record per live row and leaves the WAL usable
  (descriptor reopened, offset correct). Returns success or failure; a failure
  must leave the ORIGINAL log intact and usable, because a failed checkpoint is
  not a durability event.
- Consumes: the existing append path and commit routine, and the bitmap walk
  that `db.c` already performs in three places.

- [ ] Read three things first and confirm them, because the design rests on
  them: `apply_record` implements UPDATE as remove-then-recreate (so records are
  full row images), `wo_wal_append_insert` takes an id and reads the row from
  the store (so ids are preserved), and the tail scan treats a zero length field
  as end-of-log (so the new file must be zero beyond its records).
- [ ] Test first, RED: build a store, age it (insert rows, then update the same
  rows repeatedly so history exceeds live data), compact, then assert **both**
  that the log got materially shorter AND that a fresh replay of it produces the
  same rows with the same ids and the same values. Shorter alone is worthless —
  a truncating bug also passes that.
- [ ] Verify RED for the right reason: the entry point does not exist yet.
- [ ] Implement the walk: for each class, iterate slots via the bitmap and
  append one INSERT per live row. Reuse the append path; do not write a second
  encoder.
- [ ] **Flush every K records rather than staging the whole store.** Point a
  scratch WAL at the temp descriptor and commit periodically. State the chosen K
  and why in a comment. Without this the dump holds the entire store in RAM.
- [ ] Sequence the switch exactly: fsync the temp file, `rename` over the live
  path, **fsync the parent directory** (the rename is atomic in-kernel but the
  directory entry is not durable until the parent is synced), then reopen the
  descriptor — the old one refers to an unlinked inode — and reset the offset to
  the new end of log.
- [ ] Handle failure without losing data: any error before the rename must
  unlink the temp file and leave the live log untouched. A failed compaction is
  a missed optimisation, **not** a durability failure, so it must NOT take the
  fatal path databasev2 4 introduced.
- [ ] GREEN: `just wovm-test`.
- [ ] Commit.

## Task 2 — a stale temp file is removed, never read

**Files:**
- Modify: `database/src/wal.c` (the open path).
- Test: `runtime/test/test_wal.c`.

**Interfaces:**
- Consumes: Task 1's temp-file naming.
- Produces: the guarantee that a crash mid-rewrite leaves nothing that can be
  mistaken for data.

- [ ] Test first, RED: place a temp file next to the log containing *plausible,
  well-formed records* (not garbage — garbage would be rejected anyway and would
  prove nothing), open the store, and assert the temp file is gone and the
  replayed store is exactly what the live log said.
- [ ] Verify RED for the right reason.
- [ ] Remove any stale temp file when the WAL is opened. Note in a comment why
  this is safe: the only way one exists is a crash before a rename, and its
  contents are by definition not yet authoritative.
- [ ] GREEN: `just wovm-test`.
- [ ] Commit.

## Task 3 — the trigger, and the ordering guard

**Files:**
- Modify: `database/src/wal.c`, `database/src/wal.h` (remember the last
  compaction's size; the policy decision), `runtime/src/vm.c` (call the check
  after the barrier).
- Test: `runtime/test/test_wal.c`.

**Interfaces:**
- Consumes: Task 1's compaction entry point.
- Produces: automatic compaction, and the invariant that it never runs with
  records staged.

- [ ] Extract the policy as a **pure decision** — given the log's used bytes,
  the bytes the last compaction wrote, and a floor, should we compact? Pure
  because it is then unit-testable without a store, which is the only way this
  policy gets tested at all.
- [ ] Test the decision directly, RED then GREEN: below the floor it never
  fires however bad the ratio; above the floor it fires exactly when used bytes
  exceed the multiple; with no prior compaction it uses the floor alone.
- [ ] Record the bytes each compaction wrote, so the denominator is measured
  rather than estimated. Estimating the live size would mean estimating Text,
  and the compactor already knows the true number.
- [ ] Expose the floor and the ratio as env knobs, matching the existing idiom
  (`WO_MAILBOX`, `WO_HEAP_MB`, `WO_SHARDS`, `WO_WAL_STATS`). **This is what
  makes the policy testable** — a test sets a tiny floor and forces compaction
  in a few writes instead of waiting for megabytes. Document them beside the
  others. **Deviation from the spec, disclosed:** the spec spoke of a "manual
  trigger for tests"; env-tunable thresholds serve that purpose without adding
  language surface, which is the cheaper way to buy the same testability.
- [ ] **No timer.** If the implementer is tempted, the reason is in the spec:
  Postgres' `CheckPointTimeout` bounds loss from unflushed buffers, our records
  are durable at commit, and an idle log does not grow.
- [ ] Call the check from the one place that is safe — immediately after the
  drain's barrier, where nothing is staged. Comment that this is a correctness
  requirement and not a scheduling preference.
- [ ] Verify the guard: a test that stages records and then makes the policy
  say yes must find compaction deferred, not executed. This is the assertion
  that keeps the ordering rule true as the code moves.
- [ ] Verify durability is unaffected: `just db-bench --quick` — the crash and
  restart legs must be unchanged, and part A's `wmix` legs must still batch.
- [ ] Commit.

## Task 4 — kill -9 *during* compaction

**Files:**
- Test: `runtime/test/test_wal.c` (extend the existing fork-based crash
  battery).

**Interfaces:**
- Consumes: Tasks 1–3.
- Produces: the evidence for the criterion the whole design is shaped around.

- [ ] Read the existing crash battery first: a forked child inserts and acks
  each committed id over a pipe while the parent SIGKILLs it mid-stream, then
  the parent verifies every acked id survived. Extend that shape rather than
  inventing a second harness.
- [ ] Drive compaction repeatedly in the child (a tiny floor makes it fire
  often) while it inserts and acks, and kill at many instants so the kill lands
  inside a rewrite, at the rename, and after it.
- [ ] Assert the property, not a state: after replay the store must equal
  **either** the pre-compaction **or** the post-compaction content — never a
  mixture — and **every acked id must be present**. A test that only checks "it
  replayed without error" would pass on a silently truncated log.
- [ ] Assert no temp file survives a kill in a way that affects the next boot.
- [ ] Run the battery repeatedly, not once: this is a race, and one green run
  proves very little. State how many repetitions were run in the commit message.
- [ ] GREEN: `just wovm-test` plus the repetitions.
- [ ] Commit.

## Task 5 — measure: space, boot, and the pause

**Files:**
- Modify: `scripts/db-bench.py` (a checkpoint leg), `docs/plan/perf-targets.md`,
  `bench/baseline.json` (refresh, with the reason in the commit message).

**Interfaces:**
- Consumes: Tasks 1–3.
- Produces: the before/after record, and the pause number the spec deliberately
  refused to assume.

- [ ] Capture the before numbers already measured on master, rather than
  re-deriving them: `seed 20000` leaves a 986 614-byte log; 20 000 updates take
  it to 2 590 262 bytes **with the same live rows**; boot+verify on that aged
  store is 155 ms.
- [ ] Add a leg that ages a store, compacts it, and records: bytes before and
  after, the ratio reclaimed, and boot time before and after. Age it by
  updating the same rows — history must grow while the live set does not, or the
  leg is measuring insert throughput instead of compaction.
- [ ] Measure the **stop-the-world pause** on the largest store the harness
  builds and record it as a number. State the budget it must meet.
- [ ] **If the pause exceeds the budget, stop and report it.** That is the
  finding the spec asked for, and the alternatives (incremental copy,
  fork-and-dump) are bought against this number — not before it.
- [ ] Give the new metrics tolerances that match what they are: bytes reclaimed
  is structural and can be gated tightly; the pause is wall-clock on a shared
  box and cannot. Do not waive them all, which is the mistake part A's task 4
  made and had to undo.
- [ ] Verify the gate bites: doctor the reclaimed-bytes metric and confirm the
  suite fails on exactly that metric.
- [ ] Refresh the baseline and confirm the **full** campaign passes against it.
  The committed baseline is full-mode (`N=20000`, `crash_reps=3`) — writing a
  quick-mode baseline over it is a regression, and part A made exactly that
  mistake.
- [ ] Commit.

## Task 6 — closeout

**Files:**
- Modify: `docs/stories/databasev2/03-wal-checkpoint.md`,
  `docs/stories/00-status.md`, `docs/plan/oop-vm/04-db-binding.md`,
  `database/src/CODE-LOGIC.md`, `docs/examples/db-bench/README.md`.

- [ ] `04-db-binding.md`: the normative ordering rule — compaction runs only
  where nothing is staged, and what recovery does (unchanged: one file, replayed
  from byte 0). This is the doc the spec named for it.
- [ ] `CODE-LOGIC.md`: why one file rather than snapshot-plus-tail, why
  `rename` is the crash-safety primitive, why the dump flushes periodically, and
  why a failed compaction is not a durability event. Reasoning, not call graph.
- [ ] README: the new env knobs beside the existing ones, and the checkpoint
  leg.
- [ ] Story: progress, criteria split met/outstanding, and the measured
  before/after.
- [ ] Board: standup entry in the six-question shape, and the chain note —
  chain 6 was the last link, so say what the chain's completion means and what
  is next.
- [ ] **Record the `resident: keys` obligation prominently, in the story and at
  the compactor.** Compaction moves every record, so it invalidates every WAL
  offset iteration 2 stores; the compactor must rebuild that map as it writes.
  There is nothing to implement today because iteration 2's storage half does
  not exist — which is exactly why this must be written where the next
  implementer will hit it, not left in a spec they may not read.
- [ ] Full battery: `just wovm-test`, `just woc-test`, `just oop-e2e`,
  `just db-bench`, `python3 scripts/linkcheck.py .`
- [ ] Commit.

## Self-review notes

- **Spec coverage.** Compaction and the switch → Task 1. Stale temp → Task 2.
  Trigger, no timer, ordering rule → Task 3. Crash safety → Task 4. Space, boot,
  pause → Task 5. Normative doc, `resident: keys` obligation → Task 6.
- **The riskiest task is 4**, not 1: Task 1's correctness is a single replay
  comparison, while Task 4 is a race and can pass by luck. Hence the explicit
  instruction to run it repeatedly and to state the count.
- **Task 2 looks trivial and is not.** A stale temp file containing well-formed
  records is the one input that could be mistaken for data, so the test uses
  plausible records rather than garbage.
- **One thing deliberately NOT a task:** rebuilding the `resident: keys` offset
  map. It cannot be implemented against a feature that does not exist yet.
  Recorded as an obligation in Task 6 instead of a stub nobody can test.
