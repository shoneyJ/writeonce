# Stackless coroutines — the concurrency contract (fibers, parking, no `async`)

Normative reference for the concurrency iterations (the 8+11 arc and its
chain). Landed by the arc's stages 1+2 (branch `concurrency-arc`,
2026-08-20); the mechanism lives in `runtime/src/vm.h` (`wo_fiber`,
`wo_vm`), `runtime/src/vm.c` (scheduler), `runtime/src/park.c` (I/O
plane). Shard/actor rules live in the arc spec
([`2026-08-20-shard-fiber-arc-design.md`](../../superpowers/specs/2026-08-20-shard-fiber-arc-design.md))
— this doc is the coroutine core plus its seams.

## 1. The model — a coroutine is interpreter state, not a stack

A fiber IS the VM's per-execution state, made per-fiber: one heap
struct (`wo_fiber`) holding the register window, the frame stack, the
catch stack, the saved resume pc, and the park descriptor. That is the
whole coroutine.

**Stackless** means: no native C stack per fiber, ever. The interpreter
runs every fiber on the one OS thread stack of its shard; suspending a
fiber never saves a C stack because there is nothing on it — suspension
exists only at VM boundaries:

- the dispatch loop (reduction budget hits zero), and
- inside a blocking builtin (the builtin fills the park descriptor and
  returns to the scheduler).

C code between those boundaries never yields; therefore no
`makecontext`/`ucontext`, no split stacks, no stack copying, no guard
pages. Consequences the design buys:

| property | why it follows |
| --- | --- |
| spawn is one allocation | a fiber is a calloc'd struct, not an 8 MiB mapping |
| deterministic replay | suspension points are exact VM instructions, not signal arrival |
| trivial GC rooting | a fiber's roots are its registers + frames arrays — walkable structs |
| no FFI hazard | there is no foreign frame that could hold a suspended C stack |

## 2. No `async` keyword — the no-coloring rule

There is ONE function type. The keyword `async` (and `await`) is a
**permanently rejected surface**, not deferred (story 11's
out-of-scope). The rule that replaces it:

- **Blocking builtins park instead of block on server shards.** The same
  `net`/`time` call that blocks the thread in program mode hands its fd
  or deadline to the shard's I/O plane and parks the calling fiber; the
  shard serves other fibers meanwhile; the fiber resumes with the
  result. Same source text, no annotation, no second color of function.
- **Program mode stays single-fiber and genuinely blocking** — the
  log-watcher needs nothing more; the parked path is the serving path.
- Preemption is by **reduction budget**, never signals or safepoint
  interrupts: the dispatch loop decrements a countdown and parks the
  fiber at zero. Erlang's shape.

Precedent survey (kernel evidence, BEAM adopted, Go stack copying and
Tokio coloring rejected):
[`docs/plan/exploration/fibers/00-fibers.md`](../exploration/fibers/00-fibers.md).

## 3. The fiber state machine

States (`wo_fib_state`): **RUNNABLE → PARKED → RUNNABLE → … → DONE**.

- **RUNNABLE** — on the shard's FIFO run queue (`qhead`/`qtail`,
  intrusive `next` link). FIFO is the v1 fairness policy; no priorities.
- **PARKED** — on the parked list (`pnext` link), registered with the
  I/O plane as one wait (fd readiness or deadline).
- **DONE** — returned or trapped; reaped.

Fixed points of the machine:

- **Fiber 0 is main**, embedded in the `wo_vm` (never allocated, never
  reaped before shutdown). Spawned fibers are calloc'd; `nfibers`
  counts them.
- **`vm->cur` is the live fiber** — every interpreter access reads
  through it. One live fiber per shard at any instant.
- **Trap isolation**: an uncaught trap kills the trapping fiber alone —
  it unwinds through its own drop maps and goes DONE; the shard and the
  other fibers continue.
- **Main-return reap**: when main returns, remaining fibers are
  unwound (drop maps run — parked fibers die as cleanly as trapped
  ones) and the program ends.
- **Actor delivery fibers**: an actor executes messages on a fiber with
  `fiber->actor` set; one message at a time (the actor guarantee);
  `cur_msg` is runtime-owned and dropped after the receive call
  returns. Actor rules beyond delivery are the arc spec's.

## 4. Suspension and resume — the exact protocols

**Reduction budget.** `budget0` reductions per slice (`WO_REDUCTIONS`,
default 4000). The countdown decrements at loop **BACK-EDGES ONLY,
after the jump lands** — not at the "same three sites as the GC". The
distinction is load-bearing: a pre-instruction decrement re-executes
the jump into the same decrement at budget 1 and livelocks (pinned by
the deterministic fiber corpus). At zero the fiber re-queues RUNNABLE
at the FIFO tail and the next fiber runs.

**Parking on an fd** (accept/read/write not ready): the builtin fills
the park descriptor — `park_fd`, `park_events` (POLLIN/POLLOUT),
`park_done = 0` — and the plane arms a wait. `park_done = 0` means
resume **RE-EXECUTES the builtin**: the retry runs the same call now
that the fd is ready. A partial `net.write`'s progress crosses the
retry in `park_wr_at` (the write offset) — re-execution continues the
write, never restarts it.

**Parking on a deadline** (`time.sleep`): `park_fd = -1`,
`park_deadline` set, the result preset before parking, `park_done = 1`
— resume **continues PAST the builtin**. `park_ts` must outlive the
ring submission (the TIMEOUT op reads it asynchronously).

**The I/O plane** (`park.c`): one event loop per shard — parked fibers'
waits and the shard's inbox eventfd on the SAME loop. io_uring FIRST
(raw `io_uring_setup`/`io_uring_enter`, POLL_ADD + TIMEOUT at the Linux
5.4 op floor, libc-only); epoll is the portability fallback behind a
startup probe (seccomp'd containers routinely deny io_uring), forced
either way with `WO_IO=uring|epoll` so CI proves both paths on one
kernel. Note: the epoll fallback here predates the 2026-08-21
epoll-discard directive for PLANS — the runtime keeps the probe until a
removal slice says otherwise.

**Stop**: the stop flag interrupting the plane's wait unwinds
EVERYTHING — every fiber, parked included, through its drop maps; a
stop is not a trap and cannot be caught.

## 5. Memory and GC seams

- **Roots**: `vm_gc_roots` iterates ALL fibers' registers + frames —
  parked fibers' frames are roots exactly as the live frame stack is.
  Nothing live is collected while its only reference sits in a parked
  fiber.
- **Drops**: every exit path (return, trap, stop-unwind, main-return
  reap) runs the same drop maps; ASan-zero-leaks is the standing gate.
- **Open question** (tracked in story 11): how a parked fiber's borrow
  state interacts with the shard's GC safepoints — settles with stage 3
  or the collector's next pass.

## 6. Seams — pointers, not content

| concern | where it is normative |
| --- | --- |
| shards, envelopes, ownership-move sends, WO-E221/E222, placement | [arc spec](../../superpowers/specs/2026-08-20-shard-fiber-arc-design.md) + [arc plan deviations](../../superpowers/plans/2026-08-20-shard-fiber-arc.md) |
| request/response, bounded mailboxes, actor death, timers | [iteration 31](../../stories/language-runtime-database/refine/31-actor-lifecycle.md) — not built yet |
| the DB actor (stage 3) | [in-progress marker](../../in-progress/2026-08-21-arc-stage-3.md) |
| builtin ids and their park behavior | [`08-builtin-surface.md`](08-builtin-surface.md) |

## 7. Rejected alternatives — settled, argue against the reason

| rejected | reason |
| --- | --- |
| **`async`/`await`, function coloring** | splits the world into two function types and infects every caller; the park-under-blocking-API posture serves the same need with zero surface. Permanent. |
| **Stackful coroutines (`ucontext`/`makecontext`, per-fiber C stacks)** | pays a stack (or guard-page games) per fiber, breaks the one-allocation spawn, and reintroduces foreign-frame suspension the GC would have to scan blind. |
| **Go-style segmented/copied stacks** | stack copying needs precise pointer maps for native frames — a moving-stack machinery this VM does not need because fibers never own native frames. |
| **Signal/safepoint preemption** | signals arrive between ANY two instructions — kills deterministic replay and demands async-signal-safe everything; the reduction budget preempts at exact VM points. |
| **Per-instruction budget decrement** | measurable dispatch cost for zero fairness gain; back-edges bound every loop already (and the pre-instruction variant livelocks at budget 1). |
