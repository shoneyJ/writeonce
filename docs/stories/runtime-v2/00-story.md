# Story — runtime-v2: the runtime beyond sockets

The fifth track. Where [`databasev2/`](../databasev2/00-story.md) takes
the database beyond RAM, this track takes the I/O plane beyond sockets:
**processes, terminals and signals** — the third of the native surface
the runtime never learned. Iterations 8/11/35 taught it sockets, the
program-mode stdlib taught it files, and iteration
[42](../language-runtime-database/42-bounded-subprocess.md) (bounded
subprocess, ✅ on `master` 2026-09-01) opened this arc from inside the
language track before the arc had a name.

Numbering restarts at 1 and is local to this track; frontmatter carries
`track: runtime-v2`. Status rules are the repo's, unchanged.

## The problem, stated once

A shard's plane multiplexes fds it created itself — listeners, accepted
sockets, and (since 42) the pipe/pidfd bundle of a one-shot child. That
is not enough for any program whose job is other programs: a long-lived
child's output has no path into an actor, a pseudo-terminal cannot be
allocated or resized, a signal cannot become a message, a tty the
process was GIVEN cannot be adopted into raw mode, and an fd cannot
cross a unix socket. Five seams, each builtin-sized, each in
`runtime/src/` with a `types.ml` table row as its entire compiler cost
(the 42 precedent — no `emit.ml` change).

## The iterations

| # | Iteration | What it adds |
| --- | --- | --- |
| 1 | [streaming subprocess](01-streaming-subprocess.md) | a long-lived child as a first-class peer: output in, stdin out, exit as a notice |
| 2 | [PTY](02-pty.md) | openpty, controlling terminal, resize ioctls — a child that believes it owns a terminal |
| 3 | [signals as events](03-signals-as-events.md) | SIGWINCH/SIGCHLD/… as mailbox messages — the seam 42 deferred to its real consumer |
| 4 | [termios adoption](04-termios.md) | the process's OWN tty into raw mode and back — adopting a terminal it was given |
| 5 | [fd passing](05-fd-passing.md) | SCM_RIGHTS over unix sockets — detach/attach's foundation, and the Wayland stage's later |

Build order is 1 → 2 → 3 (each leans on the last); 4 and 5 have no
incoming edges and are startable alone. Edges live in
[dependency graph section 6](../../00-dependency-graph.md).

## The driving workload

[`wmux/`](../wmux/00-story.md) — the terminal multiplexer — consumes all
five; that track owns the product, this one owns the seams. Sibling
consumers per iteration are named in each story (skillhost 28, the zen
CDP driver, the alacritty Wayland stage). The doctrine carried over from
42: every resource a ceiling, every violation a refusal by name, no
zombie and no orphan ever, `.dev/reference/tmux` as the measured
reference.
