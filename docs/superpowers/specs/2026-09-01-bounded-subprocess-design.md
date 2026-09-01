# Bounded subprocess v1 — the one-shot form, bounded and parked

> Brainstormed and approved 2026-09-01. Story:
> [language 42](../../stories/language-runtime-database/42-bounded-subprocess.md).
> Scope settled during brainstorm: bound the existing one-shot `proc.run`
> only; the streaming form (long-lived child, output as mailbox messages) is
> the named follow-up. Exit notification via pidfd, no signal seam. Bounds as
> defaults plus per-call override, refusal by name. Argv only, no shell form
> ever. Env and cwd stay inherited, no knobs.

## The problem

`proc.run` exists (`runtime/src/sysio.c`, `WO_B_PROC_RUN`) but is bounded in
nothing that matters and blocks in the one place it must not: it runs the
fork, the pipe drain, and the `waitpid` on the shard thread itself, so one
slow child stalls every actor and fiber on that shard. It has no deadline —
a child that never exits parks the shard forever, and the `waitpid` loop
notices an engine stop only if a signal happens to interrupt it. Its output
caps are fixed constants (8,192 bytes stdout, 4,096 stderr) that truncate
silently. Nothing reaps a child when the engine stops. And the sequential
drain (stdout to cap, then stderr to EOF) is suspected of deadlocking
against a child that fills the stdout pipe while holding stderr open — to be
proven by a failing test before the fix is claimed.

Four consumers arrived at once — iteration 28's script runner and the
alacritty/tmux/zen-browser exploration studies — all bottoming out on this
as their first domino.

## The model, and why

**Bounded means: every resource a child can consume carries a declared
ceiling, and exceeding one is a refusal that names the ceiling — never a
hang, a truncation, or an OOM.** Same fail-closed posture as porch 1's pool
saturation and iteration 31's mailbox caps.

- **Time.** Every run has a deadline. At the deadline the child is killed,
  reaped, and the caller gets a catchable error naming the deadline and its
  value.
- **Output bytes.** Stdout and stderr each carry a cap. Past a cap the
  child is killed and the error names the cap — silent truncation is
  removed, which is the one observable behavior change for existing
  callers (code relying on truncation was relying on a bug).
- **Concurrency.** A per-shard ceiling on live children (default 32). At
  the ceiling a spawn fails closed with a named error rather than queueing.
- **Lifetime.** A child is owned by the fiber that spawned it. The fiber
  unwinding for any reason — actor death, engine stop — kills and reaps the
  child. Engine stop kills every registered child before the process exits:
  iteration 40's drain guarantee extends to subprocesses. A zombie or an
  orphan is a bug by definition.

**Call surface.** `proc.run(cmd, args)` keeps its shape and its `Proc`
record (`code`, `out`, `err`). Bare calls get the named defaults: 30 s
deadline, 1 MiB stdout cap, 64 KiB stderr cap. An extended form states
`deadline_ms`, `out_cap` and `err_cap` per call — deadline is intrinsically
per-call (a browser run and a `true` are not the same ask), which is why the
bounds do not live in `wo.toml`. Precedent: mailbox caps — a default that a
declaration can override.

**Argv only, no shell form, ever.** The capability a shell would add
already exists through argv (a caller can spawn `sh` with `-c` explicitly,
owning the quoting risk by having typed it); a shell builtin adds nothing
but an injection-shaped API, and consumer #1 is a *confined* script runner.
The existing argv limits (62 arguments, 512 bytes each) stay — nothing
asked for more.

**Env and cwd stay inherited, zero knobs.** No consumer demands control;
28's confinement story owns env scrubbing when it arrives, and a clean-env
knob has hidden teeth (`execvp` path search dies with an emptied
environment). Recorded as the skillhost follow-up, not built.

## Mechanics

**Parking, not blocking.** The builtin becomes a parking builtin in the
iteration 35 mould (the `_dl` net family): after the fork it registers the
two pipe read ends, a pidfd for the child (`pidfd_open`), and a deadline
timer with the shard's io_uring, then parks the calling fiber. The shard
thread never waits on a child. Readiness events append into growable
buffers up to the cap; the pidfd firing means the child exited (reap,
finish, unpark); the timer firing first means deadline (kill via
`pidfd_send_signal`, reap, unpark with the error). Both pipes are serviced
concurrently by readiness, which structurally removes the suspected
sequential-drain deadlock.

**Why pidfd and not SIGCHLD or a reaper thread.** A pidfd is a pollable fd
— exactly what the runtime's loop already multiplexes — with no
process-wide signal handler, no delivery-shard question, no masking rules,
and no race between concurrent children. The general signals-as-events
seam stays deferred by name to its real consumer (the multiplexer study's
stage C). A reaper thread would introduce a thread class the runtime does
not have, for no gain over the fd. Kernel floor: `pidfd_open` is Linux
5.3+, well under the runtime's existing io_uring floor.

**The child registry.** Each shard keeps a registry of its live children
(pid + pidfd + owning fiber). Spawn checks the ceiling against it and fails
closed at 32. Engine stop walks it and kills before exit. Fiber unwind
(actor death, cancellation) kills its own entry through the same path. One
mechanism, three callers.

**Builtin ids.** `WO_B_PROC_RUN` keeps its id and gains the parking
semantics and defaults. The extended bounded form takes the next free id
from 96 up (89/90 remain iteration 31's reserved holes). No `.wob` version
bump — iteration 38's precedent: new builtin ids alone do not move the
format.

## Acceptance criteria

- A child that exits normally: exit code, stdout and stderr surface as
  values — the existing behavior, now from a parked fiber; a second actor
  on the same shard demonstrably makes progress while a slow child runs.
- A child that outruns its deadline: caller resumes with the named timeout
  error, and the pid is verified absent from the process table — measured,
  not assumed.
- A child that exceeds an output cap: refusal names the cap and its value;
  the child is dead; no fd leaked.
- The deadlock repro: a child that fills stdout past the pipe capacity
  while holding stderr open. The test is written against the CURRENT code
  first and must fail (hang) there; it passes under the new mechanics.
- The drain leg: SIGTERM to a program with live children — every child is
  terminated before the program exits. Iteration 40's battery gains this
  leg.
- The ceiling leg: the 33rd concurrent spawn on one shard fails closed
  with the named error while the 32 live ones are unaffected.
- One thousand sequential spawns: fd count flat (the iteration 24
  measurement style), rss not growing with the loop.
- A runnable example under `docs/examples/subprocess` behind a `just`
  recipe, logging to `/tmp/<app>.log` per convention — the gate is the
  acceptance, unit tests alone are not.

## Out of scope (each with its reason)

- **The streaming form** — long-lived children with output as mailbox
  messages. The follow-up this slice deliberately excludes; tmux, alacritty
  and the zen CDP driver queue behind it. The registry, pidfd wait and
  cap machinery built here are its foundation.
- **PTY allocation** — pipes only; the pseudo-terminal surface (openpty,
  resize ioctls) is the alacritty/tmux stage-A follow-up.
- **A general signal API** — deferred to its real consumer; this slice's
  child-exit path needs no signals at all.
- **Env/cwd control** — skillhost confinement's item, recorded above.
- **stdin transport** — no consumer in this slice's set feeds a child
  interactively; the streaming form owns it.
- **Pipelines between children** — composition waits for a consumer.
- **A shell convenience form** — refused permanently, not deferred.
