# Story — one language, one runtime, one database, one binary

> Format: fiberloom `product/story-template` (tech-unit standard artifact).
> Sources consulted: fiberloom `tech-unit/framework`, `product/story-template`, `product/story-iteration-template`.

**AS** a developer building and operating my own products end to end

**I WANT** a new programming language — with arithmetic, ownership-based memory safety, and garbage collection applied automatically wherever ownership alone cannot express the shape — whose compiler, runtime, and database ship as a single never-stopping Linux binary that can update its own code in place

**TO** write an application once and run it forever: no external stack to assemble, no database server to operate, and deployments that swap code inside the running process with instant rollback.

## User Value

- One artifact is the whole system: the language's runtime owns the data,
  serves the API, and carries its own source — the "which commit is prod
  running?" class of questions disappears.
- Memory safety without a GC tax: Rust-shaped borrowing (single owner,
  second-class borrows) checked mostly at compile time, and where ownership
  cannot express the shape the compiler decides — no annotation to write, and
  collection stays per-shard so no global pause exists by construction
  (iteration 7b; iterations 1–7 shipped a per-class `@gc` opt-in instead).
- Updates are blue-green **inside** the runtime: propose, approve, compile
  in-process, atomic switch, previous version resident for instant rollback.
- The runtime is a recipe box: once language + runtime + database exist, a
  web framework arrives as a `.wo` library composing runtime capabilities —
  the recorded next story.

## Background & Constraints

writeonce today is a declarative Rust-based runtime (Stage 2). This story
evolves it into an object-oriented language (`woc` OCaml compiler, `wovm` C
VM) per the approved specs: C as the runtime's basis (libc only), no
inheritance ever, mutable value semantics for borrowing, shard-per-core
concurrency with ownership-moving messages, RAM-authoritative data under a
WAL, and the blue-green VM pair for in-runtime deployment. Target OS is
Linux; kernel primitives are the framework. The Rust runtime retires only at
parity. Constraints: OCaml stdlib only, C libc only; docs live under
`docs/`; prose-only planning artifacts (no implementation code in stories or
iterations); no commits by agents — drafts go to `.dev/commit.md`.

## Iterations (rows in dependency order — `#` is an immutable ID, not a rank)

RENUMBERED 2026-08-20 (developer directive): pending iterations carry
fresh IDs in priority order; LANDED iterations keep their historical
numbers (code comments and commit history cite them — records, not a
queue). Mapping: 19←20(scalars), 20←9c, 21←9d, 22←9e, 23←9f, 24←19(chat),
25←10, 26←12, 27←9g, 28←14, 29←13; 8, 11, 17, 18 unchanged.
Re-sequenced from the edges in
[`docs/00-dependency-graph.md`](../../00-dependency-graph.md): landed
rows in landing order, then the pending rows in implementation order.
File names keep their IDs — every board, spec, and plan references
iterations by number, so numbers never renumber.

RE-SEQUENCED 2026-08-20 (second pass) against the verified findings in
[`docs/00-code-review.md`](../../00-code-review.md). **Seq changed; no `#`
changed and no file moved** — that is what an immutable ID is for. The
rule applied: measure before optimizing, close correctness holes before
adding surface, and stop stacking features on unmeasured ground.

- **22 → first.** It has never run. `bench/baseline.json` does not exist,
  there is no `just db-bench`, and `runtime/bench/` is the retired C
  prototype's harness plus a Go reference. Until 22 runs, no performance
  statement about this project is sourced.
- **The arc's stage 3 → second, reframed as correctness.** `wo_engine_start`
  zero-initializes worker VMs, so `rt.db` is NULL off the primary and any DB
  statement there traps `WO_T_DB "database engine not initialized"`. A
  multi-shard program that touches the database is broken today.
- **New 30 (observability, CI, fuzz) and new 31 (actor lifecycle).** The
  review's two largest gaps had no iteration at all: nothing to run the
  proof automatically, and no request/response, backpressure, supervision,
  or timers behind `spawn`/`send`.
- **18, 20, 21 demoted.** All three add surface; none answer a named gap.

RE-SEQUENCED 2026-08-21 (third pass — the concurrency chain). Everything
still pending IS the runtime-concurrency chain; order:
**stage 3 → 22 → 31 → 24 → 23 → 32**. Changes from the second pass:

- **The arc's stage 3 moves ahead of 22** — correctness before
  measurement: a multi-shard program touching the database traps
  `WO_T_DB` today, and fixing that first lets one benchmark campaign
  cover single- and multi-shard honestly.
- **31 has its story file** ([refine/31-actor-lifecycle.md](refine/31-actor-lifecycle.md));
  30 stays a row until it is scheduled.
- **Holds landed** (developer decision, 2026-08-21): 18, 20, 21, 26, 27,
  28, 29 moved to `hold/`; 25's story file removed (its plan doc remains
  in superpowers). 19 landed 2026-08-20.

| Seq | # | Iteration | Delivers |
| --- | --- | --- | --- |
| 1 | 1 | [Principles doc](done/01-principles-doc.md) | `docs/00-principles.md` — the doctrine page every later slice links back to |
| 2 | 2 | [VM core](done/02-vm-core.md) | `wovm`: `.wob` loader, register interpreter, arena, borrow word, `@gc` collector |
| 3 | 3 | [Compiler front](done/03-compiler-front.md) | `woc`: lexer → parser → typechecker → ownership pass, diagnostics |
| 4 | 4 | [Single binary end-to-end](done/04-single-binary-e2e.md) | emitter + conformance corpus + `woc build` self-contained binary |
| 5 | 5 | [Language surface](done/05-language-surface.md) | Haxe-parity adoptions, grammar + strictness halves (landed in waves through 2026-08-20) |
| 6 | 6 | [Program mode + stdlib](done/06-program-mode-stdlib.md) | `fn main`, exit codes, `fs`/`proc`/`net`/`time`/`json` builtins |
| 7 | 7 | [log-watcher proof](done/07-logwatcher-proof.md) | the driving workload compiled, executable, soak-proven (landed 2026-08-15) |
| 8 | 7b | [Inferred GC + mark-sweep](done/07b-inferred-gc-mark-sweep.md) | `@gc` removed; GC-ness inferred; RC replaced by incremental per-shard tri-color mark-sweep |
| 9 | 9 | [Database engine](done/09-database-engine.md) | class-shaped tables, typed WAL + recovery, `insert`/`select` execute |
| 10 | 9b | [`@table`, relations, query](done/09b-table-relations-query.md) | `@table` real storage; `ref`/`backlink`/`multi`; compiler-checked queries |
| 11 | 15 | [deps: `wo.toml [deps]`](done/15-deps-package-manager.md) | exact-rev git deps + `wo.lock` + `.wo-deps`; flat-only, offline once locked |
| 12 | 16 | [web framework](done/16-web-framework.md) | the `.wo` framework v1 (router, middleware, auth, all three body hooks) consumed via `[deps]` |
| 13 | 8+11 | [Shard-actor runtime](in-progress/08-shard-actor-runtime.md) · [Fibers](in-progress/11-fibers.md) | THE ARC (stages 1+2 landed 2026-08-20: fibers/budget/actors/io_uring plane; pinned shards, envelope sends, home-routed frees, WO-E222); stage 3 = transparent DB RPC. **Promoted above 22 (2026-08-21): stage 3 is a correctness hole, not an optimization** — worker shards are zero-initialized, so a DB statement off the primary traps `WO_T_DB`. |
| 14 | 22 | [Durability, throughput, scale](refine/22-durability-throughput-scale.md) | restart-persistence proof, benchmarks, ~1M rows — the baseline the arc and 23 sign against. It has never run, so every performance claim on this project is currently unsourced; runs after stage 3 so one campaign covers single- and multi-shard, plus the stage-2 mutex-inbox number. *(was 9e)* |
| 15 | 30 | Observability, CI, fuzz *(no story file yet)* | **NEW** — runtime counters + a profiler hook, 22's harness wired to run per change instead of by hand, and a fuzz target on the parser and `.wob` loader. The whole proof-maturity gap had no iteration to point at. |
| 16 | 19 | [Float + Bytes](done/19-missing-scalar-types.md) | **LANDED 2026-08-20** — `.wob` v5; the full stack: IEEE-quiet f64 through literals/VM/@table/WAL/json + Bytes as the binary carrier, no implicit mixing, total-order indexes. Unblocks 24 (WS frames) and the crypto fork (digests). *(was 20)* |
| 17 | 31 | [Actor lifecycle](refine/31-actor-lifecycle.md) | request/response (today `send` is one-way and callers `sleep` to await), bounded mailboxes with backpressure (today the FIFO just grows), actor death/supervision, and timers beyond `time.sleep`. 24 cannot be written honestly without these. *(story written 2026-08-21)* |
| 18 | 24 | [chat: WebSocket workload](refine/24-chat-websocket-workload.md) | the arc's acceptance: WS upgrade + frames (SHA-1 via crypto fork, Bytes via 19), rooms/broadcast, 1k clients, drain-clean. *(was 19)* |
| 19 | 23 | [io_uring group-commit](refine/23-io-uring-commit.md) | WAL WRITE+FSYNC chains on the arc's per-shard rings; fsync fallback kept (after 22 + the arc). *(was 9f)* |
| 20 | 32 | [WAL checkpoint](refine/32-wal-checkpoint.md) | **NEW 2026-08-21** (stage-3 guarantee refinement found the hole) — the WAL is append-only forever: snapshot + truncate reclaims disk and bounds replay time; every durability guarantee byte-identical; crash mid-checkpoint recovers from the previous snapshot + full tail. After 23 (composes with group-commit); RAM slot-reuse already contracted in `04-db-binding.md`. |
| 21 | 25 | [HTTP service layer](../../superpowers/plans/2026-08-01-http-service-layer.md) | `service` blocks lower onto the framework (after 9b + 20 by their own precedence notes). **HELD 2026-08-21** — story file removed; the plan doc remains. *(was 10)* |
| 22 | 18 | [framework v2: memory-rich features](hold/18-memory-db-features.md) | spec+plan approved: TTL cache, @table flags, durable job queue, `transaction { }` over the WAL's staged batch. **Demoted from seq 14**: more surface on a framework with one consumer, and the cache still stores `Text` because there are no generics |
| 23 | 27 | [Query grammar corpus](hold/27-query-grammar-corpus.md) | grow the query grammar from real corpora; likely collapses to "confirm `len(query)` + add `exists`"; precedes 28. *(was 9g)* |
| 24 | 26 | [Blue-green deploy](hold/26-blue-green-deploy.md) | two VM slots, in-runtime compile, atomic switch, resident rollback (plan authored after 9 + 25). *(was 12)* |
| 25 | 20 | [Cross-program tables](hold/20-cross-program-tables.md) | attach to a running program's database over local IPC; owner stays the single writer (channel half-built). **Demoted from seq 16**: new distribution surface while there is no TLS, no crypto, and the multi-shard DB still traps. *(was 9c)* |
| 26 | 21 | [Keypair attach auth](hold/21-keypair-attach-auth.md) | program identity is a keypair; mutual challenge–response at attach (crypto half-built; plan folds into 20's). **Demoted with 20** — and it needs crypto primitives that do not exist. *(was 9d)* |
| 27 | 28 | [skillhost host workload](hold/28-skillhost-host-workload.md) | host-shaped driving workload naming runtime gaps — demoted with the framework goal. *(was 14)* |
| 28 | 29 | [Compile-time metaprogramming](hold/29-compile-time-metaprogramming.md) | `@derive(...)` from class-table metadata; held with the parked drain by the 2026-08-08 scope directive. *(was 13)* |
| ✅ | 17 | [library projects + `internal/`](done/17-library-projects-internal.md) | **LANDED 2026-08-20** — `kind = "library"` + entry-less check mode (retires the `--emit` workaround) and Go's `internal/` rule as WO-E108 at the consumer's `use`; driver-only, VM/GC untouched. `just web-app` 26/0 |


Review protocol: the developer reads one iteration, approves or amends;
the next starts only after approval. Each iteration is an unsplittable
value slice with its own acceptance criteria (Given/When/Then), out-of-scope
list, and a pointer to the plan document that already sequences its tasks.

## Related Links

- OOP core spec: `docs/superpowers/specs/2026-08-01-oop-compiler-vm-design.md`
- Systems track spec: `docs/superpowers/specs/2026-08-01-systems-track-design.md`
- Blue-green spec: `docs/superpowers/specs/2026-08-03-blue-green-vm-design.md`
- Sample+principles spec: `docs/superpowers/specs/2026-08-07-logwatcher-sample-and-principles-design.md`
- Plan documents: `docs/superpowers/plans/` (plans 1–10), `compiler/plan/` (2, 3, 8 + architecture)
- Roadmap map: `docs/08-project-structure.md` (build sequence)
- Behavioral reference workload: `~/projects/log-watcher` (Haxe daemon)

## Notes

- Acceptance criteria live in each iteration file; this story frames the
  outcome.
- **Critical path (locked 2026-08-08): compile and run log-watcher.**
  Iterations 3 → 4 → 5 → 6 → 7 are the committed line; nothing off that
  line lands before iteration 7's acceptance. Then 7b, then 8–12 (with 9b after the database engine).
- **Goal shifted 2026-08-20: log-watcher (iteration 7, landed 2026-08-15)
  to the web framework.** Same day, refined by directive: the framework
  stays a polished MICRO-framework (routing, middleware, `Req`/`Resp`) —
  nothing MVC-scale — and iteration 17 (library kind + `internal/`) is
  **parked** with spec + plan ready on branch `library-internal`. The
  v1 slices landed 2026-08-20 (`just web-app` 21/0 after polish, auth,
  form, multipart). The implementation-order list this note once carried
  is superseded — the iterations table above is the authority
  (re-sequenced 2026-08-20 twice, then 2026-08-21); edges live in
  [`docs/00-dependency-graph.md`](../../00-dependency-graph.md).
- **Iteration 7b (inserted 2026-08-11)** sits after the critical path
  deliberately: it delays nothing on the log-watcher line, and it must precede
  iteration 8 because the collector should be settled before shards multiply.
  It also closes iteration 4's one open gate clause and supersedes part of
  iteration 2's memory model — neither is renumbered; both point here.
- **Future iterations, after iteration 11** (parked 2026-08-08 — recorded,
  not scheduled):
  - ~~WO-W201 `@gc`-suggestion diagnostic refinement~~ — **superseded by
    iteration 7b**: the diagnostic is retired outright, because inference
    replaces the suggestion it existed to make.
  - WO-E225 unknown-type validation broadened to `ref`/`multi`/`map`
    element types and method/fn signatures.
  - ADT container roster adoption (Stack, Queue, Set, Tree, Graph, … —
    see the roster section in
    `docs/plan/compiler/nullable-types-implementation.md`); log-watcher
    needs only `multi`/`map`, so no ADT lands before iteration 7.
- Recorded future stories, deliberately outside this one: the web framework
  as a `.wo` library over the recipe-box runtime; UI (`##ui` SSR + live
  patches); script-based destructive schema migrations; MCP/agent wrapper
  over the management plane.
