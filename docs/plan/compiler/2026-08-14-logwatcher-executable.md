# log-watcher Executable Implementation Plan

> **Status: ✅ done 2026-08-15** (story iteration 7) — all six tasks landed:
> the sample compiles, runs, stops on SIGTERM, holds RSS and descriptors flat
> under sustained load in all three modes, and the soak that proves it is in
> the acceptance script. Board: [00-status.md](../../stories/00-status.md)

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

### Task 1 ✅: The owner pass must know what a callee returns — and what a `Text` is

**Concept & reason:** `owner.ml`'s `expr_ty`/`resolve_callee` have no stdlib
table — `types.ml` and `emit.ml` each got one, the ownership pass did not. So a
binding whose value comes from `fs.read_all`, `fs.list`, `net.read`,
`json.encode`, `time.iso` or `proc.run` falls back to the `Scalar "Int"`
default, is classified **Copy**, and never gets a scope-end drop. That is the
1 MiB leak measured in `run` mode: `parse_file`'s `let content = try
fs.read_all(path, FILE_CAP) catch (e) nil` holds a fresh Text nobody frees.
The fix is to read the same `Types.stdlib_members` table the other two passes
read, including through a `try`'s arms, so the classification matches reality.

- [x] Failing measurement first: `run` 1 051 040 B in 24 allocations, `watch`
      128 B in 2, both over eight seconds with a clean SIGTERM exit.
- [x] Teach the ownership pass what a callee returns — three tables it never
      read: the stdlib members, the builtins, and a class's `static` members.
- [x] **`Text` is an owned heap value, not a scalar.** `oclass_of` grouped it
      with Int/Bool, so no Text local was ever dropped; that, not the stdlib
      table alone, was the leak. Making it Owned forces the language to answer
      what a Text does at an ownership boundary, and the answer is uniform: it
      is **copied** — into a container (push/set/`m[i] = v`), into a field
      (SETF), out of a function (`return`), into a binding (`let s = other`),
      and into a loop cursor. The source keeps its own value; a freshly built
      Text stays the caller's and is dropped at the site. `WO_B_TEXT_COPY` is
      the one new builtin this needed.
- [x] Runtime bug found by the same measurement: `fs.read_all`/`net.read`
      allocate their cap and then relabel the buffer with the short length, but
      `wo_str_free` sizes a block by its `len` (obj.h keeps no size headers) —
      so a 1 MiB buffer wearing a 30-byte length went onto a 32-byte free list
      and never came back. They now copy out at the true size and release the
      buffer at the size it was taken.
- [x] Two regressions caught by the corpus and fixed in the same pass: a `@gc`
      value read out of a container is a plain borrow (not an rc-counted
      alias), and `push`'s `@gc` escape is keyed on "`push` is not a
      user-declared fn" rather than on "the callee did not resolve" — which
      stopped being true the moment builtins resolved.
- [x] Re-measured: **`run` 1 051 040 B → 2 112 B (24 → 19 allocations)**,
      **`watch` 128 B → 64 B (2 → 1)**. Everything left is Task 2's projected
      temporary and Task 3's argv container, by stack. `just oop-e2e` 71/0,
      `just woc-test` 565/0, `wovm` unit gates green, `just log-watcher` 6/0.

### Task 2 ✅: A temporary whose field is projected must still be dropped

**Concept & reason:** `for e in parse_dir(self.cron_dir).entries` compiles to
"call, keep the record in a register, read its field, iterate" — and the record
itself is never dropped, because the drop tables only track *bindings*, not the
anonymous receiver a projection borrows from. The elements stay alive (the loop
is correct), the shell leaks, once per rescan. The same shape appears wherever a
call result is projected without a `let`. The temporary must be owned by the
statement that created it and dropped at that statement's end, after every use
of the projection.

- [x] Failing measurement: `run` 2 112 B in 19 allocations; the MCP mix
      21 312 B in 63; per handler, `tools/list` 2 144 B, `get_running_crons`
      2 624 B, `list_logs` 5 184 B, `tail_log` 2 752 B — and 2 requests to 6
      grew `list_logs` from 5 to 13 allocations, so this was growth, not
      residue.
- [x] The projection was one shape of six. Every one of them is the same
      sentence — *a value this expression built, that nothing else owns* — and
      each needed its own site because the drop tables only track bindings:
      a call result compared against `nil` (`if parse_expr(s) == nil` abandoned
      a whole schedule record and its five containers per cron line); an
      argument a callee only **borrows** (`rpc_result(id, "…")`, a 1 KB string
      per MCP request); a container read's copy (`tokens[0]` copies by rule,
      and `let u = tokens[0]` was copying twice and abandoning the first); a
      loop's iterable; the projected record itself; and any of those left
      behind by a `return` taken from inside the statement that built them
      (`check_path` returns out of `for p in self.allowed_paths()`).
- [x] Two lowering bugs found while measuring, both silent: an argument
      register cannot be dropped **after** a `CALL` (the callee's frame
      overlaps it — the value read back is the callee's leftovers), so the
      reap moved into the stash slot `call_window` already emits before the
      call; and a statement-owned temporary cannot live in a temp register (a
      loop reclaims every temp for its body and the end-of-statement `DROP`
      then released a loop counter), so it is parked in a local slot.
- [x] Re-measured: **`run` 2 112 B → 64 B**, identical at 8 s and 20 s;
      **MCP mix 21 312 B / 63 → 64 B / 1**; every handler flat from 2 to 6
      requests (`list_logs` 5 184 → 64, `tail_log` 2 752 → 64,
      `get_running_crons` 2 624 → 64, `tools/list` 2 144 → 64); `watch` 64 B.
      The remaining 64 bytes are Task 3's argv container, on every path. Gates:
      `just oop-e2e` 71/0, `just woc-test` 565/0, `just wovm-test` green,
      `just log-watcher` 6/0. The image grew 35 893 → 46 137 bytes — the drops
      themselves.

### Task 3 ✅: The runtime's argv container has no owner

**Concept & reason:** program mode builds the `multi Text` of arguments in
`runtime/src/main.c` and hands it to the entry method, which borrows it. Nobody
frees it — ASan reports it on every run (64 bytes in 1 allocation, and since Task 2 it is the only leak the
sample reports in any mode). It is
bounded, so it is not the reason a daemon grows, but it is the runtime leaking
its own allocation, and it pollutes every future ASan reading of the sample.
The runtime owns that container and must release it after the entry returns,
before the heap is torn down.

- [x] Drop the argument container once the entry method has returned — one
      site covers both invocation shapes (`self_rc` only picks the argv
      offset, it does not build a second container), and it runs after a trap
      too: the container outlives the unwind. `multi_free` recurses, so the
      argument strings go with it.
- [x] **All three modes report ZERO leaks under ASan**: `watch` and `run` over
      eight seconds with a clean SIGTERM exit, and the full MCP mix (four
      tools, twice each) with a clean exit. Gates: `just oop-accept` ALL
      CRITERIA MET, `just oop-e2e` 71/0, `just wovm-test` green,
      `just log-watcher` 6/0.

### Task 4 ✅: A stopping program must actually stop

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

- [x] Failing measurement: `mcp` mode ignored SIGTERM and needed `kill -9`.
- [x] The calls that genuinely PARK — `net.accept`, a socket read/write,
      `time.sleep`, a child wait — no longer restart the syscall when the stop
      flag is set on an interruption. They hand back `WO_SYS_STOPPED`
      (builtin.h), which is **not** a trap code: no error record, no catch
      handler sees it (a `try` must not be able to swallow SIGTERM), and the
      VM unwinds the whole stack through the same drop machinery an uncaught
      trap uses, so every live value is still released. `wo_vm_call` gained a
      third outcome (1 = stopped) and the CLI maps it to the status the
      program's own `return 0` would have produced. A regular-file read keeps
      its plain retry — it does not park.
- [x] **Found and fixed while measuring: an assignment was not an ownership
      boundary.** `api_key = j.mcp.apiKey` MOVED the field pointer into the
      local, so the local aliased the record; the first stop unwind dropped
      the record and then the alias, freeing the same string twice (a SIGSEGV
      in `class_free`). `let` copied a Text place, assignment did not — it
      does now. The same double free was latent on the normal exit path,
      hidden only by the drop ORDER the compiler happens to emit.
- [x] `just log-watcher` is **7 checks** now: the seventh is the stop itself —
      SIGTERM sent to a server parked in `accept` with no traffic coming, with
      the hard kill demoted to a fallback whose use is the failure.
- [x] Re-measured, all under ASan: `mcp` stopped while parked (rc 0, zero
      leaks), `mcp` stopped after serving traffic (rc 0, zero leaks), `watch`
      and `run` stopped mid-poll (rc 0, zero leaks), and SIGINT behaves as
      SIGTERM. Gates: `just oop-accept` ALL CRITERIA MET, `oop-e2e` 71/0,
      `woc-test` 565/0, `wovm-test` green, `just log-watcher` 7/0.

### Task 5 ✅: The MCP server must close what it accepts

**Concept & reason:** `Mcp.serve` accepts a connection per request and never
calls `net.close` — the builtin exists, the sample does not use it. Every
request costs a descriptor; a long-lived server dies at the process limit. This
is the sample's own bug, and fixing it is in scope precisely because the sample
is the acceptance workload. The connection is a value the loop owns for one
iteration; it must be closed on every exit path from that iteration, including
the malformed-request path that answers 400.

- [x] Failing measurement: exactly one descriptor per request — 4 → 54 fds
      across 50 requests, read from `/proc/<pid>/fd`.
- [x] `net.close(c)` on both paths out of an iteration (the 400 included) and
      `net.close(srv)` on the stop path; the serve loop's comment claimed "the
      drop at each iteration's end IS the close", which was wrong twice —
      `net.Conn` is a scalar to the ownership pass, so nothing was dropped,
      and a drop would not close an fd anyway. The comment now says what is
      true.
- [x] Re-measured: 4 → 4 fds across 200 requests.

### Task 6 ✅: Soak — the acceptance a daemon actually has to pass

**Concept & reason:** every check today is a few seconds long, which is exactly
the window in which a leak is invisible. The claim this plan exists to support
is "you can leave it running", and nothing verifies it. Add a soak mode to the
acceptance script: run each of the three modes under load for a fixed duration,
sample RSS and descriptor count at the start and the end, and fail when either
grows beyond a stated tolerance. Keep it opt-in (an environment variable or a
flag) so the default `just log-watcher` stays fast for the ordinary loop.

- [x] `LW_SOAK=<seconds>` in the acceptance script: each mode runs under load
      (watch: an error line every 200 ms; run: the cron file rewritten every
      500 ms so every rescan reparses; mcp: all four tools back to back), with
      resident memory and descriptor count compared between a **warmed**
      baseline and the end. Warm-up includes load — measuring from before the
      first request reported the allocator reaching its working-set high-water
      as a leak. Tolerance: 256 KiB resident (the kernel accounts lazily),
      **zero** descriptors (a handle has no third state). `LW_ACCEPT_WOVM`
      points the whole script at another build.
- [x] **The soak caught what every seconds-long check missed.** The mcp mode
      grew ~1.6 MiB/minute with ASan reporting zero leaks — in-arena leaks are
      invisible to LeakSanitizer (the arena is one allocation), which is why
      the soak measures RSS and not leak reports. Five compiler/runtime bugs
      fell out, each found by an arena size-class census and a pointer trace:
      `jparse_string` sized every decoded string at "rest of the input" and
      shrank `len` after (the fs.read_all mis-size again — blocks filed on
      free lists their next allocation never reads); `!=` never dropped its
      fresh operands (`headers["authorization"] != "Bearer ${key}"`, twice
      per request); an Int-typed interpolation segment (`"${resp.status}"`)
      wore a place's clothes but is a fresh int_to_text allocation; a Ctor
      handed to `json.encode` had no owner (one record + both field copies
      per tool call); and a discarded expression statement (`pop(lines);`)
      owns what the callee handed back. After: every handler flat per
      request (arena live bytes constant from 4 to 24 requests).
- [x] Release-build soak, 30 s per mode under load: watch delta 0 KiB, run
      delta 0 KiB, mcp delta 20 KiB, descriptors 0/0/0. ASan-build soak
      recorded: RSS plateaus at ~1200 requests (quarantine + redzone
      high-water), then **flat at 14 600 KiB across 601 686 requests in
      90 s** — the authoritative ASan number, since bash's ~30 req/s client
      cannot warm that high-water inside the script's warm-up.
- [x] `just log-watcher` (fast path) stays 7 checks, green, under a minute;
      the soak is opt-in and off by default.

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
