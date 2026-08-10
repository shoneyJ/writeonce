# Program Mode + Systems Stdlib Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
>
> **Style rule (user convention):** concept, reason, and required behavior in words only; the executor writes the code.

**Goal:** Plan 9 — `.wo` becomes a systems language: `fn main` programs with exit codes, and the five capability modules (`env`, `fs`, `proc`, `net`, `time`, plus typed `json`) as safe wovm builtins with RAII handles.

**Architecture:** Plan 9 of the roadmap. Depends on plans 1–3 (toolchain) and plan 8 Task 1 (module resolver knows the stdlib namespaces) and Task 6 (`?T` — most stdlib returns are optional-typed). Runtime work is C builtin families in new `runtime/src/` modules; compiler work is thin (main detection, namespace binding). The spec's Part 2/3 tables are normative. Blocking discipline: program mode runs one shard where blocking builtins are legal; the same surface loop-integrates on server shards later (the shard-actor and HTTP plans own that side — this plan implements program mode only and keeps the builtin layer's seam clean for the other discipline).

**Tech Stack:** C11 + libc (openat/fstat/pread, fork/execvp/waitpid + pipes, socket/bind/listen/accept, clock_gettime/nanosleep, sigprocmask+signalfd), OCaml (driver + typing).

## Global Constraints

- All OOP-track constraints carry over (libc only, no commits — drafts to `.dev/commit.md`, ASan gate, docs under `docs/`).
- **Handles are owned objects; drop closes** — file/socket/process resources ride the MVS destructor path; an fd leak is a test failure, not a review comment.
- **Expected absence is `?T`, faults are traps** (spec error-handling rule): missing file → nil, permission denied on an existing path → trap; the split is documented per function in the module doc.
- **`proc` takes args arrays only** — no shell-string form exists; injection unrepresentable.
- **`fs` has no write/truncate/delete in v1** — `append` is the only mutation (read-only posture default).
- **Builtin ids extend the format doc's table** in each module's task; no opcode changes.
- **Program mode = one shard, blocking legal; server shards unchanged** — nothing in this plan touches the server path.

---

## File Structure

```
runtime/src/
  sys_env.c sys_fs.c sys_proc.c sys_net.c sys_time.c   one C module per namespace (Tasks 2-6)
  builtin.c              dispatch table grows per module
compiler/src/            main detection, stdlib namespace typing (Task 1)
compiler/bin/main.ml     program-mode driver behavior (Task 1)
tests/corpus/sys/        stdlib fixtures against real tempdirs/processes/sockets
docs/plan/oop-vm/07-systems-stdlib.md   per-function contracts: types, nil-vs-trap, bounds (Task 1)
```

---

### Task 1: Program mode — `fn main`, exit codes, `env` module

**Concept & reason:** the mode switch. A project with a free `fn main(args: multi Text) -> Int` compiles as a program: `wo run` (the C stack's runner) executes main on one shard and exits with its return; `woc build` packages it single-binary (plan-3 trailer, unchanged). Service blocks without main = server (existing); both = main runs and decides (spec's log-watcher shape). The `env` module lands here because main is unusable without it: `env.args() -> multi Text`, `env.get(name) -> ?Text`, `env.exit(code)` (typed as never-returning), `env.stopping() -> Bool` — the runtime blocks SIGTERM/SIGINT, owns a signalfd, and flips the flag; programs poll it (no callbacks, spec rule). `print_err` joins the print family; stdout/stderr flush on newline. The module doc opens with the nil-vs-trap contract table every later task extends.

- [ ] Failing fixtures: main runs with args and its return becomes the exit code; env.get present/absent; a stopping-flag fixture (send SIGTERM to the child, assert clean flagged exit); print_err lands on stderr.
- [ ] Implement driver + sys_env + builtins; green.
- [ ] Record commit draft: `feat: program mode — fn main entry with exit codes on the C stack, env module (args/get/exit/stopping via signalfd flag), print_err, flush-on-newline; docs/plan/oop-vm/07-systems-stdlib.md contract table.`

### Task 2: `time` module

**Concept & reason:** smallest module, unblocks every poll-loop fixture after it. `time.now()` exists (wall ms); add `time.mono() -> Int` (monotonic ms, CLOCK_MONOTONIC — interval math must not jump with wall-clock changes) and `time.sleep(ms)` (nanosleep; in program mode it blocks the shard, which is the point; EINTR from the shutdown signal returns early — the daemon loop's exit path).

- [ ] Failing fixtures: mono monotonicity across a sleep; sleep duration lower-bound; sleep cut short by SIGTERM with stopping() true after.
- [ ] Implement; green.
- [ ] Record commit draft: `feat(runtime): time module — mono (CLOCK_MONOTONIC ms), sleep (EINTR-aware, shutdown cuts it short); poll-loop idiom complete.`

### Task 3: `fs` module

**Concept & reason:** the log-watcher workhorse. Per the spec table: `exists`, `stat -> ?{size, inode, mtime}` (a record from plan 8 Task 4 — inode present because rotation detection depends on it), `read_at(path, offset, max) -> Text` (open/pread/close inside the builtin — bounded tail-chunk reads; no persistent file handles in v1 since every LogTail poll is open-read-close by design), `read_all(path, cap)`, `append(path, text)` (O_APPEND open-write-close — the JSONL sink), `list(dir) -> multi Text`. Nil-vs-trap per the contract: missing path → nil/false variants; EACCES on an existing path, or reads past cap → traps. All texts are bytes-faithful (logs contain ANSI junk; the language's Text carries it — sanitizing is the program's job, as log-watcher itself learned in production).

- [ ] Failing fixtures (real tempdir): stat fields incl. inode change across rename-rotation; read_at windows (offset, max, EOF clamp); append accumulates JSONL lines; list ordering defined (sorted — determinism for tests); missing-path nils; permission trap (chmod 000 fixture, skipped when running as root).
- [ ] Implement; green under ASan.
- [ ] Record commit draft: `feat(runtime): fs module — stat with inode/mtime, bounded read_at/read_all, O_APPEND append, sorted list; nil-vs-trap per contract; rotation fixture via rename.`

### Task 4: `proc` module

**Concept & reason:** the probe capability. `proc.run(cmd, args: multi Text) -> {code: Int, out: Text, err: Text}`: fork/execvp with an args array (PATH search yes, shell never), both pipes captured with a per-stream byte cap (truncation flagged in the record — a fourth field, `truncated: Bool`, small spec addition recorded in the module doc), waitpid for the exit code; signal-death reported as conventional 128+signal. Exec failure (missing binary) is expected absence territory: code 127 in the record, not a trap — matching the Haxe original's "unknown → safe direction" probes. Blocking by definition; program mode only (a server-shard call diagnoses at compile time until the loop-integrated discipline lands in its own track).

- [ ] Failing fixtures: true/false exit codes; output capture both streams; cap truncation flag; missing binary → 127; args with spaces pass verbatim (no shell proof); zombie-free after many runs (fixture loops 100 spawns, asserts no defunct children via /proc scan).
- [ ] Implement; green.
- [ ] Record commit draft: `feat(runtime): proc module — fork/execvp args-only run with capped dual capture + truncated flag, waitpid codes (128+sig, 127 missing), zombie-free; server-shard use diagnosed.`

### Task 5: `net` module

**Concept & reason:** the serve capability, and the RAII showcase. `net.listen(addr, port) -> Listener` (SO_REUSEADDR, backlog sane), `net.accept(listener) -> ?Conn` (blocking; nil when interrupted by shutdown — the accept-loop exit path), `net.read(conn, max) -> ?Text` (nil on peer close), `net.write(conn, text)`. Listener and Conn are owned handle objects — class-table natives whose drop closes the fd (the plan-1 native-sentinel pattern; format doc grows the two handle classes). One request per connection is the supported v1 shape (the MCP pattern); keep-alive service belongs to the server stack, not here.

- [ ] Failing fixtures (loopback): listen/accept/read/write echo round-trip driven by a scripted client; peer-close nil; shutdown interrupts accept with nil + stopping(); **fd RAII battery** — accept and drop many conns in a loop, assert stable fd count via /proc/self/fd (the spec's leak test, ASan-adjacent but fd-specific).
- [ ] Implement; green.
- [ ] Record commit draft: `feat(runtime): net module — TCP listen/accept/read/write with owned Listener/Conn handles (drop closes), shutdown-aware accept, peer-close nil; fd-count RAII battery.`

### Task 6: `json` module

**Concept & reason:** typed decode, the `Dynamic` replacement. `json.decode(text) as RecordType -> ?RecordType`: the emitter passes the target class/record id alongside the builtin call; the C side walks the plan-6 codec's parse events against the class table's field kinds — matching fields fill, `?fields` absent stay nil, unknown JSON keys skip, any shape mismatch (wrong type, missing required field) yields nil overall (never a trap — config errors are expected absence). Nested records, `multi` of scalars/records, and `map<Text, scalar>` decode; anything else in the target type diagnoses at compile time as undecodable. `json.encode(value) -> Text` walks kinds in reverse (the plan-6 encoder generalized). If plan 6 has not landed when this executes, the codec lands here and plan 6 consumes it — the doc notes the either-order seam.

- [ ] Failing fixtures: config-shaped decode (the SupConfig pattern: numbers, optional string, array of strings); missing-required nil; unknown-keys-skip golden; nested + multi + map decode; undecodable-target must-fail; encode round-trip.
- [ ] Implement; green.
- [ ] Record commit draft: `feat: typed json — decode-as against class-table kinds (?fields nil, unknown keys skip, mismatch = nil never trap), encode by kinds; undecodable targets diagnosed at compile time.`

### Task 7: Corpus battery + acceptance

**Concept & reason:** criteria 2 and 3 of the spec, gated. The `sys/` corpus runs everything above end to end plus the cross-module fixtures that mimic log-watcher's composites: a tail-poll fixture (write to a tempfile between polls, assert offset math via read_at), a probe fixture (flock-style: proc.run against a held lock file — using the real flock binary when present, skipped cleanly otherwise), a mini serve-loop fixture (accept one request, respond, exit on stopping). `just oop-accept` gains the sys corpus; the module doc's contract table gets a shipped-status column; CLAUDE.md commands note program mode.

- [ ] Wire fixtures + gate; green; docs synced.
- [ ] Record commit draft: `test(corpus): sys battery — tail-poll offset math, real-flock probe (skip-aware), mini serve loop with shutdown; oop-accept gains the sys gate; docs synced.`

---

## Plan self-review notes

- **Spec coverage (Parts 2–3, criteria 2–3):** program mode T1, all five modules T2–T6 matching the spec tables exactly (one addition: `proc` result's `truncated` flag, recorded in the module doc), RAII proofs in T5 (fd battery) + ASan everywhere, corpus gate T7.
- **Dependency honesty:** needs plan 8's modules (`use`), records, and `?T`; json (T6) shares the plan-6 codec with an either-order seam noted.
- **Order rationale:** env/main first (nothing testable without an entry point), time second (fixtures need sleep/mono), fs/proc/net by increasing machinery, json last (needs records + codec), battery at the end.
