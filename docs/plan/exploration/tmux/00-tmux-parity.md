# tmux parity — the multiplexer as the language's next driving workload

Source: [`.dev/reference/tmux/`](../../../../.dev/reference/tmux/) (shallow
clone, surveyed 2026-09-01). Companion to
[`alacritty/00-alacritty-parity.md`](../alacritty/00-alacritty-parity.md),
whose stage C ("tmux-lite") this study scopes for real. tmux is the more
instructive reference of the two: it needs **no graphics at all**, so the
entire program sits inside the territory the alacritty study's stages A–C
cover — it is the proof target, not a stepping stone to one.

## What tmux actually is (measured)

~112,000 lines of C (100,027 in the 153 top-level `.c` files; the rest is
`compat/` shims and headers). Exactly **two** external dependencies —
libevent (fd loop + buffers) and ncurses used *only* as a terminfo(5) reader
(`tty-term.c` calls `setupterm`/`tigetstr` and nothing curses-like). Every
other need is vendored in `compat/`: OpenBSD's imsg framing, `forkpty` for
five platforms, `daemon`.

The architecture is one daemonized **server** owning every session, window,
pane and PTY (`server.c:190` forks it), and a deliberately thin **client**
(808 lines): the client connects over a unix socket and **passes its own
terminal fd to the server** with SCM_RIGHTS (`compat/imsg-buffer.c:798`,
`proc.c` — `imsgbuf_allow_fdpass`); from then on the server writes escape
sequences directly to the client's tty. Detach survives because the client
process is disposable — the server never was attached to a terminal it
doesn't hold as a passed fd.

Where the lines actually go — the multiplexer kernel is small, the UX is not:

- **Kernel, ~27k**: `server-client.c` 3,280 · `input.c` 3,745 (the escape
  parser) · `tty.c` 3,221 (the output driver) · `screen-write.c` 3,204 ·
  `window.c` 2,893 · `grid.c` 1,839 · `layout.c` 2,022 (the pane split
  tree) · `tty-keys.c` 1,883 (key decoding) · `utf8.c` 1,043 · plus
  session/spawn/proc/job plumbing.
- **A command LANGUAGE, ~12k**: 63 `cmd-*.c` files behind a yacc grammar
  (`cmd-parse.y`) — the config file is just commands; `format.c` is 7,182
  lines implementing a template DSL with ~380 variables; `options-table.c`
  2,070 lines of typed settings.
- **Interactive UX, ~20k**: `window-copy.c` 7,300 (copy mode is the single
  biggest file in tmux) · choose-tree/customize/menus/prompt.

## What writeonce already answers

- **Client/server over a unix socket** — `listen_unix` landed (iteration 35);
  the request/actor shape is the porch daily bread.
- **Server = actor tree.** tmux multiplexes everything through one
  single-threaded libevent loop with callbacks; sessions, windows, panes and
  clients as actors with ownership-move messages is the same topology with
  the concurrency written down instead of implied. Iteration 24 (chat rooms,
  fan-out, lifecycle, SIGTERM drain) already proved every piece of the
  pattern.
- **The DSL layer is free.** tmux hand-rolls a command grammar, a format
  template language and a typed options table (~12k lines) because C has no
  expression language to lend. writeonce's config/scripting surface can be
  the language itself — `${ }` interpolation replaces `format.c`, and a
  `@table` of settings replaces `options-table.c`.
- **Durable sessions beyond tmux.** tmux state dies with the server; a
  writeonce multiplexer's session/layout tables can be `durable: true` for
  free — scrollback in the WAL is the kind of trick the storage engine
  exists for. Feature, not parity.

## Gaps — mostly shared with the alacritty list, two new, one promoted

1. **PTY + bounded subprocess, signals, arbitrary-fd readiness, unicode
   width, monotonic timers** — identical to alacritty study gaps 1–5;
   `spawn.c`/`job.c`/`compat/fdforkpty.c` are the reference reading.
2. **fd passing (SCM_RIGHTS) — PROMOTED.** The alacritty study placed it in
   stage D (Wayland). Wrong stage: detach/attach — the whole point of a
   multiplexer — is built on handing the client's tty fd across a unix
   socket. Second consumer found; the builtin belongs to the multiplexer
   stage. (The alacritty study is corrected in place.)
3. **NEW — termios control of an existing terminal.** The client must put
   *its own* tty into raw mode and restore it on exit (tmux:
   `cfmakeraw`, `tcgetattr`/`tcsetattr`). The PTY gap covers creating
   terminals; this is adopting one you were given. Small builtin family,
   nothing else substitutes.
4. **NEW — the terminfo fork.** tmux answers "what escape sequences does
   THIS client's terminal speak" from the terminfo database. Two honest
   options: read the compiled terminfo format in pure `.wo` (a documented
   binary file — a parser, not a linked library; ncurses would NOT be
   imported) or emit a fixed xterm-256color profile and refuse exotic
   terminals by name. Decide at brainstorm; start fixed, the refusal names
   the gap.
5. **Daemonization** — fork-and-detach with the socket handed over. Cheap,
   and arguably skippable first (a foreground server under systemd was good
   enough for writeonce.de).

## Corrected staged path (supersedes the alacritty study's stage C sizing)

- **Stage A/B unchanged** — headless PTY runner, then the VTE grid replayed
  against recorded sessions (`input.c` + `grid.c` are the behaviours to pin).
- **Stage C — the multiplexer** now carries its real bill: A + B **plus**
  fd passing (gap 2), termios adoption (gap 3), and the terminfo decision
  (gap 4). Proof: detach, kill the client, reattach from another terminal,
  scrollback intact — then restart the *server* and reattach with layout and
  scrollback replayed from the WAL, which is the demo tmux cannot give.
- **Stage D/E unchanged** (Wayland shm, then the GPU fork) — and stage D
  gets gap 2 for free once C lands.

Scope honesty: parity with tmux the product is ~100k lines including a 7k
copy mode and 20k of chooser UX — not the goal. The kernel a driving
workload needs is the ~27k-line column, and the DSL third of tmux dissolves
into language features writeonce already has.
