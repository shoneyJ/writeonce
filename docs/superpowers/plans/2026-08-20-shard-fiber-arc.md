# The 8+11 concurrency arc — implementation plan (staged)

> **Status: STAGES 1 AND 2 COMPLETE 2026-08-20** (branch `concurrency-arc`,
> T1–T4 landed + the `docs/examples/fibers` demo and its `just fibers`
> gate, 8/0). Stages 2–3 pending. Execution deviations, disclosed:
> (1) the reduction budget decrements at loop BACK-EDGES ONLY, after the
> jump lands — the spec's "same three sites as the GC" wording had a
> livelock at budget 1 (pre-instruction save re-executes the jump into
> the same decrement; pinned by test_fiber's exact round-robin);
> (2) T4 landed io_uring-FIRST per the amended spec (park.c: raw ring,
> POLL_ADD/TIMEOUT, WO_IO=uring|epoll, epoll fallback) and the battery
> ran green on BOTH backends plus LW_SOAK over parked I/O;
> (3) a partial net.write's progress crosses the park via park_wr_at —
> the retry protocol needed write-offset state the spec never named.
> Stage-2 deviations: (4) the inbox is a MUTEX-guarded list + eventfd,
> not the spec's lock-free MPSC ring — rings arrive when 22 measures the
> mutex; (5) WO-E222 applies to EVERY spawn/send (round-robin placement
> makes any actor potentially remote — compile time cannot see the
> shard); (6) the deterministic corpus pins WO_SHARDS=1, and the
> multi-shard truth (output SETS, TSan, both I/O backends) lives in the
> fibers gate — the spec's narrowed-determinism rule made operational;
> (7) two TSan-caught races fixed (late-init memset vs concurrent push;
> wake-efd read outside the lock) and one teardown SEGV (routed frees
> during teardown are now no-ops: arenas die wholesale).
> Board: [docs/00-status.md](../../stories/00-status.md).

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans task-by-task. Steps use checkbox syntax.
>
> **Style rule (user convention):** concept, reason, required behavior in
> words plus verification commands only — the executor writes the code.

**Goal:** the spec's three stages — fibers on one shard, shards with
ownership-moving sends, the transparent DB actor — each landing with
every standing gate green before the next begins.

**Architecture:** see the spec (normative):
[`../specs/2026-08-20-shard-fiber-arc-design.md`](../specs/2026-08-20-shard-fiber-arc-design.md).
Stories: [8](../../stories/language-runtime-database/in-progress/08-shard-actor-runtime.md) ·
[11](../../stories/language-runtime-database/in-progress/11-fibers.md).

**Tech Stack:** C11 libc-only (`wovm`), OCaml stdlib-only (`woc`), bash
gates; TSan added to the corpus harness at stage 2.

## Global Constraints

- Branch `concurrency-arc` off master; commits local only, never push.
- A stage does not start until the previous stage's FULL battery is
  green: `just woc-test`, `just oop-e2e` (ASan stage included),
  `just deps-accept`, `just web-app`, `just log-watcher`,
  `just employee`.
- Stage 1 changes NO observable behavior for single-fiber programs —
  every existing fixture byte-identical; that is the refactor's proof.
- New E-codes: WO-E221 (spawn needs `receive(msg: M)`), WO-E222
  (traced-containing cross-shard send). `.wob` format: spawn/send lower
  to new BUILTIN ids — no new opcodes; the version bumps ONLY if the
  actor-type field encoding forces it (decide in T3, disclose).

---

## Stage 1 — fibers on one shard

### Task 1 — the fiber context extraction (pure refactor)

**Files:** `runtime/src/vm.h`, `runtime/src/vm.c`, `runtime/src/gc.c`
(`vm_gc_roots`), `runtime/src/main.c`, `runtime/test/*` (compile fixes
only).

**Interfaces:** produces `wo_fiber` (registers, frames, depth, catch
stack, caught error, resume pc/state) and `wo_vm` holding module + rt +
current-fiber + a fiber list; everything else consumes `vm->cur`.

- [x] Move the five interpreter-state fields into the fiber struct; the
  vm allocates fiber 0 (main) at init; `vm_run`/`wo_vm_call`/unwind/
  trap paths read through the current-fiber pointer.
- [x] `vm_gc_roots` iterates ALL fibers' registers+frames (one fiber
  today — the loop is the change).
- [x] Verify: full battery byte-identical (this task has NO functional
  change to hide behind). Commit.

### Task 2 — run queue + reduction budget

**Files:** `runtime/src/vm.c`, `runtime/src/vm.h`.

**Interfaces:** produces fiber states (RUNNABLE/PARKED/DONE), a FIFO
queue, `WO_REDUCTIONS` (default 4000), and a scheduler loop around the
dispatch loop; consumes Task 1's context switch (save/restore = swap the
current-fiber pointer).

- [x] Budget decrements — AMENDED to back-edges only (deviation 1); originally the GC-safepoint sites (NEW, CALL,
  backward-JMP); zero → re-queue + switch. One fiber = no observable
  change (it re-queues to itself).
- [x] A trap unwinding out of a non-main fiber kills that fiber alone
  (drops run, uncaught-trap report to stderr, program lives); main's
  return ends the program and unwinds every remaining fiber through the
  stop path (drop-clean).
- [x] C unit test (`runtime/test/test_fiber.c`): two hand-built fibers
  interleave deterministically under a tiny budget; kill-on-main-return
  leaks nothing under ASan. Full battery. Commit.

### Task 3 — spawn / send / `actor M` (language + same-shard delivery)

**Files:** `compiler/src/{token,lexer,ast,parser,types,owner,emit,dump}.ml`,
`runtime/src/{wob.h,builtin.c,vm.c}`, corpus fixtures.

**Interfaces:** produces the `actor M` field-type shape (parametric like
`multi T`), `spawn Cls { ... }` expression (M inferred from Cls's
`receive`; WO-E221 otherwise), `send(addr, msg)` (msg typed M, owner
pass TRANSFERS it — sender's later use is the existing use-after-move
diagnostic), and runtime delivery: an actor is a fiber that sleeps
between messages, mailbox FIFO, one `receive` call per message.

- [x] Type/AST/parse: `actor M` in field positions; spawn/send keywords
  (grep the corpus for identifier collisions first, the standing
  keyword discipline).
- [x] Owner: send's message argument is a transfer (the take machinery);
  spawn's ctor literal moves its field values (existing ctor rules).
- [x] Emit + runtime: two builtin ids; spawn creates the actor fiber +
  returns the address as a scalar word; send enqueues + wakes.
  Addresses are copyable scalars; send-to-dead = silent drop (v1,
  spec'd).
- [x] Corpus: `run/actor-echo` (spawn, send, receive mutates actor
  state, deterministic output), `run/actor-move` (sender's reuse after
  send = compile-fail fixture, actually its own
  `compile-fail/send-after-move`), `compile-fail/spawn-no-receive`
  (WO-E221). ASan. Full battery. Commit.

### Task 4 — parking builtins on the shard's io_uring loop (AMENDED)

**Files:** `runtime/src/builtin.c` (net/time cases), `runtime/src/vm.c`
(the per-shard ring the scheduler idles in), `runtime/src/sysio.c`.

**Interfaces:** produces PARKED fibers keyed by ring `user_data`;
consumes Task 2's states. AMENDED 2026-08-20 (developer directive +
the linux reference project): the loop is **io_uring in readiness mode**
(POLL_ADD for fds, TIMEOUT for sleeps; resume re-executes the now-ready
builtin), with the startup probe + `WO_IO=uring|epoll` override and the
epoll path kept as the PORTABILITY fallback (seccomp'd containers deny
io_uring) — CI runs the fiber tests on both. Raw syscalls, 5.1-safe ops
only; the ring layout goes in the binding doc. 23 later rides the SAME
per-shard ring for WAL WRITE+FSYNC chains, and the ops-mode ladder
(reads/writes as ring ops with completion results) is its own follow-up
task after T4 proves the loop.

- [x] Stop composes: the stop flag wakes every parked fiber into the
  unwind path (the existing stop story, per fiber).
- [x] Proven live instead of a bespoke C test: the fibers demo (part 2) + the full battery over parked I/O (log-watcher's mcp loop, web-app's matrix) on BOTH backends + LW_SOAK; originally: fiber A parked in sleep, fiber B progresses, one TID;
  A resumes on time. Corpus: `run/fiber-park-net` if expressible
  without a peer (loopback pair via the existing net builtins — the
  log-watcher mcp fixtures prove the shape); otherwise the unit test
  carries it, disclosed.
- [x] Full battery + framework/web-app gates (serve loop untouched —
  single fiber per process today, degenerate path). Commit. **Stage 1
  complete: board + stories note it.**

## Stage 2 — shards

### Task 5 — shard threads + per-shard runtimes

**Files:** `runtime/src/{vm.h,vm.c,main.c,obj.c,gc.c}`,
`compiler/bin/main.ml` (manifest `[runtime] shards`).

- [x] One pinned pthread per shard, each with its own full `wo_rt`;
  count = cores default, `WO_SHARDS`/manifest override; N=1 must be
  byte-identical to stage 1 (the escape hatch and the proof).
- [x] Program mode: main runs on shard 0; with no spawns the other
  shards idle at zero cost (parked on their eventfds).
- [x] Full battery at N=1 AND at default cores (no cross-shard sends
  exist yet — the threads just have to not break anything). Commit.

### Task 6 — cross-shard send + traced rejection + home-routed frees

**Files:** `runtime/src/{vm.c,obj.c,gc.c}` (MPSC rings, eventfd wake,
home-free ring), `compiler/src/types.ml` (WO-E222 via the
transitively-traced check), corpus + TSan.

- [x] Round-robin spawn placement; pointer handoff; header shard id
  routes the eventual free home (drained per tick, debug-asserted).
- [x] WO-E222 at compile time for traced-containing sends (reuse the
  gc_may-shaped fixpoint the inference already computes).
- [x] The actor corpus under ASan AND TSan: zero races/leaks; output-SET
  assertions (cross-shard nondeterminism is honest, spec'd).
- [x] Full battery at default cores. Commit. **Stage 2 complete.**

## Stage 3 — the DB actor + closing the arc

> **Guarantee obligations (2026-08-21 refinement, developer-approved)**
> — Tasks 7–8 build against these, in addition to their own checkboxes:
> (1) a worker write RPC is exactly ONE owner-shard commit; the ack
> crosses shards only AFTER the owner's fsync — a kill between send and
> commit leaves no ack and no partial state; (2) workers never open the
> WAL or data directory (debug-build assert); replay completes on the
> primary before any worker serves; (3) statements are serialized by the
> DB actor — replies are materialized copies, no torn reads under the
> concurrent multi-shard corpus (TSan). The five-property map lives in
> [the marker doc](../../in-progress/2026-08-21-arc-stage-3.md); disk
> space reclamation is story 32, not this stage.

### Task 7 — transparent DB RPC

**Files:** `runtime/src/builtin.c` (db cases marshal when not on shard
0), `database/src/` untouched (the engine never learns), `runtime/src/vm.c`
(request/reply parking).

- [ ] Non-owner DB builtins marshal statement + args to shard 0, park,
  resume with materialized reply; `transaction { }` travels as one unit
  (18's staged batch stays owner-side).
- [ ] `just employee` + `just web-app` at default cores, answers
  byte-identical to N=1 (iteration 8's criterion 4, the arc's headline
  proof). Commit.

### Task 8 — the arc's closeout

- [ ] 22's benchmark re-run (or its minimal precursor if 22 has not
  landed: the employee read/write loop timed) single- vs multi-shard;
  numbers recorded in the stories.
- [ ] Stories 8 + 11 landing banners; board rows; graph node classes;
  framework README ledger rows that the arc unblocks (streaming etc.
  stay ⏸ until their own slices — the arc UNBLOCKS, it does not build
  them); CODE-LOGIC files (vm fiber model, shard/mailbox model, DB RPC).
- [ ] Full battery once more after doc edits. Commit.

## Success criteria

The spec's four, verbatim — stage 1's determinism + ASan-clean unwind,
stage 2's TSan-clean moves + WO-E222, stage 3's byte-identical
multi-shard gates + recorded 22 delta, and the language having grown
exactly `spawn`/`send`/`actor M`.

## Self-review notes

- Spec coverage: Part A → T1–T4; Part B → T5–T6; Part C → T7–T8;
  diagnostics T3 (E221) / T6 (E222); out-of-scope untouched.
- The riskiest surgery (T1) is deliberately a PURE refactor with a
  byte-identical battery as its only claim — functional change never
  hides inside it.
- Names used consistently: `wo_fiber`, `vm->cur`, WO-E221/E222,
  `WO_REDUCTIONS`, `WO_SHARDS`, `actor M`, `spawn`/`send`.
