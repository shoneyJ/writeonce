---
track: runtime-v2
iteration: "4"
status: pending
readiness: ready
---

# runtime-v2 4 — termios adoption: raw mode on a terminal we were given

> Part of [Story — runtime-v2: the runtime beyond sockets](00-story.md).
> No incoming edges — startable alone, any time.
>
> **The problem.** [2](02-pty.md) creates terminals for children; this
> adopts the one the PROCESS was given. A wmux client must put its own
> stdin into raw mode (no echo, no line buffering, keys arrive as they
> are typed) and — non-negotiably — restore it on every exit path,
> including a trap. A terminal left raw is the classic way a program
> makes a user's shell unusable.

## Info — the forks, settled (brainstorm 2026-09-01; spec:
[`2026-09-01-runtime-v2-design.md`](../../superpowers/specs/2026-09-01-runtime-v2-design.md))

1. **Exactly two verbs**: `term.raw(fd)` and `term.restore(fd)`. Saved
   termios live in a per-shard table keyed by fd (one thread — no
   locks); double-raw on one fd refuses by name; no flag knobs.
2. **Restore is a RUNTIME OBLIGATION.** Fiber unwind, engine stop and
   `wo_vm_destroy` restore every saved tty, newest first — "no orphan"
   has its terminal-state sibling: no wrecked tty, ever, trap paths
   included.
3. **fd-taking from day one** — a tty received via
   [5](05-fd-passing.md) works without the verb growing an argument
   later.

## Acceptance sketch

- Raw on, keystroke arrives unbuffered and unechoed (driven under a
  [2](02-pty.md) PTY in the test — the two iterations prove each other).
- Restore: `tcgetattr` before equals after, on the normal path AND after
  a deliberate trap while raw.
- The stop path restores too — SIGTERM while raw leaves a sane terminal.

## Consumers

wmux 1's client half. The alacritty stage-A harness benefits but does
not require it.
