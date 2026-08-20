# fibers — the hybrid scheduler, demonstrated

The smallest program that shows all three legs of the concurrency model
(the doctrine-depth writeup lives in
[`docs/plan/exploration/fibers/00-fibers.md`](../../plan/exploration/fibers/00-fibers.md)):

1. **Reduction-budget eviction** — part 1's output is byte-exact and
   timing-free: main sends three messages, then burns reductions; every
   time its budget expires the counter actor gets a turn and delivers
   exactly one message. Cooperative mechanics, preemptive fairness.
2. **Actors as the spawn surface** — `spawn Counter { ... }` returns an
   `actor Tick`; `send` moves the message (using it afterwards is a
   compile error); delivery is one message at a time per actor.
3. **Parking, not blocking** — part 2's sleeper actor calls `time.sleep`
   mid-receive: the fiber parks on the shard's I/O plane (io_uring
   primary, epoll fallback — `WO_IO=uring|epoll` forces either) and
   main keeps ticking while it sleeps.

Run: `just fibers` (the acceptance gate builds it, checks part 1
byte-exact, asserts part 2's ordering invariants, and repeats the run on
both I/O backends plus ASan).
