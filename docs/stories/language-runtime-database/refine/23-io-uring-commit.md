# Iteration 23 — io_uring group-commit write path

> Format: fiberloom `product/story-iteration-template`. Part of
> [Story — one language, one runtime, one database, one binary](../00-story.md).
>
> **Inserted 2026-08-15.** The write-path optimization, and deliberately the
> LAST database performance iteration: it only earns its complexity once
> there is a measured fsync-per-commit baseline to beat (iteration 22) and a
> multithreaded runtime to overlap against (iteration 8). Doing it earlier
> would optimize a number nobody had measured, against a runtime that
> couldn't use it.
>
> **No spec exists yet.** The forks in *Info* are genuine decisions.

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

- What to achieve?
    - **Given** the io_uring write path under the iteration-22 crash battery
      (concurrent writers, kill -9 mid-stream, reboot, replay),
    - **when** it runs,
    - **then** every acknowledged write is present after replay and no
      unacknowledged partial write is ever visible — the exact result the
      fsync path gives, so durability is provably unchanged.
- What to achieve?
    - **Given** the iteration-22 durable write benchmark,
    - **when** it is run on the fsync-per-commit path and then the io_uring
      group-commit path on the same machine,
    - **then** the io_uring path's write throughput is materially higher and
      its p99 commit latency lower, with the before/after numbers recorded —
      the payoff, measured, not asserted.
- What to achieve?
    - **Given** a kernel without io_uring (old, or restricted by seccomp),
    - **when** the runtime starts,
    - **then** it falls back to the pwrite + fdatasync path automatically and
      correctly — io_uring is an accelerator, never a hard dependency, and a
      binary that runs everywhere is the whole project's premise.

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
