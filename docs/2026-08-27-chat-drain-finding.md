# Iteration 24 T9 — the drain bug the gate was hiding

**Found 2026-08-27** while finishing T8/T9 on branch `chat-ws-lifecycle`.
Not fixed: the fix is an engine-level decision, recorded here so it is not
rediscovered.

## The symptom

`just chat`'s drain leg asserts both connected clients receive a WebSocket
close frame on `SIGTERM`. Against a **fresh** server it is flaky:

| Sample | Result |
| --- | --- |
| 5 fresh servers, 2 clients each | 4 × `close\|close`, 1 × `eof\|close` |
| 12 fresh servers | 3 failures, one of them `eof\|eof` |
| 16 fresh servers | 5 failures |

A failing client's socket reaches EOF with **no close frame and no
diagnostic** — the process exits and the kernel closes the fd.

## Why the gate never caught it

The drain leg did not start its own server. It inherited `$SRV` from the soak
leg — a server the soak had already pushed 1000 clients through, so every
shard was warm and every actor already scheduled. Draining a warm server hides
the cold-start race. Fixed in this change: **every leg now starts its own
server**, which is what exposed the bug.

## Root cause, traced

Instrumented the sample's actors (diagnostics not committed) and correlated
against failing runs:

1. `DIAG registry-shutdown rooms=1` — main's `send(reg, kind: 2)` **is**
   delivered and the Registry runs.
2. `DIAG room-shutdown` — **never printed on a failing run.** The Room never
   processes the `kind: 4` shutdown the Registry sends it.
3. The Writer's close branch never runs for the affected client, so no close
   frame is written and the fd is never closed by the Writer. Its
   `try net.write_dl(...)` is **not** failing — a diagnostic on that path
   printed zero times.
4. A client that *does* get a close frame is usually saved by its own
   **Reader** noticing `env.stopping()` and running its tail
   (`DIAG reader-tail bob r2=1`), not by the room broadcast.

So the drain chain is main → Registry → Room → Writer, three hops across
shards, and **the Room's shard does not reliably adopt its inbox before the
engine stops.**

## What was ruled out

- **Not the spin budget.** Replacing `spin < 20000000` with a wall-clock
  deadline of 1 s (`time.ticks()`) still failed 2 of 12. More time does not
  help, which is the strongest evidence the room's shard is not being
  scheduled at all rather than being scheduled late. That change was reverted:
  it fixed nothing and cost a fixed 1 s on every shutdown.
- **Not `dummy_writer()` spawning during shutdown.** Hoisting it to a
  Registry field spawned once at startup left 5 of 16 failing.
- **Not a write failure.** See point 3.

## The decision this needs

`main` cannot park after the stop flag (a park unwinds), so it spins — and
spinning is not a barrier. Either:

- **the engine drains pending inboxes before stopping**, so a `send` issued
  before the stop flag is guaranteed delivered; or
- **the sample gets a real barrier** — the drain is acknowledged back to main,
  which requires main to observe a reply without parking.

The first is the honest fix and belongs to the actor lifecycle (iteration 31,
absorbed into 24). It is a semantic guarantee — "a send before shutdown is
delivered" — not a tuning parameter, and it should be stated in the runtime's
lifecycle docs and pinned by a corpus fixture, not left to a spin count.

## Gate defects fixed alongside (all committed)

1. **fd check was core-count dependent.** `fds_before + 8` read lazy per-shard
   init as a leak: shards initialise on first fiber, each taking one
   `io_uring` + one `eventfd`, capped at `nproc`. On a 20-core box the first
   wave legitimately adds 18. Measured 26 → 44 after 20 clients, then **still
   44 after 40 more**. Replaced with the invariant the check is actually for:
   a second wave must not raise the count. Core-count independent, and it
   catches a slow leak that any fixed slack would hide.
2. **A failed leg orphaned its server.** The drain leg's python died on
   `int("")` when `$SRV` was empty, so the soak server was never killed and
   its listener broke the *next* run's soak on the same port. `cleanup` now
   kills every server a run started, matched on the run's unique temp dir.
3. **Two legs the plan requires were missing** — `WO_SHARDS=1` (the
   single-shard control that says a failure is placement's fault) and
   `WO_MAILBOX=8` (the drop-slow-member backpressure path). Both added, both
   green. The mailbox leg manufactures a genuinely slow member by shrinking
   its `SO_RCVBUF`, so it needs no sleeps.
