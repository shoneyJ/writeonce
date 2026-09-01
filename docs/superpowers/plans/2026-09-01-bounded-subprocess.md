# Bounded Subprocess (iteration 42) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.
>
> **Project rule (overrides the plan-skill template):** plan docs carry
> concept, reason and actions in words — no implementation or test code
> blocks. Each step names the exact functions, fields, ids and expected
> outcomes; the implementer writes the code at the keyboard, matching the
> anchors cited here.

**Goal:** `proc.run` becomes bounded (deadline, output caps, per-shard
ceiling, owner-bound reaping) and parked (fiber waits, shard never stalls),
plus an extended `proc.run_dl` stating bounds per call.

**Architecture:** rework `WO_B_PROC_RUN` in `runtime/src/sysio.c` from
blocking drain + `waitpid` into the iteration 35 `_dl` parking mould: after
the fork, the two pipe read ends and a pidfd for the child are bundled
behind one epoll fd; the fiber parks on that bundle fd with the existing
`fb->dl_active`/`fb->dl_at` deadline sweep armed; re-entry after each wake
drains whatever is ready into growable capped buffers, and the pidfd firing
means reap-and-return. A per-shard child registry in `wo_vm` carries the
cross-park state and serves the ceiling, engine-stop kill, and unwind
cleanup.

**Tech Stack:** C (runtime), OCaml (compiler stdlib table), `.wo` (example
app), bash (gate script), just (recipes).

**Spec:** `docs/superpowers/specs/2026-09-01-bounded-subprocess-design.md`
— the plan argues from it; read both.

## Global Constraints

- Linux only; pidfd needs runtime kernel ≥ 5.3 (Ubuntu 22.04 ships 5.15).
- glibc 2.35 (the release build floor) has NO `pidfd_open` /
  `pidfd_send_signal` wrappers (they arrived in 2.36) — call both via raw
  `syscall(SYS_pidfd_open, …)` / `syscall(SYS_pidfd_send_signal, …)`.
- Defaults, verbatim from the spec: deadline 30 000 ms, stdout cap
  1 048 576 bytes, stderr cap 65 536 bytes, ceiling 32 children per shard.
- Bound violations kill the child and trap as `WO_T_IO` with a message
  naming the bound and its value; silent truncation is removed.
- New builtin id: `WO_B_PROC_RUN_DL = 96` (89/90 stay iteration 31's
  reserved holes). No `.wob` version bump — `WOB_VERSION` does not move.
- No new dependencies, no new threads, no signal handlers.
- All work on `dev`; every commit title prefixed `feat(lang42):` /
  `fix(lang42):` / `docs(lang42):`; bullet-point commit bodies, ≤25 lines.
- Builds and tests only through just recipes: `just wovm-build`,
  `just wovm-test`, `just woc-build`, `just woc-test`.
- Runtime test binaries build with ASan+UBSan (runtime/Makefile does this
  for every `test/test_*.c` automatically) — a leak or race is a failure.

---

### Task 1: claim the prefix, commit the standing docs

**Files:**
- Modify: `docs/00-git-commit-history.md` (prefix registry table)
- Already-edited, to commit: story 42, spec, `docs/stories/00-status.md`
  row, `docs/00-dependency-graph.md` node, the three exploration studies
  under `docs/plan/exploration/{alacritty,tmux,zen-browser}/`, this plan.

**Interfaces:** none — bookkeeping.

- [ ] **Step 1:** Add a `lang42` row to the prefix registry table in
  `docs/00-git-commit-history.md`: feature "iteration 42 — bounded
  subprocess (proc.run bounded + parked, proc.run_dl)", status "on `dev`".
- [ ] **Step 2:** Commit all listed docs as one
  `docs(lang42): story, spec and plan for bounded subprocess` commit
  (bullets: story+spec+plan added; board row; graph node; the three
  parity studies that motivated it).

### Task 2: baseline suite — pin what `proc.run` already does

**Files:**
- Create: `runtime/test/test_proc.c` (auto-picked by the Makefile's
  `$(wildcard test/test_*.c)` — no build wiring needed)

**Interfaces:**
- Consumes: the test harness macros in `runtime/test/t.h` (`T_EQ`,
  `T_CHECK`), `wo_db_init`-style setup patterns from `test_builtin.c`, and
  direct builtin dispatch the way `test_builtin.c` invokes cases — operands
  in registers, `WO_B_PROC_RUN` (id 56) takes cmd Text, `multi Text` args,
  and the Proc class id, returns the record {code, out, err}.
- Produces: `test_proc.c` as the home for every later runtime leg.

- [ ] **Step 1:** Write three green legs against CURRENT behavior, copying
  `test_builtin.c`'s VM/fiber setup: (a) a child that exits 0 with known
  stdout — assert code 0 and the exact bytes; (b) a child that exits with a
  known nonzero code — assert the code; (c) a nonexistent command — assert
  code 127 (the execvp-failed convention already in the code).
- [ ] **Step 2:** `just wovm-test` — all three legs pass, whole suite
  stays green, ASan clean.
- [ ] **Step 3:** Commit `feat(lang42): pin proc.run's current contract in
  test_proc`.

### Task 3: the parked rework — pidfd + epoll bundle + registry

The core task. The suspected sequential-drain deadlock is proven first,
then dissolved by the rework.

**Files:**
- Modify: `runtime/src/sysio.c` (the `WO_B_PROC_RUN` case, currently
  ~lines 723–815)
- Modify: `runtime/src/vm.h` (the per-shard registry in `wo_vm`, one
  pointer field on `wo_fiber` for the in-flight entry)
- Modify: `runtime/src/vm.c` (engine-stop sweep over the registry)
- Test: `runtime/test/test_proc.c`

**Interfaces:**
- Consumes: `WO_SYS_PARKED` re-entry convention (`sysio.c:510` READ_DL is
  the model: fill `fb->park_fd`, arm `fb->dl_active`/`fb->dl_at` once —
  guarded so re-entry does not re-arm — return `WO_SYS_PARKED`; the plane
  re-runs the builtin on wake); `stop_pending()`; `wo_str_new`.
- Produces: a registry entry type (name it `wo_child`) holding pid, pidfd,
  the bundle epoll fd, both pipe fds, two growable buffers with their caps,
  the deadline, and the owning fiber pointer; `wo_vm` gains a fixed array
  of 32 `wo_child` slots plus a live count; `wo_fiber` gains a pointer to
  its in-flight entry (NULL when none). Task 4–8 legs and Task 9's
  `WO_B_PROC_RUN_DL` all reuse exactly this machinery.

- [ ] **Step 1 (the red test):** In `test_proc.c`, add the deadlock leg: a
  child (use `sh -c` in the test only) that writes ~200 KiB to stdout and
  one line to stderr, stderr kept open until stdout completes. Guard the
  leg with a wall-clock check: it must complete within 5 s. Under the
  current sequential drain the parent stops reading stdout at 8,192 bytes,
  the child blocks on a full pipe, and stderr never reaches EOF.
- [ ] **Step 2:** `just wovm-test` — the new leg FAILS (hangs into the
  guard) while everything else stays green. This is the bug proven.
- [ ] **Step 3 (the rework):** Rewrite the `WO_B_PROC_RUN` case: pipes
  opened `O_NONBLOCK` on the parent side; after the fork,
  `syscall(SYS_pidfd_open, pid, 0)`; create one epoll fd and register both
  pipe read ends and the pidfd; claim a registry slot (fail closed with
  `WO_T_IO` naming the 32-per-shard ceiling if none — the message text the
  Task 6 leg asserts); stash caps and buffers in the slot, point the fiber
  at it. First entry and every re-entry then run the same drain: read each
  ready pipe into its growable buffer; a buffer passing its cap means kill
  (`syscall(SYS_pidfd_send_signal, pidfd, SIGKILL, 0, 0)`), reap, release
  the slot, trap `WO_T_IO` naming the cap and value. Pidfd readable means
  exited: reap via `waitpid` (now non-blocking — the pidfd said so), do a
  final drain of both pipes to EOF (bounded by the caps), release, build
  the Proc record exactly as today. Nothing ready and child alive: park on
  the bundle fd with the deadline armed (30 000 ms default), return
  `WO_SYS_PARKED`. Deadline re-entry with `dl_at` passed: kill, reap,
  release, trap `WO_T_IO` naming the deadline. `stop_pending()` on any
  entry: kill, reap, release, return `WO_SYS_STOPPED`.
- [ ] **Step 4:** Engine-stop sweep: where `vm.c`'s worker loop observes
  the stop flag, kill + reap every live registry entry on that shard —
  covers fibers that never get rescheduled.
- [ ] **Step 5:** `just wovm-test` — deadlock leg green, Task 2 baseline
  legs still green (same results from the parked path), suite ASan clean.
- [ ] **Step 6:** Commit `feat(lang42): proc.run parks — pidfd + epoll
  bundle + child registry` (bullets: the deadlock repro and its dissolve;
  raw syscalls because glibc 2.35).

### Task 4: the deadline leg

**Files:** Test: `runtime/test/test_proc.c`; fix (if red exposes drift):
`runtime/src/sysio.c`.

**Interfaces:** consumes Task 3's machinery unchanged; the leg drives the
default arming path.

- [ ] **Step 1 (red first if Task 3 left a gap):** a child sleeping 10 s,
  run with the deadline forced low for the test (drive the builtin with a
  small `dl` the way the harness passes operands — until Task 9 the
  extended operands are reachable only from C, which is fine here). Assert:
  the trap message names the deadline and its value; then assert the pid is
  GONE — `kill(pid, 0)` returns ESRCH — measured, not assumed.
- [ ] **Step 2:** `just wovm-test` green (implement/adjust the kill path if
  Step 1 caught drift). Also add the fiber-progress assertion: while the
  sleeping child runs, a second fiber on the same VM increments a counter —
  assert it advanced before the deadline fired (the shard was never
  blocked).
- [ ] **Step 3:** Commit `feat(lang42): deadline kills, parked shard keeps
  scheduling`.

### Task 5: the output-cap leg

**Files:** Test: `runtime/test/test_proc.c`; fix: `runtime/src/sysio.c`.

- [ ] **Step 1:** a child writing unbounded output against a small stdout
  cap passed from the harness. Assert: `WO_T_IO` whose message names the
  cap and its value; pid gone (ESRCH); and — fd hygiene — the shard's open
  fd count returns to its pre-call value (count `/proc/self/fd` entries
  before and after).
- [ ] **Step 2:** Same for the stderr cap.
- [ ] **Step 3:** `just wovm-test` green. Commit `feat(lang42): output caps
  refuse by name, no truncation`.

### Task 6: the ceiling leg

**Files:** Test: `runtime/test/test_proc.c`.

- [ ] **Step 1:** 32 fibers each spawn a child sleeping 2 s; a 33rd spawn
  must trap `WO_T_IO` naming the ceiling while the 32 keep running to
  completion unharmed. Then all 32 complete with code 0.
- [ ] **Step 2:** `just wovm-test` green (the slot-claim refusal exists
  since Task 3; this pins it). Commit `feat(lang42): per-shard ceiling
  fails closed at 32`.

### Task 7: the churn leg — fds flat over a thousand runs

**Files:** Test: `runtime/test/test_proc.c`.

- [ ] **Step 1:** loop one thousand sequential short-lived children
  (the iteration 24 measurement style): record the `/proc/self/fd` entry
  count before, assert the count after equals it, and assert no registry
  slot remains claimed.
- [ ] **Step 2:** `just wovm-test` green. Commit `feat(lang42): a thousand
  spawns leave the fd table flat`.

### Task 8: stop and unwind reap their children

**Files:** Test: `runtime/test/test_proc.c`; fix: `runtime/src/sysio.c`,
`runtime/src/vm.c`.

- [ ] **Step 1:** park a fiber on a child sleeping 10 s, set the stop flag
  the way `test_fiber.c` does, drive the loop; assert `WO_SYS_STOPPED`
  surfaced AND the child pid is gone. Second leg: a child owned by a fiber
  that is unwound (the `test_unwind.c` pattern) is also gone.
- [ ] **Step 2:** `just wovm-test` green. Commit `feat(lang42): stop and
  unwind kill the children they own`.

### Task 9: `proc.run_dl` — the per-call bounds surface

**Files:**
- Modify: `runtime/src/wob.h` (enum entry `WO_B_PROC_RUN_DL = 96`, comment
  stating the operand order and the nil-never contract: bounds violations
  trap, they do not nil)
- Modify: `runtime/src/sysio.c` (a thin case: parse deadline_ms, out_cap,
  err_cap operands, then fall into Task 3's core with those instead of the
  defaults)
- Modify: `compiler/src/types.ml:315` region (one row beside `proc.run`:
  module `proc`, member `run_dl`, arity 5, id 96, same nullable-Proc
  return and Proc record class as the existing row)
- Test: `runtime/test/test_proc.c` (drive id 96 with explicit bounds);
  compiler: `just woc-test` must stay green — the generic builtin path
  needs no `emit.ml` change (the net `_dl` family at ids 91–93 landed with
  table rows alone; verify by reading their absence from `emit.ml` before
  assuming).

**Interfaces:**
- Produces: `proc.run_dl(cmd, args, deadline_ms, out_cap, err_cap)` in the
  language, the name the example app and every future consumer calls.

- [ ] **Step 1 (red):** runtime leg driving id 96 with a tight explicit
  deadline — fails while the case is absent.
- [ ] **Step 2:** add the wob.h entry and the sysio.c case; green.
- [ ] **Step 3:** add the types.ml row; `just woc-build && just woc-test`
  green.
- [ ] **Step 4:** Commit `feat(lang42): proc.run_dl — deadline and caps at
  the call site`.

### Task 10: the example app and its gate

**Files:**
- Create: `docs/examples/subprocess/` (wo.toml + one `.wo` source: an
  actor that serves `proc.run`/`proc.run_dl` results — a fast command, a
  deliberate deadline hit caught with try/catch, a deliberate cap hit
  caught, and a long-running child for the drain leg; logs to
  `/tmp/subprocess.log` per the examples convention, announced and
  banner-separated)
- Create: `scripts/subprocess-accept.sh` (the gate)
- Modify: `justfile` (recipe `subprocess` running the gate, in the
  `residency`/`site` recipe mould)

**Interfaces:**
- Consumes: `proc.run` and `proc.run_dl` exactly as Task 9 shipped them.

- [ ] **Step 1:** write the app and gate with these legs: fast command
  returns its output through the actor; deadline violation is caught in
  `.wo` and reported (proves catchability from the language, not just from
  C); cap violation likewise; concurrency — a slow child runs while a
  second request is answered (wall-clock assertion that the second answer
  did not wait for the child); SIGTERM while a `sleep 30` child lives —
  the gate records the child pid, stops the app, asserts the app exited
  cleanly AND the child pid is gone (iteration 40's battery gains its
  subprocess leg here).
- [ ] **Step 2:** `just subprocess` — all legs green, printed as
  `subprocess-accept: N checks, 0 failures`.
- [ ] **Step 3:** Commit `feat(lang42): subprocess example + gate` (app,
  script, recipe).

### Task 11: close-out

**Files:**
- Modify: `runtime/src/CODE-LOGIC.md` (the proc.run section: parked
  lifecycle, the registry, the raw-syscall note, the deadlock that was)
- Modify: `docs/stories/language-runtime-database/42-bounded-subprocess.md`
  (frontmatter `status: done`; Progress note of what landed vs the spec)
- Modify: `docs/stories/00-status.md` (NEXT PLAN entry answering the six
  standup questions — implemented, key findings measured, learned,
  unblocked (the streaming form, tmux/alacritty/zen stages), next steps,
  `.dev/reference` used: alacritty/tmux/zen-browser; flip the pending row)
- Modify: `docs/00-dependency-graph.md` (node 42 class → done, same change
  as the board row per the maintenance rule)

- [ ] **Step 1:** run the full belt: `just wovm-test`, `just woc-test`,
  `just subprocess`, plus `just site` untouched-but-verified. All green,
  outputs quoted in the board entry, not asserted.
- [ ] **Step 2:** write CODE-LOGIC.md beside the code (project rule) and
  the three doc updates.
- [ ] **Step 3:** Commit `docs(lang42): close out iteration 42`.

---

## Self-review (done at write time)

- Spec coverage: every acceptance criterion has a task — normal exit (2),
  deadlock repro-then-fix (3), deadline + progress (4), caps (5), ceiling
  (6), fd-flat thousand (7), stop/unwind (8), per-call bounds (9), drain
  leg + example gate (10). Out-of-scope items appear in no task.
- Placeholders: none; every step names its files, ids, messages and
  expected outcomes.
- Consistency: registry type `wo_child`, bundle-epoll park, id 96, caps
  30 000 ms / 1 MiB / 64 KiB / 32 used identically in tasks 3–10.
- One verify-before-assuming flag left deliberately in Task 9: confirm the
  net `_dl` rows needed no `emit.ml` change before mirroring them.
