# log-watcher Executable Implementation Plan

> **Status: 🔄 in progress** (story iteration 7) — the sample compiles and its
> three modes run; this plan is everything still between "it runs" and "you can
> leave it running". Board: [00-status.md](../../00-status.md)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
>
> **Style rule (user convention):** concept, reason, and required behavior in words only; the executor writes the code.

**Spec:** [`docs/superpowers/specs/2026-08-01-systems-track-design.md`](../../superpowers/specs/2026-08-01-systems-track-design.md) (Part 1 verdict table, normative), amended by [`docs/superpowers/specs/2026-08-10-logwatcher-gap-closure-design.md`](../../superpowers/specs/2026-08-10-logwatcher-gap-closure-design.md).

**Goal:** `docs/examples/log-watcher` is **executable** — not merely compilable.
Each of its three modes runs indefinitely without growing, stops when told to,
and ships as one self-contained binary. Nothing else is in scope: every task
below exists because a measurement on the sample demanded it, and anything the
sample does not exercise is deferred by name in "Out of scope".

**Architecture:** the compiler front (`compiler/src/`), the VM's ownership
tables (`owner.ml` ↔ `emit.ml`) and the runtime's process surface
(`runtime/src/main.c`, `sysio.c`). No new language features — the four
compiler-side tasks are missing *ownership knowledge*, not missing grammar.

**Tech Stack:** OCaml stdlib (compiler), C11 libc (runtime), the conformance
corpus as regression, `scripts/log-watcher-accept.sh` as acceptance.

## Where this plan starts (measured 2026-08-14)

- `woc --emit docs/examples/log-watcher` → **0 diagnostics**, 35 KB image.
- `woc build …` → a **106 KB standalone binary** that runs its three modes.
- `just log-watcher` → **6 checks, 0 failures** (compile, watch alert, cron
  schedule, MCP initialize / tools/list / 401).
- All four MCP tools answer with `isError:false`.
- **Under ASan, both long-running modes leak**: `watch` 128 bytes in 2
  allocations; `run` over 1 MB across 6 allocations in eight seconds — the
  1 MiB one is a single `fs.read_all` result.
- The sample uses **zero `@gc`**: 35 classes, none with the gc flag, 0 `RC_INC`
  / 0 `RC_DEC`, 78 `DROP`s. Deterministic ownership is the whole memory story
  here, which is why the leaks above are compiler bugs, not collector gaps.

## Global Constraints

- **The sample is the test.** No new corpus fixtures for this plan (user
  direction, 2026-08-14); `just oop-e2e` must stay green as a regression, and
  `just log-watcher` is the acceptance gate.
- **No new language surface.** A task that finds itself wanting one has found a
  defect report against this plan, not a feature — stop and ask.
- Every task ends with the sample rebuilt and `just log-watcher` green, and
  with the ASan measurement re-run so the number moves in writing.
- Commits are local only; never push.

## File Structure

```
compiler/src/owner.ml      stdlib return types; temporary-value drops (Tasks 1, 2)
compiler/src/emit.ml       the drop sites those tables imply (Tasks 1, 2)
runtime/src/main.c         argv container lifetime; stop-signal exit (Tasks 3, 4)
runtime/src/sysio.c        blocking calls observing the stop flag (Task 4)
docs/examples/log-watcher/ mcp.wo: close what accept opened (Task 5)
scripts/log-watcher-accept.sh  the soak check (Task 6)
```

### Task 1: The owner pass must know what the stdlib returns

**Concept & reason:** `owner.ml`'s `expr_ty`/`resolve_callee` have no stdlib
table — `types.ml` and `emit.ml` each got one, the ownership pass did not. So a
binding whose value comes from `fs.read_all`, `fs.list`, `net.read`,
`json.encode`, `time.iso` or `proc.run` falls back to the `Scalar "Int"`
default, is classified **Copy**, and never gets a scope-end drop. That is the
1 MiB leak measured in `run` mode: `parse_file`'s `let content = try
fs.read_all(path, FILE_CAP) catch (e) nil` holds a fresh Text nobody frees.
The fix is to read the same `Types.stdlib_members` table the other two passes
read, including through a `try`'s arms, so the classification matches reality.

- [ ] Failing measurement first: record the current ASan totals for `watch` and
      `run` (eight seconds each, clean exit via SIGTERM) so the drop is proven,
      not assumed.
- [ ] Teach the ownership pass the stdlib return shapes; every stdlib member
      that yields a fresh Text, `multi` or record is Owned at its binding.
- [ ] Re-measure: the `fs.read_all` and `fs.list` allocations disappear from
      both modes' reports; `just oop-e2e` and `just woc-test` stay green.

### Task 2: A temporary whose field is projected must still be dropped

**Concept & reason:** `for e in parse_dir(self.cron_dir).entries` compiles to
"call, keep the record in a register, read its field, iterate" — and the record
itself is never dropped, because the drop tables only track *bindings*, not the
anonymous receiver a projection borrows from. The elements stay alive (the loop
is correct), the shell leaks, once per rescan. The same shape appears wherever a
call result is projected without a `let`. The temporary must be owned by the
statement that created it and dropped at that statement's end, after every use
of the projection.

- [ ] Failing measurement: the `run` mode's per-rescan growth over ~60 seconds,
      with the rescan interval shortened, as the number to beat.
- [ ] Give a projected temporary a real owner and a drop at the end of its
      statement, including when the projection feeds a loop that outlives the
      expression.
- [ ] Re-measure: rescan no longer grows the process; corpus and unit gates
      stay green.

### Task 3: The runtime's argv container has no owner

**Concept & reason:** program mode builds the `multi Text` of arguments in
`runtime/src/main.c` and hands it to the entry method, which borrows it. Nobody
frees it — ASan reports it on every run (128 bytes in 2 allocations). It is
bounded, so it is not the reason a daemon grows, but it is the runtime leaking
its own allocation, and it pollutes every future ASan reading of the sample.
The runtime owns that container and must release it after the entry returns,
before the heap is torn down.

- [ ] Drop the argument container once the entry method has returned (both the
      plain `wovm image.wob …` path and the single-binary path).
- [ ] `watch` under ASan reports **zero** leaks for a clean exit.

### Task 4: A stopping program must actually stop

**Concept & reason:** `env.stopping()` installs SIGTERM/SIGINT handlers that set
a flag, and `net.accept`/`net.read` retry on `EINTR` — so a server parked in
`accept` never observes the flag and TERM does nothing; only `kill -9` ends it.
`watch` and `run` stop correctly today only because they sleep between polls.
A service that cannot be stopped is not executable in any operational sense
(no clean restart, no deploy, no supervisor integration). The decision to make
and record: when a blocking stdlib call is interrupted **and** the stop flag is
set, the runtime stops the program rather than restarting the syscall — the
exit is the entry's normal one, with the same status a clean `return 0` gives.
The alternative (surface the interruption to the source) is rejected here: it
would put a trap in the middle of every accept loop the language will ever
write, and the shard-actor runtime (iteration 8) replaces these blocking calls
with an event loop anyway.

- [ ] Failing measurement: `mcp` mode ignores SIGTERM and needs `kill -9`.
- [ ] Blocking stdlib calls observe the stop flag on interruption; the process
      exits cleanly, flushing output.
- [ ] `just log-watcher` no longer needs `kill -9` in teardown, and the script's
      hard-kill fallback becomes belt-and-braces rather than the mechanism.

### Task 5: The MCP server must close what it accepts

**Concept & reason:** `Mcp.serve` accepts a connection per request and never
calls `net.close` — the builtin exists, the sample does not use it. Every
request costs a descriptor; a long-lived server dies at the process limit. This
is the sample's own bug, and fixing it is in scope precisely because the sample
is the acceptance workload. The connection is a value the loop owns for one
iteration; it must be closed on every exit path from that iteration, including
the malformed-request path that answers 400.

- [ ] Failing measurement: descriptor count for the server process across a few
      hundred requests.
- [ ] Close the connection on every path out of the serve loop's body.
- [ ] Re-measure: the descriptor count is flat.

### Task 6: Soak — the acceptance a daemon actually has to pass

**Concept & reason:** every check today is a few seconds long, which is exactly
the window in which a leak is invisible. The claim this plan exists to support
is "you can leave it running", and nothing verifies it. Add a soak mode to the
acceptance script: run each of the three modes under load for a fixed duration,
sample RSS and descriptor count at the start and the end, and fail when either
grows beyond a stated tolerance. Keep it opt-in (an environment variable or a
flag) so the default `just log-watcher` stays fast for the ordinary loop.

- [ ] Soak the three modes with a stated duration, load pattern and tolerance;
      report the measured deltas whether it passes or fails.
- [ ] Run the soak against an ASan build once and record the result in the
      status board's known-gaps section.
- [ ] `just log-watcher` (fast path) stays green and stays under a minute.

## Out of scope — deferred by name

- **`?T` forced handling, `pub(read)` write enforcement, `using`, `#if`,
  reject-row diagnostics** (plan 8 Tasks 6–8's remainder). They make the
  language *stricter*; they do not make this program run. Plan 8 stays open for
  them behind this plan.
- **Anything `@gc`**: iteration 7b (inferred GC + incremental mark-sweep),
  `set`'s `@gc` retention gap, iteration 4's `gc/held-cycle` leak. Measured:
  the sample declares no `@gc` class and emits no `RC_INC`/`RC_DEC` at all, so
  none of it can affect this workload.
- **json's `Bool` renders as `0`/`1`** and fractional numbers truncate — both
  documented format consequences; the MCP client the sample targets reads them
  fine.
- **Iterations 8–12** (shard-actor runtime, database engine, `@table`/query,
  HTTP layer, fibers, blue-green) — unchanged, and unblocked by this plan.
