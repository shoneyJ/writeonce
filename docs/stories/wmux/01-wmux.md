---
track: wmux
iteration: "1"
was_language_iteration: "43"
status: pending
readiness: refine
---

# wmux 1 — the terminal multiplexer, writeonce's tmux alternative

> Part of [Story — wmux, the writeonce terminal multiplexer](00-story.md).
> The product the [tmux study](../../plan/exploration/tmux/00-tmux-parity.md)
> scoped and iteration
> [42](../language-runtime-database/42-bounded-subprocess.md) unblocked
> first —
> a terminal multiplexer in pure `.wo`, driving every remaining
> native-workload gap into the open the way log-watcher drove the
> language and chat drove the actors.
>
> **Why this workload.** tmux is ~112k lines of C with two dependencies,
> but its multiplexer kernel is ~27k and a third of the rest is a
> hand-rolled command/format/options DSL that dissolves into writeonce
> language features. Its architecture — one server owning sessions and
> PTYs, thin clients over a unix socket — is the actor tree writeonce
> already has, with the concurrency written down instead of locked. And
> wmux can give the one demo tmux cannot: sessions, layout and
> scrollback in `durable: true` tables, so **reattaching after a server
> restart replays everything from the WAL**.

## The dependency remap (graph section 6 carries the edges)

Iteration 42 (bounded subprocess) is DONE and was the first domino. The
runtime seams now live as the [`runtime-v2/`](../runtime-v2/00-story.md)
track, each brainstormed on demand — the order below is the build order:

1. **[Streaming subprocess](../runtime-v2/01-streaming-subprocess.md)**
   (runtime-v2 1) — 42's named follow-up: a long-lived child whose
   output arrives as mailbox messages in the owning actor; stdin
   transport. The registry/pidfd/cap machinery is already built.
2. **[PTY allocation](../runtime-v2/02-pty.md)** (runtime-v2 2) —
   openpty, the child on the slave as its controlling terminal, resize
   ioctls. Extends the same `wo_child` slot; `.dev/reference/tmux`
   `spawn.c`/`compat/fdforkpty.c` are the reading.
3. **[Signals as events](../runtime-v2/03-signals-as-events.md)**
   (runtime-v2 3) — SIGWINCH (and SIGCHLD beyond the pidfd path) as
   mailbox messages; the seam iteration 42 deliberately deferred to its
   real consumer. This is that consumer.
4. **[termios adoption](../runtime-v2/04-termios.md)** (runtime-v2 4) —
   the CLIENT puts its own tty into raw mode and restores it on exit.
   Creating terminals is gap 2; adopting one you were given is this.
   Startable alone.
5. **[SCM_RIGHTS fd passing](../runtime-v2/05-fd-passing.md)**
   (runtime-v2 5) — the client hands its tty fd to the server over the
   unix socket; detach/attach is built on it. Startable alone.
6. **VTE grid in pure `.wo`** — the escape parser and cell grid, pinned
   by replaying recorded sessions against the reference `input.c`/
   `grid.c` behaviour. Needs **unicode width tables** in the stdlib.
   This one is wmux's own, not runtime work.
7. **wmux itself** — server (session/window/pane actors, durable
   tables), client (thin: raw mode, fd handover, restore), the gate.

Not on the critical path, recorded: `time.mono`'s return (status-line
clock, repaint pacing — v2), and the terminfo fork below.

## Info — the forks (open; why this is `refine`)

- **Terminfo.** Answer "what does the client's terminal speak" by
  parsing the compiled terminfo database in pure `.wo` (a documented
  binary format — a parser, not a linked library) versus emitting a
  fixed xterm-256color profile and refusing exotic terminals by name.
  The tmux study leans fixed-first with a named refusal; confirm at
  brainstorm.
- **v1 surface.** Sessions + one window each, no split panes (tmux's
  `layout.c` is 2,022 lines of split-tree bookkeeping) versus splits in
  v1. Lean: no splits — detach/attach + durability IS the product's
  proof; splits are v2.
- **Scrollback residency.** Scrollback rows in a `resident: keys` table
  (the 120 GB-audit-table machinery, databasev2 2) versus resident with
  a row cap. Real fork: scrollback is append-mostly and read-rarely —
  keys-resident's exact profile.
- **Command surface.** tmux's 63 `cmd-*.c` + yacc grammar versus wmux
  taking commands as... writeonce source? a tiny line protocol? The DSL
  third of tmux should dissolve, not be rebuilt — decide what into.
- **Verb shape of the streaming form** (gap 1's own brainstorm): one
  `proc.spawn` returning an actor-addressable handle, versus spawn
  options on `proc.run_dl`.

## Acceptance sketch (firmed when the last gap lands)

- Given a wmux server with one session running a shell, when the client
  detaches and its process is killed, then the shell keeps running and a
  NEW client attaches from a different terminal with the screen intact.
- Given a server stopped with SIGTERM and restarted against the same
  `WO_DATA`, when a client reattaches, then sessions, layout and
  scrollback are replayed from the WAL — the beyond-tmux leg.
- Given SIGTERM with live shells, then every child is gone before exit
  (iteration 40 + 42's guarantee, now with PTYs).
- Given a `vttest`/asciinema replay corpus, then the grid matches the
  reference implementation's final state (stage B's pin, kept green).
- Gate: `scripts/wmux-accept.sh` + `just wmux`, log to `/tmp/wmux.log`.

## Consumers and siblings

The alacritty study's stages A–C become wmux's gaps 1–6 verbatim; its
stage D (Wayland shm terminal) reuses gaps 5 and 6. The zen study's CDP
driver shares gap 1 only. skillhost (28) consumes gap 1's stdin
transport. Every remaining item in 28's old fan-out except fs-metadata
and FFI-vs-out-of-process now has wmux as its driving workload.
