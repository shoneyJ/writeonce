# runtime-v2 — the runtime beyond sockets, all five iterations settled

> Brainstormed and approved 2026-09-01, one pass across the whole track:
> [runtime-v2](../../stories/runtime-v2/00-story.md) iterations 1–5.
> Scope settled: PULL transport (a child is fds; the net verbs already
> drive them), `wait_dl` over death notices, PTY as a spawn variant,
> signals via signalfd delivering the signal number, termios as two
> fd-taking verbs with runtime-guaranteed restore, fd passing as two
> verbs plus `net.connect_unix`. This spec is track-wide; each iteration
> gets its own implementation plan when it starts.

## The one principle

**A child is fds plus a registry slot — runtime-v2 adds ACQUISITION
verbs, never new transport.** `net.read_dl`, `net.write_dl` and
`net.close` already park on any fd through the shard plane (POLL_ADD is
fd-agnostic; pipes and PTY masters qualify today). So:

- Backpressure is the kernel pipe. An owner that stops reading makes
  the child block on write — exactly tmux under a slow client. The
  mailbox-cap collision never starts because nothing is pushed.
- The idle deadline is `read_dl`'s own per-call ms. Total-output caps
  disappear because no runtime buffer exists — the caller's read sizes
  ARE the bound.
- A wmux pane actor is chat's Reader actor with a different fd.

The rejected alternative, recorded: PUSH (child output as mailbox
messages) required new delivery machinery in `vm.c`, a policy for the
`wo_mailbox_cap` collision (drop, kill, or park — each wrong somewhere),
ordering rules across two streams, and per-chunk allocation. Everything
pull gets from three landed iterations, push would rebuild.

## The surface

New builtin ids from 97 up. Every id is FOUR registrations — `wob.h`
enum, `loader.c` arity table, `builtin.c` dispatch range, `types.ml`
stdlib row — the iteration 42 lesson; missing one reads as "unknown
stdlib builtin" from a valid image. No `.wob` version bump (the 38/42
precedent). `Child` joins the predeclared records (`types.ml`
`stdlib_records`): `id`, `in`, `out`, `err` — all Int-shaped; the fds
are ordinary conn-like values every existing fd verb accepts.

| Verb | Contract |
| --- | --- |
| `proc.spawn(cmd, args) -> ?Child` | iteration 1. Pipes on all three stdio fds, parent ends `O_NONBLOCK`; the registry slot is claimed (ceiling 32/shard, fails closed by name) and OWNED (below) |
| `proc.wait_dl(id, ms) -> ?Int` | iteration 1. Parks on the slot's pidfd (the 42 machinery); the exit code, or nil at the deadline. ONE waiter per id — a second concurrent wait refuses by name |
| `proc.signal(id, sig)` | iteration 1. `pidfd_send_signal` through the slot — no pid-reuse race |
| `proc.spawn_pty(cmd, args, cols, rows) -> ?Child` | iteration 2. openpty; child on the slave as controlling terminal; `in` and `out` are both the master, `err` is nil; master raw by default, no knobs |
| `proc.resize(id, cols, rows)` | iteration 2. TIOCSWINSZ on the slot's master; refusal by name on a pipe child |
| `signal.on(sig, addr)` | iteration 3. Standing subscription; delivery is an ordinary send of the SIGNAL NUMBER as a scalar (scalars copy — no moved-message ownership rules). Offerable: SIGWINCH, SIGCHLD, SIGHUP, SIGUSR1, SIGUSR2. SIGTERM/SIGINT refused by name — the stop latch stays the engine's (iteration 40 is load-bearing). Kernel coalescing disclosed, not sequenced |
| `term.raw(fd)` / `term.restore(fd)` | iteration 4. Save termios into the shard table, `cfmakeraw`, restore from the table. Restore is a RUNTIME OBLIGATION: fiber unwind, engine stop and `wo_vm_destroy` restore every saved tty, newest first. fd-taking from day one — a tty received via iteration 5 works |
| `net.send_fd(conn, fd) -> Bool` | iteration 5. `sendmsg` + SCM_RIGHTS, exactly one fd, fixed ancillary buffer; refusal by name on a non-unix socket or a second fd |
| `net.recv_fd(conn) -> ?Int` | iteration 5. The received fd as a plain Int; nil when the peer sent none |
| `net.connect_unix(path) -> Int` | iteration 5 carries it — iteration 38 (`net.connect`) has not landed; verified pending, not assumed |

## Lifecycle — 42's doctrine, streaming edition

The `wo_child` slot grows: pid, pidfd, owner, and the fds the runtime
must close if the owner never does. **Owner is the spawning ACTOR when
one exists, else the program.** Actor death (iteration 24's dead mark),
engine stop, and `wo_vm_destroy` each sweep: kill via pidfd, reap,
close the slot's fds, restore any termios the shard saved. The
per-shard ceiling stays 32; a zombie, an orphan, or a wrecked tty is a
bug by definition. `wait_dl` parking reuses `fb->dl_active`/`dl_at`
and parks on the pidfd directly — no epoll bundle needed for a single
fd.

## Mechanics notes (for the plans)

- signalfd joins shard 0's plane as one more registered fd with a
  sentinel `user_data`, the wake-eventfd precedent in `park.c`; reads
  drain `signalfd_siginfo` records and fan out sends.
- The termios table is per-shard (one thread — no locks), keyed by fd;
  double-raw on one fd refuses by name.
- Raw syscalls wherever glibc 2.35 lacks wrappers (the 42 rule);
  `signalfd` and `openpty` are fine (`openpty` needs `-lutil` — check
  the Makefile link line before assuming).
- Each iteration's gate legs: the 42 batteries repeat — fd-flat churn,
  stop/unwind reaping, refusals asserted verbatim — plus the
  iteration's own proof (PTY prompt bytes, SIGWINCH round trip, termios
  restore-after-trap, fd-across-socket byte echo).

## Sequencing (the graph remap this settles)

Pull transport cuts the old 1→2→3 chain: signals never needed PTY, only
the resize PAIRING. Edges now: **1 → 2** (spawn_pty extends spawn's
plumbing); **3, 4, 5 startable alone, today**; the VTE grid is wmux's
own `.wo` work, also standalone (a replay corpus needs no subprocess).
wmux 1 consumes all five plus the grid.

## Out of scope, by name

- PUSH delivery of child output — rejected above, revisit only with a
  consumer pull cannot serve.
- Death notices (`proc.watch`) — a two-line fiber composes `wait_dl`
  into an event; build it in `.wo`, not in C.
- termios knobs beyond raw/restore; multi-fd SCM_RIGHTS arrays;
  sequence-numbered signals; PTY termios shaping — each waits for a
  named consumer.
- SIGTERM/SIGINT user handling — permanently the engine's.
