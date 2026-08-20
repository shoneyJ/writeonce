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

Re-sequenced 2026-08-20 from the edges in
[`docs/00-dependency-graph.md`](../../00-dependency-graph.md): landed
rows in landing order, then the pending rows in implementation order.
File names keep their IDs — every board, spec, and plan references
iterations by number, so numbers never renumber.

| Seq | # | Iteration | Delivers |
| --- | --- | --- | --- |
| 1 | 1 | [Principles doc](01-principles-doc.md) | `docs/00-principles.md` — the doctrine page every later slice links back to |
| 2 | 2 | [VM core](02-vm-core.md) | `wovm`: `.wob` loader, register interpreter, arena, borrow word, `@gc` collector |
| 3 | 3 | [Compiler front](03-compiler-front.md) | `woc`: lexer → parser → typechecker → ownership pass, diagnostics |
| 4 | 4 | [Single binary end-to-end](04-single-binary-e2e.md) | emitter + conformance corpus + `woc build` self-contained binary |
| 5 | 5 | [Language surface](05-language-surface.md) | Haxe-parity adoptions: switch, records, optionals, try/catch, statics, modules… (`pub(read)`/`using`/`#if` remainders sit in the post-12 drain) |
| 6 | 6 | [Program mode + stdlib](06-program-mode-stdlib.md) | `fn main`, exit codes, `fs`/`proc`/`net`/`time`/`json` builtins |
| 7 | 7 | [log-watcher proof](07-logwatcher-proof.md) | the driving workload compiled, executable, soak-proven (landed 2026-08-15) |
| 8 | 7b | [Inferred GC + mark-sweep](07b-inferred-gc-mark-sweep.md) | `@gc` removed from the language; compiler infers GC-ness; RC replaced by incremental per-shard tri-color mark-sweep |
| 9 | 9 | [Database engine](09-database-engine.md) | class-shaped tables, typed WAL + recovery, `insert`/`select` execute |
| 10 | 9b | [`@table`, relations, query](09b-table-relations-query.md) | `@table` becomes real storage; typed `ref`/`backlink`/`multi` relations; compiler-checked LINQ-shaped queries lowered to engine ops |
| 11 | 15 | [deps: `wo.toml [deps]`](15-deps-package-manager.md) | exact-rev git dependencies + `wo.lock` + `.wo-deps` cache; `use <dep>` resolves a fetched project as a module root; flat-only, network-free when locked |
| 12 | 16 | [web framework](16-web-framework.md) | a `.wo`-library framework (HTTP/1.1 keep-alive behind a TLS-terminating proxy): router, `Handler`/`Middleware` interfaces, auth, all three body hooks, `@table` data layer; `docs/examples/web-app` consumes it via `[deps]`; h2c parked behind 8/9f/11 |
| **13** | **18** | [framework v2: memory-rich features](18-memory-db-features.md) | **NEXT — spec approved 2026-08-20**: TTL cache, @table feature flags with cached reads, durable @table job queue with drain-on-request, `transaction { }` exposing the WAL's staged batch (enqueue + write, one commit — no outbox) |
| 14 | 9c | [Cross-program tables](09c-cross-program-tables.md) | attach to a running program's database over a local IPC channel: manifest-granted rights, typed statements, owner stays the single writer (channel half-built) |
| 15 | 9d | [Keypair attach auth](09d-keypair-attach-auth.md) | program identity is a keypair: mutual challenge–response at attach, grants name public keys, replay-proof (crypto half-built; plan folds into 9c's) |
| 16 | 9e | [Durability, throughput, scale](09e-durability-throughput-scale.md) | restart-persistence proof, read/write benchmark, ~1M-row load; the baseline 8/9f/11 sign against |
| 17 | 8 | [Shard-actor runtime](08-shard-actor-runtime.md) | thread-per-core shards, per-shard heaps, ownership-move messaging (collector precondition met by 7b) |
| 18 | 9f | [io_uring group-commit](09f-io-uring-commit.md) | replace fsync-per-commit with io_uring batched durability on the shard tick; fsync fallback kept (after 8 + 9e, explicit) |
| 19 | 11 | [Fibers](11-fibers.md) | green threads on the shard scheduler: reduction-budget preemption, blocking builtins park; unparks h2c (with 8/9f), streaming, cancellation, pub/sub, fiber jobs |
| 20 | 10 | [HTTP service layer](10-http-service.md) | `service` blocks lower onto the framework (after 9b + 9c by their own precedence notes) |
| 21 | 12 | [Blue-green deploy](12-blue-green-deploy.md) | two VM slots, in-runtime compile, atomic switch, resident rollback (plan authored after 9 + 10) |
| 22 | 9g | [Query grammar corpus](09g-query-grammar-corpus.md) | grow the query grammar from real corpora; likely collapses to "confirm `len(query)` + add `exists`"; precedes 14 |
| 23 | 14 | [skillhost host workload](14-skillhost-host-workload.md) | host-shaped driving workload naming runtime gaps (bounded subprocess, stdin/stdout transport, fs metadata, FFI-vs-out-of-process) — demoted with the framework goal |
| 24 | 13 | [Compile-time metaprogramming](13-compile-time-metaprogramming.md) | `@derive(Json/Csv/Eq/Hash/Show)` from class-table metadata; held behind 12 with the parked drain by the 2026-08-08 scope directive |
| ⏸ | 17 | [library projects + `internal/`](17-library-projects-internal.md) | **PARKED** (spec + plan approved, branch `library-internal`) — `wo.toml` kind = "library" + Go's `internal/` rule; slots anywhere after 16 whenever directed, bringing the framework reorg with it |

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
  form, multipart). Implementation order for everything still pending:
  **18 (spec approved)** → 9c/9d → 9e → 8 → 9f → 11 (+ h2c unparks) →
  10 → 12 → 9g → 14 → 13 + parked drain; 17 parked, slots anywhere after
  16 on directive. The iterations table above carries this order
  row-by-row; edges live in
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
