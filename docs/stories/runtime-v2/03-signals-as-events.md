---
track: runtime-v2
iteration: "3"
status: pending
readiness: refine
---

# runtime-v2 3 — signals as events: SIGWINCH into a mailbox

> Part of [Story — runtime-v2: the runtime beyond sockets](00-story.md).
> The seam iteration
> [42](../language-runtime-database/42-bounded-subprocess.md)
> deliberately deferred "to its real consumer" — this is that consumer
> arriving. A terminal application's resize IS a signal (SIGWINCH), and
> nothing today can turn a signal into anything a program observes
> except the SIGTERM/SIGINT stop latch.
>
> **The problem.** Signals are process-global, delivered on an arbitrary
> thread, and allowed to do almost nothing — the exact opposite of a
> shard-owned mailbox message. The runtime already crossed this bridge
> once: the stop flag is a signal made safe by latching. This iteration
> generalizes that shape without handing user code a signal handler.

## Info — the forks (open)

1. **Registration surface.** `signal.on(SIGWINCH, addr, msg)` in the
   `time.after` mould (the msg MOVES to the runtime, delivered on
   arrival) versus a process-level subscription table in `wo.toml`.
   Lean: the builtin — dynamic, one consumer today.
2. **Delivery mechanics.** `signalfd` on shard 0's plane (a signal
   becomes an fd event — no async-signal-safety questions at all, the
   kernel-primitive taste) versus a latch array swept like deadlines.
   Lean: signalfd; the runtime is Linux-first and the plane already
   multiplexes fds.
3. **Which signals are offerable.** SIGWINCH and SIGCHLD certainly;
   SIGTERM/SIGINT stay the ENGINE's (the stop latch is load-bearing —
   iteration 40's drain). The fork is whether user registration for the
   stop signals is refused by name or layered before the latch.
4. **Coalescing.** Signals coalesce in the kernel; a mailbox message per
   delivery can't promise one-per-resize. Disclose coalescing (lean —
   it is what SIGWINCH consumers expect anyway) versus sequence-number
   them.

## Acceptance sketch

- Resize the controlling terminal of a test child: the registered actor
  receives the message; the grid-owning code calls
  [2](02-pty.md)'s resize onward — the wmux wiring, proven in miniature.
- SIGCHLD registration does not disturb 42's pidfd machinery (they
  coexist; the pidfd stays the reap path).
- SIGTERM still stops the engine with the full drain — the iteration 40
  battery unchanged.

## Consumers

wmux 1 (SIGWINCH fan-out to panes). Everything else can wait — this
iteration exists exactly once a real consumer does, per 42's deferral.
