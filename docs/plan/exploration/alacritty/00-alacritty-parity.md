# Alacritty parity — what the language needs to build a terminal-class application

Source: [`.dev/reference/alacritty/`](../../../../.dev/reference/alacritty/)
(shallow clone, surveyed 2026-09-01). Companion study to
[`fiber/00-fiber-parity.md`](../fiber/00-fiber-parity.md), which asked the same
question for the web-framework surface. This one asks it for a native,
event-driven, GPU-rendered desktop application — the workload class writeonce
is furthest from today.

## What alacritty actually is (measured, not summarized from its README)

33,699 lines of Rust across a 4-crate workspace, ~22 direct dependencies:

- **`alacritty_terminal`** — the portable core, no GUI anywhere in it: PTY
  creation and lifecycle (`tty/unix.rs`, 448 lines — openpty, fork/exec of the
  shell, window-size ioctls, child reaping), a byte-level escape-sequence
  parser (the `vte` crate), the grid data structure, and an event loop over
  the `polling` crate (epoll/kqueue on arbitrary fds).
- **`alacritty`** — the application: windowing (`winit`), OpenGL context
  creation (`glutin`, EGL/WGL), glyph rasterization (`crossfont`), a
  timer-driven scheduler (`scheduler.rs` — cursor blink, repaint deadlines),
  config with live reload (`notify` file watching), clipboard, signal
  handling (`signal-hook`), CLI (`clap`).
- **`alacritty_config`/`_derive`** — config plumbing (a proc-macro crate;
  writeonce's analogue would be compile-time codegen, which `@table` already
  does for a different domain).

The split matters more than the line count: the terminal CORE is a
files-and-processes program with zero graphics, and the GUI shell around it is
where every heavyweight dependency lives.

## What already fits — the part that costs nothing

- **The concurrency model is a better fit than alacritty's own.** Alacritty
  runs a PTY-reader thread and a window-event thread synchronized through
  `parking_lot` locks around the grid. Shard actors with ownership-move
  messages express this without shared state: a reader actor owns the parser,
  sends grid deltas; a display actor owns the grid. Iteration 24's chat app
  already proved the shape (fd-driven actor, fan-out, lifecycle).
- **Single self-contained binary** — alacritty's distribution story is
  writeonce's existing one.
- **Bytes + bitwise operators** (iterations 19, 36) — the VTE parser is a byte
  state machine; the primitive layer for it exists.
- **`fn main`, exit codes, env, fs, time** (plan 9 stdlib) — config discovery,
  CLI-shaped startup.

## The gaps, ordered by what unblocks what

Each maps onto an existing iteration where one exists; only two items are
genuinely new.

1. **PTY + bounded subprocess** — the heart of the core. Openpty, fork/exec
   with the child on the slave side as its controlling terminal, resize
   ioctls, orderly child shutdown. This is exactly iteration 28's named gap
   fan-out ("bounded subprocess, stdin/stdout transport") plus the PTY-specific
   ioctls. ~450 lines of Rust in the reference; a C-builtin family in
   writeonce's existing style (crypto/net precedent: small id-numbered
   builtins, no general FFI).
2. **Readiness on arbitrary fds.** The runtime's io_uring loop watches sockets
   it created. A PTY master fd — and later a display-server fd — must be
   registrable in the same loop, parking the owning fiber until readable.
   Alacritty needs nothing fancier (its `polling` crate is the same shape);
   this is a seam widening, not a new subsystem.
3. **Signals as events.** SIGCHLD (child died) and SIGWINCH (resize) must
   arrive as mailbox messages, the way iteration 24 handles fd events. Today
   signals are runtime-internal (SIGTERM drain). Small, but nothing else can
   substitute for it.
4. **Unicode width + UTF-8 decode in the stdlib.** The grid is addressed in
   cells; every printed byte-run needs "how many columns". A data-table
   problem, not a design problem — but without it a terminal misrenders
   immediately. (Alacritty: `unicode-width` crate.)
5. **`time.mono` returns, plus a timer wheel.** Cut 2026-08-10 with "returns
   when a workload needs monotonic math" — this workload is that consumer.
   `time.after` (id 90) exists; a repaint/blink scheduler needs monotonic
   deadlines that survive wall-clock jumps.
6. **Outbound connection + fd passing + shared-memory buffers.** Iteration 38
   already names `net.connect`. Wayland is *a unix-socket protocol*: with
   connect, SCM_RIGHTS fd passing, and an mmap/shm builtin, a Wayland client
   with software rendering (wl_shm) is expressible in pure `.wo` — a windowed
   terminal with **no C dependency linked at all**. This is the outside-the-box
   route the reference makes visible: alacritty predates it culturally (X11
   era) and pays for GL instead.
7. **The GPU fork — decide late.** OpenGL/EGL and font rasterization
   (freetype/fontconfig) cannot be spoken over a socket; they are C ABI or
   nothing. Three options, same fork iteration 28 already recorded as
   "FFI-vs-out-of-process": (a) general FFI — largest doctrine change, rejected
   until a second consumer demands it; (b) subsystem C builtins (the crypto
   precedent) — a `gfx`/`font` builtin family; (c) an out-of-process render
   server writeonce talks to over its own socket — fits the actor model,
   keeps the language pure, costs a second process. Software rendering via
   route 6 defers this fork entirely: glyph rasterization from a pre-baked
   bitmap font atlas is pure byte math.

## The maturity path, as driving workloads (the project's own method)

Each stage is a shippable proof, ordered so no stage waits on the fork in 7:

- **Stage A — headless terminal**: PTY builtins + signals + fd readiness
  (items 1–3). Proof: a `.wo` program spawns a shell, feeds it a script,
  captures and asserts the output — a `script`/`expect` clone. Closes
  iteration 28's bounded-subprocess gap as a side effect.
- **Stage B — VTE grid**: parser + grid in pure `.wo` (item 4). Proof: replay
  recorded terminal sessions (vttest, asciinema casts) and assert final grid
  state against the reference implementation's.
- **Stage C — multiplexer**: A + B + existing net = a tmux-lite: sessions
  survive detach, clients attach over a unix socket. No graphics, real
  product, exercises everything server-side writeonce is already good at.
  *Corrected by the tmux study (2026-09-01): this stage also needs
  SCM_RIGHTS fd passing (item 6's builtin, promoted here), termios adoption
  of the client's own tty, and the terminfo fork — see
  [`../tmux/00-tmux-parity.md`](../tmux/00-tmux-parity.md).*
- **Stage D — windowed, software-rendered**: item 6 (connect, fd passing,
  shm) + a Wayland client library in `.wo`. First pixel on screen with zero
  linked C.
- **Stage E — GPU**: only now decide item 7, with D as the measured baseline
  that says whether GL is worth an FFI doctrine change.

## What this does NOT recommend

No general FFI now (one consumer, and stages A–D never need it); no bundled
font rasterizer until D shows bitmap atlases failing; no attempt at winit-class
cross-platform windowing — Linux/Wayland first, the same way the runtime is
Linux/io_uring first.
