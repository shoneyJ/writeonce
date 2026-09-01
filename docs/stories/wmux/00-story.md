# Story — `wmux`, the writeonce terminal multiplexer

The fourth track, and the first whose product is an end-user *program*
rather than a library or the toolchain: wmux, a tmux alternative written
in `.wo`. Where [`porch/`](../porch/00-story.md) proves writeonce can
carry a web framework, this track proves it can carry a native
fd-and-process workload — PTYs, signals, raw terminals, fd passing —
the territory the
[alacritty](../../plan/exploration/alacritty/00-alacritty-parity.md),
[tmux](../../plan/exploration/tmux/00-tmux-parity.md) and
[zen-browser](../../plan/exploration/zen-browser/00-zen-browser-parity.md)
parity studies mapped on 2026-09-01.

Numbering restarts at 1 and is local to this track. Frontmatter carries
`track: wmux`; iteration 1 was briefly language iteration 43 and keeps
`was_language_iteration:` so a search for the old number still finds it.
Status rules are the repo's, unchanged.

## Why a separate track

Same three reasons porch got one, plus a fourth:

1. **Different substrate, different gates.** wmux is `.wo` source, proven
   by its own gate (`just wmux` when it exists) and a replay corpus, not
   by the conformance corpus.
2. **Different cadence.** Its runtime prerequisites — the
   [`runtime-v2/`](../runtime-v2/00-story.md) track: streaming
   subprocess, PTY, signals, termios, fd passing — are each
   builtin-sized runtime slices; the product iterations on top are
   `.wo` work.
3. **It is a product surface.** A working tmux alternative is the
   strongest public claim the language can make about native workloads.
4. **The upstream split is explicit.** When a wmux iteration needs a new
   builtin, that half is a [`runtime-v2/`](../runtime-v2/00-story.md)
   iteration, called out by name — the way iteration 42 (bounded
   subprocess) landed first in the language track before the arc had a
   folder. The gap chain lives in
   [dependency graph section 6](../../00-dependency-graph.md).

## Where the sequence came from

The tmux study, measured against a shallow clone in
`.dev/reference/tmux`: ~112k lines of C, two dependencies, a ~27k-line
multiplexer kernel, a ~12k-line command/format/options DSL that
dissolves into writeonce language features, and one structural trick —
the client passes its own tty fd to the server with SCM_RIGHTS, which is
all detach/attach is. wmux's beyond-tmux leg is native to this stack:
sessions, layout and scrollback in `durable: true` tables, replayed from
the WAL after a server **restart** — state tmux loses by design.
