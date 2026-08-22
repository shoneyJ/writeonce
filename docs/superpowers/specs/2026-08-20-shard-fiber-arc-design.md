# The 8+11 concurrency arc — shards, fibers, actors: design

> **Status: spec, awaiting review (2026-08-20).** The arc's decisions were
> settled in [iteration 8](../../stories/language-runtime-database/done/08-shard-actor-runtime.md)
> (refined) and the brainstorm of 2026-08-20 (this document's Decisions).
> Covers iterations 8 AND 11 as one arc; iteration 24 (chat) is its
> acceptance workload and gets its own spec after this one. The plan
> follows after review. Board: [docs/00-status.md](../../stories/00-status.md).
>
> Per repo convention: concept, reason, and required behavior in words
> only — no implementation code.

## Goal

The runtime scales past one core without ever showing a thread, a lock,
or a colored function: pinned thread-per-core shards whose only
communication is ownership-moving messages, cooperative fibers above them
preempted by reduction budget, blocking builtins that park instead of
block, and a database that stays single-writer by being runtime plumbing
on its owner shard. One `spawn`/`send` surface; app code byte-identical
at one shard or sixteen.

## Decisions (settled; the spec builds on them)

From the 2026-08-20 refinement: the DB is an actor on an owner shard
(never user-visible); 8+11 are one arc; one unified address surface;
chat (19) is the driving workload; order 22 → arc → 19 → 23.

From the arc brainstorm (2026-08-20):

1. **All cores by default.** `WO_SHARDS` / `[runtime] shards = N`
   override (N=1 is the debug/serial escape hatch); the arc's landing
   criterion is every standing gate green AT the default. Brave by
   choice: the gates become the stress test.
2. **Typed receive.** An actor is any class with `fn receive(msg: M)` —
   structural, the Handler doctrine, no blocking-receive keyword, no new
   interface to implement. `spawn Cls { ... }` infers M from the class's
   `receive` signature and returns an **`actor M`** address (a
   parametric builtin type exactly like `multi T`/`map<K, V>`).
   `send(addr, msg)` compile-checks that msg is an M and MOVES it.
3. **Transparent DB RPC.** insert/update/delete/query builtins executed
   on a non-owner shard marshal the statement to the owner shard and
   PARK the calling fiber until the reply; results come back
   materialized (they already do). App code never changes; the fourth
   iteration-8 criterion (byte-identical answers single- or multi-shard)
   is the proof.
4. **Fibers first, then shards.** Stage 1 ships fibers on one shard
   (framework's fiber-per-connection lands early, no threads); stage 2
   ships pinned threads + cross-shard send; stage 3 ships the DB RPC and
   re-runs 22. Each stage gates green independently.

## Part A — the fiber runtime (stage 1, one shard)

### The fiber context

A fiber IS the interpreter state `wo_vm` already isolates: the register
window (`WO_STACK_SLOTS`), the frame stack (`WO_MAX_FRAMES`), the catch
stack, the caught-error slot, and a resume point. Stage 1 extracts those
fields into a fiber context; `wo_vm` keeps the module, the runtime
(arena/GC/db — per SHARD, not per fiber), the current-fiber pointer, and
a FIFO run queue. Cost, stated: a context is ~42 KiB at today's
constants (32 KiB registers + frames + catches), so 1k fibers ≈ 42 MiB —
acceptable v1, arena-allocated; segmented/growable windows are recorded
future work, not v1.

### Scheduling

- FIFO run queue, no priorities (iteration 11's out-of-scope holds).
- **Preemption by reduction budget**: the budget decrements at the
  EXISTING safepoint sites (NEW, CALL, backward-JMP — the same places
  7b's GC polls, one counter check added, no new instrumentation); at
  zero the fiber re-queues and the next runs. Default budget 4000
  reductions, `WO_REDUCTIONS` overrides. Deterministic: same program,
  same schedule.
- `main` is fiber 0. When it returns, the program's exit value is
  main's; every other fiber — runnable or parked — is UNWOUND through
  the drop machinery (the SIGTERM/stop story, reused verbatim): ASan
  zero leaks is the criterion. A trap that unwinds out of a non-main
  fiber kills that fiber alone (its drops run, its error goes to stderr
  the uncaught-trap way); the program does not die.
- GC: every fiber's registers and frames are roots — `vm_gc_roots`
  iterates all contexts, not just the live one. Parked and queued
  fibers pin their values exactly like the running one.

### spawn / send (same-shard in stage 1)

- `spawn Cls { fields }` — an expression; Cls must declare
  `fn receive(msg: M)` where M is a class, record, or union type
  (**WO-E221** otherwise, naming what's missing); the ctor literal's
  fields are the actor's state, moved in (the take-push shape). Result
  type `actor M`.
- `send(addr, msg)`: msg must type as M (existing assign/param
  machinery) and be OWNED (the owner pass's transfer rules; a borrowed
  or copy-in-hand value is the existing WO-E3xx shape). After the send
  the sender's binding is dead — compile-time move, the iteration-8
  criterion.
- Delivery: the runtime calls the actee's `receive` with the message,
  one message at a time per actor, on the actor's home shard, as its
  fiber's next work item. An actor is a fiber that sleeps between
  messages; its mailbox is the FIFO the runtime owns.
- Same-heap sends hand the pointer over — no copy, no serialization;
  the move made aliasing impossible at compile time.
- `actor M` values are plain copyable words (an address), storable in
  fields/containers like Int — sending TO a dead actor is a silent drop
  of the message v1 (Erlang's shape; a delivery-receipt story is future
  work, recorded).

### The I/O plane: io_uring-first (AMENDED 2026-08-20, developer directive)

The original wording made epoll the v1 loop with io_uring as a
benchmark-gated swap. That framing is REVERSED by directive, and the
[linux reference project](../../plan/exploration/linux/07-io_uring.md)
already pointed here: io_uring is the successor to epoll + libaio for
BOTH the storage engine's WAL path and the socket path — one event loop.

- **One io_uring per shard thread** is THE event loop, serving three op
  families as the arc progresses: (1) fiber parking — `IORING_OP_POLL_ADD`
  for parked net fds, `IORING_OP_TIMEOUT` for sleeps, the mailbox eventfd
  registered the same way (stage 2's wake); (2) WAL durability — 23's
  WRITE + FSYNC(DATASYNC) chains ride the SAME ring, `user_data` the
  commit correlation, the shard executing while the ring drains; (3) the
  data path — multishot accept/recv/send for iteration 24's connection
  fan-in, LAST, because it changes resume semantics.
- **The readiness→ops ladder, staged honestly:** T4 uses the ring in
  READINESS mode (POLL_ADD/TIMEOUT — resume re-executes the now-ready
  builtin, the same retry contract epoll would have had, so the builtin
  layer barely changes). Submitting the reads/writes THEMSELVES as ring
  ops (completion carries the result, no retry) is the follow-up step —
  bigger builtin surgery, staged after T4 proves the ring loop.
- **Kernel floor and ops discipline:** io_uring is Linux 5.1+, mature
  5.11+; v1 uses only 5.1-safe ops (POLL_ADD, TIMEOUT, WRITE, FSYNC).
  Multishot accept (5.19) / multishot recv (6.0) are recorded
  optimizations for 19's spec, gated on the probe, never assumed.
- **The fallback is PORTABILITY, not preference:** a startup probe
  (`io_uring_setup`, fall back on ENOSYS/EPERM — seccomp'd containers
  routinely deny io_uring) selects the ring or the epoll+blocking-fsync
  path; `WO_IO=uring|epoll` overrides so CI proves BOTH paths on one
  kernel (this subsumes 23's `WO_WAL_MODE`). io_uring is the design;
  epoll exists so the binary runs everywhere — the project's premise.
- Raw `io_uring_setup`/`io_uring_enter` syscalls, libc-only (23's settled
  fork, now arc-wide); the mmap'd ring layout goes in the binding doc,
  normative, exactly like the WAL format.
- `net.accept`/`net.read`/`net.write` and `time.sleep` PARK the calling
  fiber against the ring; the shard runs other fibers; completion
  re-queues the parked one. With exactly one fiber the ring wait IS the
  blocking call — same code path, program mode is the one-fiber case.
- `fs.*` stays genuinely blocking in v1 (local disk, bounded); its reads
  become ring ops in the ops stage if 22's numbers ask; recorded.
- The stop story composes: the stop signal wakes the ring (the existing
  self-pipe/eventfd trick), every parked fiber unwinds — the SIGTERM
  drain criterion extends to fibers for free.
- The framework consequence (its own slice, after stage 1): the serve
  loop spawns a fiber per connection and the close-when-idle policy
  dies; that lands with iteration 24's spec, not this one.

### Fiber context growth (recorded improvement, post-stage-1)

A fiber context is ~42 KiB today (fixed register window + frame array).
Unlike native-stack green threads (Go's grow-by-copy must rewrite every
pointer into the old stack), this VM's "stack" is register-INDEXED, not
address-based — a context is relocatable by construction, so growth is
an allocate-larger + memcpy with zero pointer fixups. Start-small
(~4 KiB) growable contexts are therefore cheap to add and take fiber
counts from thousands toward millions; scheduled after stage 1, before
iteration 24's 1k-connection target if measurement asks.

## Part B — shards (stage 2)

- One pinned pthread per shard; shard count = cores by default
  (`WO_SHARDS`/manifest override). Each shard owns a full `wo_rt`:
  arena, traced list, GC — 7b's collector is per-shard by construction,
  so no global pause exists to remove.
- Cross-shard `send`: an MPSC mailbox ring per shard plus an eventfd to
  wake an idle shard's epoll loop (the c-runtime exploration's proven
  pair). The message POINTER crosses; the object's home shard is stamped
  in its header (there since iteration 2); its eventual free routes back
  to the allocation-home arena (a small home-free ring per shard,
  drained at the tick).
- What may cross: OWNED values whose class transitively contains no
  TRACED field — the `gc_may`-shaped fixpoint the runtime already
  computes, surfaced at compile time: sending a value whose type is or
  contains an inferred-traced class is **WO-E222**, naming the traced
  class and why it is traced. Aliased graphs never cross heaps.
- `spawn` placement: round-robin across shards by default; no placement
  argument in v1 (recorded future work if 22 shows a need). The
  language never names a shard.
- Determinism criterion narrows honestly: single-shard runs stay
  deterministic (stage 1's property); cross-shard interleaving is
  nondeterministic by nature — the corpus asserts OUTPUT SETS and
  ownership invariants under TSan, not byte-identical transcripts.

## Part C — the DB actor + serving (stage 3)

- The engine and WAL live on shard 0 (the owner). DB builtins executed
  on the owner run exactly today's code. On any other shard they
  marshal the statement into a runtime message, park the fiber, and
  resume with the materialized reply. `transaction { }` (iteration 18)
  marshals as one unit — the staged batch stays owner-side, semantics
  unchanged.
- The listener: `net.listen` + `net.accept` stay on the shard that
  calls them v1 (the framework serves from one accept loop; accepted
  connections' fibers stay on that shard). Distributing accept
  (SO_REUSEPORT per shard) is 19/23-adjacent future work the benchmark
  must justify — recorded, not built.
- Stage 3 closes the arc: `just employee`/`just web-app` run
  multi-shard byte-identical (criterion 4), the actor corpus runs under
  ASan+TSan, and 22 re-runs with the before/after recorded.

## Diagnostics (new)

- **WO-E221** — `spawn` on a class with no `receive(msg: M)` (or an M
  that is not a class/record/union).
- **WO-E222** — cross-shard send of a traced (or traced-containing)
  type; names the class and the inference reason. Compile-time; stage 1
  same-shard sends are exempt (same heap, aliasing is GC's problem and
  GC handles it).
- Existing machinery covers the rest: send-of-borrowed is the owner
  pass's transfer rules; send-type mismatch is the parameter boundary.

## Out of scope (arc-wide, restated)

Fiber migration across shards; priorities/timers/structured concurrency
(recipe-box, later `.wo` libraries); async/await (permanently rejected);
cross-shard transactions/2PC; SO_REUSEPORT accept distribution;
segmented fiber stacks; delivery receipts / actor supervision trees
(future story); program-mode API changes (none — one fiber is the
degenerate case).

## Success criteria

1. **Stage 1:** thousands of fibers on one shard, one hot loop among
   them — everything progresses (starvation-free fixture), output
   deterministic; a parked-fiber shutdown unwinds ASan-clean; a fiber
   blocked in `net.read` resumes while the shard served others, one TID.
2. **Stage 2:** the actor corpus under ASan+TSan — zero races, zero
   leaks; an owned send compiles as a move (sender's later use is a
   compile error); a traced-containing send is WO-E222; frees route
   home (asserted in debug builds).
3. **Stage 3:** `just employee` and `just web-app` green at the
   all-cores DEFAULT with byte-identical answers; 22's benchmark re-run
   with the delta recorded; every standing gate green.
4. The language grew exactly `spawn`, `send`, and the `actor M` type —
   no async/await, no locks, no thread ever visible.
