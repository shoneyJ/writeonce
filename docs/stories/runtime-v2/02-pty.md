---
track: runtime-v2
iteration: "2"
status: pending
readiness: ready
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

## Info — the forks, settled (brainstorm 2026-09-01; spec:
[`2026-09-01-runtime-v2-design.md`](../../superpowers/specs/2026-09-01-runtime-v2-design.md))

1. **Separate verb**: `proc.spawn_pty(cmd, args, cols, rows) -> ?Child`
   — its own builtin id (static arity table stays static), same Child
   record with `in` and `out` both the master fd, `err` nil.
2. **Resize ships HERE**: `proc.resize(id, cols, rows)` → TIOCSWINSZ on
   the slot; refusal by name on a pipe child.
3. **Transport settled by iteration 1's pull decision** — the master fd
   is driven by the existing net verbs, nothing new.
4. **Master raw by default, no knobs** — until a consumer asks by name.
   (`openpty` may need `-lutil` — the plan verifies the link line
   before assuming.)

## Acceptance sketch

- A real shell spawned on a PTY prints a PROMPT (it never does over
  pipes) — asserted on the bytes.
- Resize: the child (a script reading TIOCGWINSZ) observes the new size.
- `isatty` inside the child answers yes on all three fds.
- Kill/reap/stop legs identical to 1's, PTY edition; fd count flat.

## Consumers

wmux 1 (every pane), the alacritty study's stage A (the headless
expect-clone is exactly "spawn on a PTY, script it, assert").
