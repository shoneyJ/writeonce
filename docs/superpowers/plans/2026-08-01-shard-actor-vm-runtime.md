# Shard-Actor VM Runtime Implementation Plan

> **Status: ✖ DISCARDED 2026-08-21** (developer decision) — superseded by
> the arc plan of record,
> [`2026-08-20-shard-fiber-arc.md`](2026-08-20-shard-fiber-arc.md), whose
> stages 1+2 landed 2026-08-20. This plan's premise is inverted twice: it
> builds on "epoll now / io_uring when the loop module ports phase C"
> (the io_uring-first directive made the ring primary, and the epoll-based
> approach is discarded), and it ports patterns from `runtime/wo-rt.c`, a
> tree removed 2026-08-18 with the Rust track. Kept as historical
> reference for the mailbox-ring and heap-stamping idea shapes only.
> Recorded in [`plan/discarded.md`](../../plan/discarded.md). Board:
> [00-status.md](../../00-status.md)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
>
> **Style rule (user convention):** concept, reason, and required behavior in words only; the executor writes the code.

**Goal:** Sub-project 2 of the OOP spec — multithread the VM: one pinned worker per core, one VM heap per shard, `spawn`/`send` in the language with ownership-transfer semantics, and GC pacing on the event-loop tick — no locks on the data path, no stop-the-world, ever.

**Architecture:** Plan 4 of 7. Depends on plans 1–3 (wovm, woc, emit/corpus). The concurrency substrate already exists and is *shipped*: `runtime/wo-rt.c` phases A–F (thread-per-core pthreads pinned via sched_setaffinity, SO_REUSEPORT accept spreading, per-thread io_uring loops, one mmap arena sharded by address, per-shard WAL + boot replay — see `docs/plan/exploration/c-runtime/00-plan.md`, all exit criteria met). This plan does NOT rewrite that; it ports the patterns into `runtime/src/` modules and mounts one `wo_vm` per shard on top. `wo-rt.c` stays the untouched reference.

**Tech Stack:** C11 + libc, raw syscalls (pthreads, sched_setaffinity, eventfd, epoll now / io_uring when the loop module ports phase C). C11 atomics allowed ONLY in the mailbox ring — the data path stays lock-free and atomic-free per doctrine.

## Global Constraints

- All plan-1 constraints carry over (libc only, no commits — drafts to `.dev/commit.md`, ASan gate, docs under `docs/`).
- **Thread-per-core, shared-nothing** (plan 09 doctrine, c-runtime decision 1): no work stealing, no connection migration, no locks/atomics on the data path.
- **One heap per shard:** every `wo_vm` owns its arena; an object's `shard_id` header field (reserved since plan 1) names its owner. Cross-shard access is always a message, never a pointer dereference.
- **Send = ownership move, not copy** (spec concurrency decision): transferred objects hand over the pointer; memory returns to its origin shard's arena for freeing.
- **Jobs never block; waiters pump their own inbox** (deadlock-freedom doctrine from `crates/rt` shard bus).
- **GC is per-shard and budgeted per tick** — replacing plan 3's post-exit pump; no cross-shard tracing exists by construction.
- **ThreadSanitizer joins the gate:** the mailbox ring and shutdown path must be TSan-clean in addition to ASan.

---

## File Structure

```
runtime/src/
  sched.c sched.h        worker spawn/pin/join, signalfd shutdown broadcast (Task 1)
  mailbox.c mailbox.h    MPSC ring + mail eventfd per shard (Task 3)
  shard.c shard.h        per-shard state: vm, loop, mailbox, gc pacing (Tasks 2, 5)
compiler/src/            grammar/typing/owner additions for spawn/send (Task 6)
tests/corpus/actor/      concurrency fixtures (Task 7)
docs/plan/oop-vm/03-shard-actor.md   the runtime contract this plan pins (Task 1)
```

---

### Task 1: Scheduler skeleton + contract doc

**Concept & reason:** port phase A's shipped pattern into modules: `WO_THREADS` workers (default = online cores), each pinned, each owning an event loop and — new — one `wo_vm` instance with its own heap. Signals blocked before spawn; worker 0 owns the signalfd and broadcasts shutdown via per-worker eventfds; clean join. The contract doc records what every later task obeys: shard ownership rules, mailbox semantics, send-as-move, free-routing, gc pacing — the C sibling of plan 09's doctrine section.

- [ ] Failing test: a harness boots N workers, proves pinning (`/proc/self/task/*/stat` cpu affinity), proves per-worker VM isolation (each runs a trivial method concurrently, results independent), proves clean SIGTERM join.
- [ ] Implement; TSan + ASan clean. Write the contract doc.
- [ ] Record commit draft: `feat(runtime): shard scheduler — pinned workers each owning a wo_vm, signalfd shutdown broadcast (phase-A port into src/); docs/plan/oop-vm/03-shard-actor.md contract.`

### Task 2: Per-shard heaps + ownership stamping

**Concept & reason:** every allocation stamps the allocating shard's id into the header (plan 1 reserved the field, always 0 until now). Debug builds assert on every object access that the toucher is the owner shard — turning silent cross-shard races into loud failures during development; release builds compile the assert out. The arena stays per-VM as plan 1 built it; what this task adds is identity and enforcement.

- [ ] Failing test: allocation on shard *t* stamps *t*; a rigged cross-shard touch aborts under the debug assert; release build tolerates the same touch (documented as UB caught only in debug).
- [ ] Implement; green.
- [ ] Record commit draft: `feat(runtime): shard ownership — allocations stamp shard_id, debug-build access asserts turn cross-shard races into aborts.`

### Task 3: Mailboxes

**Concept & reason:** the only cross-thread structure in the system, so it carries the whole correctness burden. Per-shard MPSC ring (fixed capacity, C11 atomics for the producer side, single consumer = the owner shard) plus a mail eventfd the owner's event loop watches — the same wake pattern the Rust shard bus and phase A's shutdown already use. Messages are small fixed structs: kind, sender shard, payload words. Full ring applies backpressure by failing the send — the sender re-queues or traps — never by blocking (doctrine). Two message kinds exist from day one: USER (a sent value) and FREE (route a transferred object's memory home, Task 4).

- [ ] Failing tests: single-producer and multi-producer ordering; full-ring send fails without blocking; eventfd wake fires exactly when an empty ring becomes non-empty; TSan-clean under a hammering stress test.
- [ ] Implement; green under ASan + TSan.
- [ ] Record commit draft: `feat(runtime): per-shard MPSC mailbox ring + mail eventfd — non-blocking sends with backpressure, USER/FREE message kinds; TSan-hammered.`

### Task 4: Send as ownership move + free routing

**Concept & reason:** the spec's "no copy, no locks" promise made mechanical. Sending an owned object: the sender's register is dead after the send (compiler enforces — Task 6), the pointer travels in the message, the receiver restamps `shard_id` to itself. The memory, though, still belongs to the origin shard's arena — freeing it on the wrong thread would corrupt the free lists. So `wo_drop_any` grows a routing check: an object whose *allocation home* (recorded beside the arena, not the mutable `shard_id`) differs from the current shard is not freed in place — a FREE message carries it home, and the origin shard frees it on its next tick. `@gc` objects do not transfer in milestone one: sending one is a compile error (Task 6) — cross-shard reference counting is a distributed-GC problem the spec's model deliberately avoids.

- [ ] Failing tests: transfer round-trip (shard A builds an owned graph, sends to B, B reads/mutates/drops it, memory returns to A's arena — ASan-proven across a full drain-and-join); FREE routing under load; deep graphs transfer whole (children move with the root).
- [ ] Implement; green.
- [ ] Record commit draft: `feat(runtime): ownership-transfer send — pointer handoff with shard restamp, allocation-home free routing via FREE messages, @gc transfer excluded; ASan-proven round-trips.`

### Task 5: GC pacing on the loop tick

**Concept & reason:** the spec's no-stop-the-world claim becomes an event-loop property. Each shard's loop calls the budgeted collector step (plan 1's `wo_gc_step` semantics) once per tick when the candidate buffer is non-empty, budget from configuration, between draining I/O and draining the mailbox — so collection interleaves with work and its cost per tick is bounded by construction. Plan 3's post-exit pump in the CLI stays for single-shot `wovm file.wob` runs; the scheduler path supersedes it. Stats surface per shard: candidates, freed, steps, max step time.

- [ ] Failing test: a shard under continuous method traffic that churns `@gc` cycles keeps collecting (buffer drains over ticks) while request latencies stay bounded — the test asserts the buffer does not grow monotonically and no single tick exceeds the budgeted slice by more than the documented component overshoot.
- [ ] Implement; green.
- [ ] Record commit draft: `feat(runtime): gc pacing — budgeted collector step per shard loop tick between io and mailbox drains, per-shard stats; latency-bounded under cycle churn.`

### Task 6: Language surface — `spawn` and `send`

**Concept & reason:** the smallest surface that exposes the model, mirroring the spec's deferral note. `spawn free_fn(args)` schedules a free fn onto a chosen-by-runtime shard (round-robin), arguments move; `send(handle, value)` moves a value to the shard owning `handle`; `receive` binds delivered values in a free fn designated as an actor body. Grammar keeps the repo's keyword discipline (these parse positionally where possible). The typechecker forbids sending `@gc` values and borrow-mode parameters across; the owner pass treats send/spawn argument positions exactly like `take` (move sites, two-site errors on reuse). The emitter lowers to SEND/SPAWN builtins (builtin table extension — the format doc gains the ids; no opcode change, no version bump).

- [ ] Failing tests: golden parses; must-fail fixtures (use-after-send, sending @gc, sending a borrow); emit goldens for the new builtins; format doc updated in the same change.
- [ ] Implement compiler + wovm builtin halves; green.
- [ ] Record commit draft: `feat: spawn/send surface — parser/typing/owner treat send args as moves (@gc and borrows rejected), SEND/SPAWN builtins in wovm + format doc; ownership corpus grows the send suite.`

### Task 7: Concurrency corpus + acceptance

**Concept & reason:** determinism first: fixtures pin `WO_THREADS`, and assertions observe *joined* results (a spawner collects replies and prints a sorted summary), never interleaving-dependent output. Suite: ping-pong transfer between two shards; fan-out/fan-in over all shards; transfer-heavy stress under ASan and TSan; a mailbox-backpressure fixture proving non-blocking behavior surfaces as a trap, not a hang. Acceptance extends `just oop-accept` with the actor corpus and the TSan gate; the contract doc gets the measured evidence appended, in the style the c-runtime plan's phases record their exits.

- [ ] Add fixtures + gate wiring; all green; evidence recorded.
- [ ] Record commit draft: `test(corpus): actor suite — deterministic ping-pong/fan-in/stress under ASan+TSan, backpressure-as-trap fixture; oop-accept gains the concurrency gate.`

---

## Plan self-review notes

- **Spec coverage (sub-project 2):** shard-actor with per-shard heaps, ownership-transfer sends, no-GC-pause-by-construction, header shard id activated, deadlock-freedom doctrine — all tasked. Deliberately excluded, stated where: cross-shard `@gc` (rejected at compile time), cross-shard transactions (Rust plan 09e territory), io_uring port of the VM loop (the phase-C pattern exists; this plan runs on the epoll loop and the port is a later mechanical swap).
- **Order rationale:** scheduler before heaps before mailboxes before send (each is the next one's substrate); language surface only after the runtime halves exist; corpus last.
- **Risk called out:** free-routing (Task 4) is the subtle piece — its tests are the ones ASan must own end to end.
