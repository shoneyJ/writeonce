---
track: runtime-v2
iteration: "1"
status: done
readiness: ready
---

# runtime-v2 1 — streaming subprocess: a long-lived child as a peer

> Part of [Story — runtime-v2: the runtime beyond sockets](00-story.md).
> Spec: [`2026-09-01-runtime-v2-design.md`](../../superpowers/specs/2026-09-01-runtime-v2-design.md)
> (track-wide brainstorm, approved 2026-09-01 — the forks below are settled).
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

## Info — the forks, settled (brainstorm 2026-09-01)

1. **Verb shape: `proc.spawn(cmd, args) -> ?Child`** — a new verb, and
   the handle is a RECORD, not an actor: `Child {id, in, out, err}`,
   all Int-shaped, joining the predeclared records.
2. **Transport: PULL, and it already exists.** The child's fds are
   ordinary conn-like values; `net.read_dl`/`net.write_dl`/`net.close`
   drive them unchanged through the park plane. This iteration adds NO
   transport code at all — only acquisition, ownership and exit.
3. **The mailbox-cap collision never starts** — nothing is pushed. The
   kernel pipe is the backpressure: an owner that stops reading makes
   the child block on write, exactly tmux under a slow client.
4. **stdin mirror settled by the same move**: `net.write_dl` on
   `child.in` parks with a deadline; torn-write semantics as on sockets.
5. **Bounds:** the idle deadline is `read_dl`'s per-call ms; output caps
   are the caller's read sizes (no runtime buffer exists); exit is
   `proc.wait_dl(id, ms) -> ?Int` parking on the slot's pidfd — one
   waiter per id, a second refuses by name. `proc.signal(id, sig)`
   completes the surface. A death-notice verb is refused: a two-line
   fiber composes `wait_dl` into an event.

Ownership (stated, not forked): the spawning ACTOR owns the child (the
program, when spawned outside one). Actor death, engine stop and
`wo_vm_destroy` kill, reap and close — 42's doctrine, streaming edition.
Ceiling stays 32 per shard, failing closed by name.

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
