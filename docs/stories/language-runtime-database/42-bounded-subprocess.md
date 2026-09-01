---
track: language-runtime-database
iteration: "42"
status: done
readiness: ready
---

# 42 — bounded subprocess: spawn, supervise, and reap a child process

> Part of [Story — one language, one runtime, one database](00-story.md).
> Spec: [`2026-09-01-bounded-subprocess-design.md`](../../superpowers/specs/2026-09-01-bounded-subprocess-design.md)
> (brainstormed and approved 2026-09-01; the six forks below are settled).
> First named as iteration [28](28-skillhost-host-workload.md)'s leading gap
> ("bounded subprocess, stdin/stdout transport"), promoted to its own
> iteration on 2026-09-01 when three further consumers arrived at once — the
> [alacritty](../../plan/exploration/alacritty/00-alacritty-parity.md),
> [tmux](../../plan/exploration/tmux/00-tmux-parity.md) and
> [zen-browser](../../plan/exploration/zen-browser/00-zen-browser-parity.md)
> studies all bottom out on it as their first domino.
>
> **The problem.** A one-shot `proc.run` already exists
> (`runtime/src/sysio.c`, `WO_B_PROC_RUN`: fork/execvp on an argv vector,
> stdout/stderr captured through pipes, exit code returned) — but it is
> bounded in nothing that matters and blocking in the one place it must not
> be. It runs on the shard thread, not parked in a fiber, so one slow child
> stalls every actor on that shard; it has no deadline, and its `waitpid`
> loop notices a runtime stop only if a signal happens to interrupt it; its
> output caps (8,192 / 4,096 bytes, fixed) truncate silently rather than
> refuse; there is no stdin, no streaming, no concurrency ceiling, and no
> story for what happens to a child when its owner dies. Suspected but not
> yet reproduced: a child that fills the stdout pipe past the cap while the
> parent is waiting for stderr EOF deadlocks both — needs a test before it
> is claimed. Every workload class beyond "server that owns all its state"
> — a script runner (28), a terminal (alacritty stage A), a multiplexer
> (tmux), a browser driver (zen C′) — begins by starting a process and ends
> by being responsible for it.

## Why *bounded* — the doctrine half

The feature is deliberately not "exec". Every resource a child can consume
carries a declared ceiling, and exceeding it is a refusal by name, never a
hang or an OOM — the same fail-closed posture porch 1 proved for pool
saturation and iteration 31 for mailbox caps:

- **Time** — a deadline after which the child is killed and the caller
  resumes with a named timeout error.
- **Output bytes** — a cap on captured stdout/stderr; past it the child is
  killed and the error names the cap, because an unbounded pipe buffer is an
  unbounded allocation.
- **Concurrency** — a ceiling on live children per program; at the ceiling,
  spawning fails closed rather than queueing invisibly.
- **Lifetime** — a child is owned by the actor that spawned it. Owner dies,
  child is killed and reaped. Program stops, children are terminated before
  exit (iteration 40's drain guarantee extends to them). A zombie or an
  orphan is a bug by definition, not a caveat.

## Info — the forks, settled (brainstorm 2026-09-01)

1. **Retrofit, one-shot only.** The existing `proc.run` gains the bounds
   in place (deadline, declared output caps, fiber parking, owner-bound
   reaping, per-shard ceiling). The streaming form — long-lived child,
   stdout as mailbox messages, what tmux/alacritty/zen need — is the named
   follow-up, building on this slice's registry and pidfd machinery.
2. **Transport** — died with fork 1: a one-shot form streams nothing.
   Settled by the follow-up when it arrives.
3. **pidfd, no signal seam.** Child exit is a pollable fd (`pidfd_open`,
   Linux 5.3+) registered in the shard's io_uring beside the pipes and the
   deadline timer; the fiber parks in the iteration 35 `_dl` mould. The
   general signals-as-events seam stays deferred by name to its real
   consumer (the multiplexer study's stage C).
4. **Argv only; a shell form is refused permanently.** A caller wanting a
   shell types `sh -c` into their own argv and owns the quoting risk;
   consumer #1 is a confined script runner and must never get a
   shell-shaped API.
5. **Env and cwd stay inherited, zero knobs.** No consumer demands
   control; skillhost confinement owns env scrubbing when it arrives, and
   a clean-env knob breaks `execvp` path search.
6. **Ids.** `WO_B_PROC_RUN` keeps its id and gains parking + defaults; the
   extended bounded form takes the next free id from 96+ (89/90 remain
   31's holes); no `.wob` bump (38's precedent).

Bounds surface: bare `proc.run(cmd, args)` gets named defaults (30 s,
1 MiB stdout, 64 KiB stderr); an extended form states them per call;
exceeding any bound kills the child and raises a catchable error naming
the bound — silent truncation is removed.

## Progress

**DONE 2026-09-01, same day as the brainstorm.** Everything the spec
names landed: the parked rework (pidfd + epoll bundle + `wo_child`
registry in `sysio.c`/`vm.h`/`vm.c`), `proc.run_dl` end to end (wob.h id
96, loader arity row, builtin dispatch range, `types.ml` row — no
`emit.ml` change, as the net `_dl` precedent predicted), and the
suspected drain deadlock proven red against the old code (5.0 s hang to
the alarm, stdout truncated at 8192) before the rework dissolved it
(15 ms). Gates: `test_proc` 128/0 inside a fully green 19-suite ASan run,
woc-test 557/0, `just subprocess` 12/0 first run (ping answered in 2 ms
while a sleep-2 child was parked; SIGTERM left no child), `just site`
23/0 untouched. Mechanics written up in `runtime/src/CODE-LOGIC.md`.

## Acceptance criteria — firmed in the spec, normative form there

- Given a child that exits normally, when it is run, then its exit code is
  an ordinary value in the language and both output streams are readable.
- Given a child that outruns its deadline, when the deadline passes, then
  the child is dead (verified by pid absence, not assumed), the caller has a
  named timeout error, and no fd has leaked.
- Given a child whose output exceeds the byte cap, then the refusal names
  the cap and the child is dead — measured with a deliberately chatty child.
- Given an owning actor that dies while its child lives, then the child is
  reaped — verified from the outside via the process table.
- Given SIGTERM to the program while children live, then every child is
  terminated before the program exits — the drain gate (iteration 40's
  battery) gains a subprocess leg.
- Given a spawn loop of one thousand sequential children, then the
  program's fd count is flat (the iteration 24 measurement style) and rss
  does not grow with the loop.
- A runnable example under `docs/examples/` with a `just` gate, in the
  residency/chat mould — the gate is the acceptance, not the unit tests
  alone.

## Out of scope, by name

- **PTY allocation** — pipes only. Giving the child a pseudo-terminal is
  the alacritty/tmux stage-A follow-up and has its own ioctl surface.
- **fd passing (SCM_RIGHTS), termios, terminfo** — the tmux study's stage-C
  bill, separate iterations.
- **WebSocket client** — zen C′'s remaining item, not process work.
- **A general signal API for user code** — only if fork 3 resolves toward
  the private reaping path; otherwise this iteration carries the seam but
  not the surface.
- **Pipelines between children** — composition can wait for a consumer.

## Consumers, for the record

Iteration [28](28-skillhost-host-workload.md) (script runner, "bounded
subprocess first" was already its stated order); alacritty study stage A
(headless PTY runner — needs this plus PTY); tmux study (spawn half of its
kernel); zen study stage C′ (spawn the browser, then drive it). Four
consumers is the most any single named gap has accumulated.
