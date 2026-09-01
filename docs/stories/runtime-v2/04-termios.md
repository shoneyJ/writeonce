---
track: runtime-v2
iteration: "4"
status: pending
readiness: refine
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

## Info — the forks (open)

1. **Surface size.** Exactly two verbs — `term.raw()` returning a
   restore token and `term.restore(token)` (the lean: wmux needs
   nothing else; tmux itself uses little more than `cfmakeraw`) —
   versus exposing termios flag knobs. YAGNI says two verbs and a
   refusal for the rest.
2. **Restore guarantee.** Tie restoration to the unwind machinery (the
   42 ownership pattern: fiber dies, terminal restored — a runtime
   obligation) versus caller's-problem-with-a-doc-note. Lean: runtime
   obligation; "no orphan" has a terminal-state sibling: no wrecked tty.
3. **Scope.** stdin only, versus any tty fd (a client adopting a tty it
   received via [5](05-fd-passing.md) — the wmux SERVER's need). This
   fork decides whether the verb takes an fd argument now or grows one
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
