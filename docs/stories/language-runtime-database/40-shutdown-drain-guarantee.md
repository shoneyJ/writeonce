---
iteration: "40"
status: done
chain: 3
---

# iteration 40 — the shutdown drain guarantee: a send before the stop flag is delivered

> Part of [Story — one language, one runtime, one database, one binary](00-story.md).
>
> **Split out of [24](24-chat-websocket-workload.md) on 2026-08-27** because it
> is a runtime *semantic*, not a task in a sample's gate. It belongs to the
> actor lifecycle ([31](31-actor-lifecycle.md), absorbed into 24) and it is
> the half of "lifecycle" that nothing had stated: 31 gave actors a death
> notice, this gives the program a shutdown that does not lose mail.
>
> **Found by measurement, not review.** The chat gate's drain leg had been
> passing only because it drained a server the 1k soak had already warmed.
> Making every leg start its own server exposed it:
> [`2026-08-27-chat-drain-finding.md`](../../2026-08-27-chat-drain-finding.md).

## The rule

**A message sent before the stop flag is observed must be delivered and run
before the engine stops.** One sentence, and it is the whole iteration. It is a
guarantee, not a tuning parameter — which is why a spin count could never
express it.

What it does *not* promise: that a message sent *after* the flag is delivered,
that a parked fiber is resumed, or that an actor gets unbounded time. The drain
window is the primary's, and it closes when the primary returns.

## The bug, as measured

Fresh server, two WebSocket clients, `SIGTERM`, both must receive a close frame:

| Sample | Result |
| --- | --- |
| 5 fresh servers | 1 failure (`eof\|close`) |
| 12 fresh servers | 3 failures, one `eof\|eof` |
| 16 fresh servers | 5 failures |

The failing client's socket reaches EOF with **no close frame and no
diagnostic** — the process exits and the kernel closes the fd.

Traced with instrumentation on the sample's actors: `main` → Registry → Room →
Writer. The Registry runs and sees its room. The **Room never processes the
shutdown message**, so the Writer's close branch never runs. Clients that did
get a frame were saved by their own Reader noticing `env.stopping()`, not by the
room broadcast.

## The design, as built

`runtime/src/vm.c` already encoded the correct contract in `NEXT_RUNNABLE()`:
a worker that takes a stop while it has a live fiber returns 2 and **keeps
draining its inbox** until the primary sets `eng_shutdown`. Its comment says so
in as many words — "queued shutdown messages (close frames!) still run".

`shard_main`'s own idle branch contradicted it. A worker with an empty run queue
waits in `wo_io_wait`, and on `WO_IO_STOP` it called `fib_reap_all` and
**broke** — abandoning whatever was still in its inbox, which `wo_engine_stop`
then freed wholesale during teardown.

So the failure needed a shard that was *idle* at `SIGTERM`. A Room actor between
messages is exactly that, which is why the warm soak server hid it: warm shards
had live fibers and took the correct path.

The fix makes the idle branch obey the same contract: while the primary's drain
window is open, an idle worker adopts its inbox and runs what arrives, yielding
between empty polls so a drain cannot become a hot spin across every core. Only
`eng_shutdown` — set by the primary after `main` returns — ends it.

One branch, in one place, matching a contract the file already stated.

## Progress

| Piece | State |
| --- | --- |
| the idle-worker drain branch in `shard_main` (`runtime/src/vm.c`) | ✅ one branch, matching the contract `NEXT_RUNNABLE()` already stated |
| `sched_yield` on an empty poll so the drain cannot hot-spin | ✅ |
| fresh-server drain, repeated | ✅ **20 of 20**, from 5-in-16 failing |
| chat gate at the default 1k soak | ✅ **11 checks, 0 failures** — 1000/1000 clients, both `WO_IO` backends, ASan clean |
| full runtime battery (this touches every actor program's shard loop) | ✅ **36 suites** (18 × both dispatch flavors), 0 fail, `cli_smoke: OK`; compiler 556 checks 0 fail |
| the regression pin | ✅ the chat gate's drain leg, now that it starts its OWN (cold) server — that decoupling is what caught this. **Not** a corpus fixture or unit test: nothing in `runtime/test/` drives `wo_engine_start`/`wo_engine_stop` today, and no corpus fixture can trigger a stop, so pinning it below the gate means new multithreaded test infrastructure — named as its own cost, not smuggled in here |

**Measured 2026-08-27.** Before: 5 of 16 fresh-server drains left a client at
EOF. After: **20 of 20 clean.** At the observed failure rate, 20 clean runs by
luck would be about 0.04%, so this is the fix rather than a quieter race.

## Acceptance Criteria

Met:

- **Given** a fresh server with two connected WebSocket clients, **when** it is
  sent `SIGTERM`, **then** both clients receive a close frame — **repeatedly**,
  not once. The bug reproduced at 5 in 16, so a single green run proves nothing;
  the criterion is a run of at least 16 with zero failures.
  ✅ **20 of 20**, from 5-in-16 failing. A single run would have proved nothing.
- **Given** an actor whose shard is idle at the moment of the stop, **when** a
  message is sent to it before the stop flag is observed, **then** its
  `receive` runs before the engine stops. ✅ this is exactly the case that
  failed — the Room between messages — and it is what the branch now covers.
- **Given** the drain window, **when** a worker has nothing to adopt, **then**
  it does not hot-spin. ✅ `sched_yield()` on an empty poll; the 1k soak's RSS
  and timing legs are unchanged (marker reached all 1000 in 28 ms).
- **Given** `just chat`, **when** it runs at the default soak, **then** all
  legs pass on both `WO_IO` backends and under the ASan build with zero leaks.
  ✅ 11 checks, 0 failures. The fd leg also settled the lazy-init question at
  scale: **1000 connections left the count at 44**, unchanged after 20 more.
- **Given** the full runtime battery, **when** it runs, **then** no suite
  regresses — this touches the shard loop every actor program uses. ✅ 36 suites
  0 fail, plus the compiler's 556 checks.
- **Given** a program with no worker shards (`WO_SHARDS=1`), **when** it stops,
  **then** behaviour is unchanged. ✅ the gate's `WO_SHARDS=1` leg passes, and
  the branch is unreachable there — `wo_engine_stop` returns early at
  `nshards <= 1`, so a single-shard program never enters a worker loop.

Outstanding:

- **A pin below the gate.** The guarantee is currently proven by the chat gate
  only. Nothing in `runtime/test/` drives `wo_engine_start`/`wo_engine_stop`,
  and no corpus fixture can trigger a stop, so pinning it lower means new
  multithreaded test infrastructure. Named as its own cost rather than assumed
  cheap.

## Out Of Scope

- **Unbounded drain.** The window is the primary's and closes when `main`
  returns. A program that wants longer holds the window open itself.
- **Delivering sends issued *after* the stop flag.** Nothing promises that, and
  promising it would mean a program could refuse to exit.
- **Resuming parked fibers on stop.** `WO_SYS_STOPPED` unwinds them; that
  contract is iteration 24's and stays.
- **A shutdown acknowledgement in the language surface.** The alternative fix
  was a barrier the sample builds itself, rejected below.
- **`main` parking after the stop flag.** Still forbidden — a park after the
  flag unwinds. `main` still spins; the point is that spinning now works
  because the workers cooperate.

## Info — the forks, settled

1. **Engine guarantee, not a sample barrier.** The alternative was an
   acknowledged drain: rooms confirm back to `main`, which waits. Rejected —
   `main` cannot park after the stop flag, so it could only spin on the
   acknowledgement anyway, and every future actor program would have to
   re-implement the same handshake to avoid losing mail. A guarantee is stated
   once; a barrier is re-invented per program.
2. **Not the spin budget.** Replacing the sample's `spin < 20000000` with a 1 s
   wall-clock deadline still failed 2 of 12. More time cannot help when the
   shard is not scheduled at all, and the reverted attempt cost a fixed second
   on every shutdown. Recorded because a bigger spin is the obvious wrong fix.
3. **Not `dummy_writer()`.** Hoisting the shutdown message's placeholder actor
   out of the drain path (it spawned during shutdown) left 5 of 16 failing.
4. **Yield rather than spin in the idle drain.** A worker polling an empty
   inbox in a tight loop would burn a core per shard during the window and
   starve the actors being drained.
5. **Chain position 3**, with [31](31-actor-lifecycle.md): it is lifecycle
   semantics, and [24](24-chat-websocket-workload.md)'s gate is what proves it.
