---
track: runtime-v2
iteration: "1"
status: pending
readiness: refine
---

# runtime-v2 1 — streaming subprocess: a long-lived child as a peer

> Part of [Story — runtime-v2: the runtime beyond sockets](00-story.md).
> Iteration [42](../language-runtime-database/42-bounded-subprocess.md)'s
> named follow-up, promoted to this track's opening slice. 42 built the
> machinery a streaming form reuses whole: the `wo_child` registry, the
> pidfd wait, the epoll bundle, the cap/refusal doctrine, the ownership
> sweeps (`fib_reap`/`wo_vm_destroy`/stop).
>
> **The problem.** `proc.run`/`proc.run_dl` are one-shot: nothing can
> talk to a child while it runs, and a child that legitimately runs for
> hours (a shell under wmux, a browser under the CDP driver, a script
> under skillhost) has no shape at all. Output must reach the owning
> actor as it happens, stdin must reach the child, and exit must arrive
> as an event — all without violating a single 42 guarantee.

## Info — the forks (open; why this is `refine`)

1. **Verb shape.** A new `proc.spawn(cmd, args, …)` returning a child
   HANDLE, versus spawn-options on `proc.run_dl`. Sub-fork: the handle
   as an actor address (the child looks like an actor — `send` to feed
   stdin, `monitor` for exit, iteration 24 machinery free) versus an
   opaque scalar with its own verb family.
2. **Output transport.** PUSH — stdout/stderr chunks delivered as
   `Bytes` messages into the owner's mailbox (the fd-event pattern) —
   versus PULL — a `read`-style verb the fiber parks on, the
   `net.read_dl` mould. Push composes with actors; pull composes with
   backpressure.
3. **The mailbox-cap collision** — the fork that decides whether push is
   viable at all: a chatty child versus `wo_mailbox_cap`'s fail-fast
   1024. Drop chunks? Kill the child by name? Or stop reading the pipe
   and let the KERNEL buffer be the backpressure (the lean — the child
   blocks on a full pipe exactly as it would under a slow tmux).
4. **stdin and its mirror.** Child not reading, pipe full: park the
   writing fiber with a deadline (the `net.write_dl` mould) versus
   refuse at a byte cap.
5. **Bounds semantics shift.** Total-output cap and total deadline stop
   meaning anything for a shell that runs for days — per-chunk caps and
   an IDLE deadline replace them; exit notification as a monitor-style
   death notice carrying the code, versus a blocking `wait` verb.

Ownership does not fork: 42's rule stands — the owner unwinding kills
the child; engine stop kills them all; a zombie or an orphan is a bug.

## Acceptance sketch (firmed at brainstorm)

- A child that emits a line a second: each line reaches the owning
  actor while the child lives — not after it exits.
- stdin round trip: feed a `cat`-like child, read the echo back.
- The chatty child against whatever fork 3 picked: the declared
  behavior happens, by name, and the shard never stalls.
- Exit: the owner learns the code as an event; the registry slot is
  released; fd count flat (the 42 measurement legs, streaming edition).
- Stop/unwind: identical guarantees to 42, proven with a LIVE stream.
- Gate legs land in `runtime/test/test_proc.c` beside 42's.

## Consumers

wmux 1 (the shell under every pane), skillhost 28 (stdin/stdout
transport was half its named gap), the zen study's CDP driver (spawn the
browser, then talk to it). The alacritty stage-A "headless PTY runner"
is this plus [runtime-v2 2](02-pty.md).
