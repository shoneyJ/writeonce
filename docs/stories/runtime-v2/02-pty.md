---
track: runtime-v2
iteration: "2"
status: pending
readiness: refine
---

# runtime-v2 2 — PTY: a child that believes it owns a terminal

> Part of [Story — runtime-v2: the runtime beyond sockets](00-story.md).
> After [1](01-streaming-subprocess.md): a streaming child whose stdio
> is a pseudo-terminal instead of pipes. A shell run over pipes disables
> its prompt, its line editing and its job control; a multiplexer is
> pointless without them.
>
> **The problem.** Nothing can allocate a PTY pair, place the child on
> the slave side as its controlling terminal, or resize it. The
> reference reading is `.dev/reference/tmux` `spawn.c` +
> `compat/fdforkpty.c` (~450 lines of C covering five platforms; Linux
> alone is far smaller).

## Info — the forks (open)

1. **Surface.** A `pty: true` option on iteration 1's spawn verb (the
   lean — one spawn shape, two transports) versus a separate
   `proc.spawn_pty`.
2. **Resize.** A verb on the handle (`resize(cols, rows)` → TIOCSWINSZ)
   — needed the moment [3](03-signals-as-events.md) delivers SIGWINCH.
   The fork is only whether it ships here or with 3.
3. **The master fd's transport** is settled by iteration 1's fork 2 —
   whatever won there carries the PTY master identically.
4. **Encoding edge.** A PTY master delivers the child's output with tty
   post-processing (ONLCR and friends): raw the master by default versus
   expose termios knobs. Lean: raw, no knobs, until a consumer asks.

## Acceptance sketch

- A real shell spawned on a PTY prints a PROMPT (it never does over
  pipes) — asserted on the bytes.
- Resize: the child (a script reading TIOCGWINSZ) observes the new size.
- `isatty` inside the child answers yes on all three fds.
- Kill/reap/stop legs identical to 1's, PTY edition; fd count flat.

## Consumers

wmux 1 (every pane), the alacritty study's stage A (the headless
expect-clone is exactly "spawn on a PTY, script it, assert").
