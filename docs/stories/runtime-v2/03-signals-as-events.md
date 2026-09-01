---
track: runtime-v2
iteration: "3"
status: pending
readiness: ready
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

## Info — the forks, settled (brainstorm 2026-09-01; spec:
[`2026-09-01-runtime-v2-design.md`](../../superpowers/specs/2026-09-01-runtime-v2-design.md))

1. **`signal.on(sig, addr)`** — a standing subscription delivering the
   SIGNAL NUMBER as a scalar message. No moved-message ownership rules:
   scalars copy, so a repeating SIGWINCH needs no per-delivery payload.
2. **signalfd on shard 0's plane** — one more registered fd with a
   sentinel `user_data` (the wake-eventfd precedent); reads drain
   `signalfd_siginfo` records and fan out ordinary sends.
3. **Offerable set: SIGWINCH, SIGCHLD, SIGHUP, SIGUSR1, SIGUSR2.**
   SIGTERM/SIGINT registration is refused by name — the stop latch
   stays the engine's, iteration 40 is load-bearing.
4. **Coalescing disclosed, not sequenced** — what SIGWINCH consumers
   expect anyway.

Also settled: this iteration is STANDALONE — the old edge from PTY was
only the resize pairing; signalfd needs nothing from iteration 2.

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
