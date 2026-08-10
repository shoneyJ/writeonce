# Story — one language, one runtime, one database, one binary

> Format: fiberloom `product/story-template` (tech-unit standard artifact).
> Sources consulted: fiberloom `tech-unit/framework`, `product/story-template`, `product/story-iteration-template`.

**AS** a developer building and operating my own products end to end

**I WANT** a new statically-typed systems programming language — object-oriented by default, with ownership-based memory safety (borrow semantics) and per-class `@gc` garbage collection where I opt in — whose compiler, runtime, and database ship as a single never-stopping Linux binary that can update its own code in place

**TO** write an application once and run it forever: no external stack to assemble, no database server to operate, and deployments that swap code inside the running process with instant rollback.

## User Value

- One artifact is the whole system: the language's runtime owns the data,
  serves the API, and carries its own source — the "which commit is prod
  running?" class of questions disappears.
- Memory safety without a GC tax: Rust-shaped borrowing (single owner,
  second-class borrows) checked mostly at compile time, with per-class `@gc`
  opt-in collected per shard — no global pause exists by construction.
- Static typing all the way down: every slot's type is known at compile
  time, so the VM runs untagged 64-bit registers — no `Dynamic`, no boxing
  tax, no hashed field lookups (the Haxe→C++ reference workload pays all
  three).
- The database is the object model: a `@table` class is a table, a
  `ref`/`multi` field is a relation — the `@`-annotations are the built-in
  ORM, resolved at compile time. No external mapping layer, and no
  user-defined macros (the verdict table's rejection stands; annotations
  are compiler-known).
- Updates are blue-green **inside** the runtime: propose, approve, compile
  in-process, atomic switch, previous version resident for instant rollback.
- The runtime is a recipe box: once language + runtime + database exist, a
  web framework arrives as a `.wo` library composing runtime capabilities —
  the recorded next story.

## Background & Constraints

writeonce began as a declarative Rust-based runtime (Stage 2); that chapter
closes here. This story pivots it into a statically-typed, object-oriented
**systems programming language** with its own runtime and database (`woc`
OCaml compiler, `wovm` C VM) per the approved specs: C as the runtime's basis (libc only), no
inheritance ever, mutable value semantics for borrowing, shard-per-core
concurrency with ownership-moving messages, RAM-authoritative data under a
WAL, and the blue-green VM pair for in-runtime deployment. Target OS is
Linux; kernel primitives are the framework. The Rust runtime retires only at
parity. Constraints: OCaml stdlib only, C libc only; docs live under
`docs/`; prose-only planning artifacts (no implementation code in stories or
iterations); no commits by agents — drafts go to `.dev/commit.md`.

## Iterations (review in this order — the order is chronological)

| # | Status | Iteration | Delivers |
| --- | --- | --- | --- |
| 1 | ✅ | [Principles doc](01-principles-doc.md) | `docs/00-principles.md` — the doctrine page every later slice links back to |
| 2 | 🔄 | [VM core](02-vm-core.md) | `wovm`: `.wob` loader, register interpreter, arena, borrow word, `@gc` RC (cycles staged to 8) |
| 3 | 🔄 | [Compiler front](03-compiler-front.md) | `woc`: lexer → parser → typechecker → ownership pass, diagnostics |
| 4 | ⬜ | [Single binary end-to-end](04-single-binary-e2e.md) | emitter + conformance corpus + `woc build` self-contained binary |
| 5 | ⬜ | [Language surface](05-language-surface.md) | Haxe-parity adoptions: switch, records, optionals, try/catch, statics, modules… |
| 6 | ⬜ | [Program mode + stdlib](06-program-mode-stdlib.md) | `fn main`, exit codes, `fs`/`proc`/`net`/`time`/`json` builtins |
| 7 | ⬜ | [log-watcher proof](07-logwatcher-proof.md) | the driving workload compiled and detecting silent deaths live |
| 8 | ⬜ | [Shard-actor runtime](08-shard-actor-runtime.md) | thread-per-core shards, per-shard heaps, ownership-move messaging, `@gc` cycle collector |
| 9 | ⬜ | [Database engine](09-database-engine.md) | class-shaped tables, typed WAL + recovery, `insert`/`select` execute |
| 10 | ⬜ | [HTTP service layer](10-http-service.md) | `service` blocks route to VM methods; REST parity with Stage 2 |
| 11 | ⬜ | [Fibers](11-fibers.md) | green threads on the shard scheduler: reduction-budget preemption, park-on-I/O builtins, ownership-move sends |
| 12 | ⬜ | [Blue-green deploy](12-blue-green-deploy.md) | two VM slots, in-runtime compile, atomic switch, resident rollback |

Chronology notes: 2 and 3 are the one deliberate overlap — the OOP spec's
first sub-project builds both halves concurrently (both in progress:
`runtime/src/` carries the VM's loader/vm/obj/borrow/gc modules,
`compiler/` has front-end tasks 1–4 of 8); they meet in 4. Iterations 1–7
complete the single-shard language; 8–12 scale the runtime (8 is the
substrate for everything after: the DB engine lives in its shards, HTTP
serves from them, fibers refine their scheduler, blue-green drains them).
Fibers (11) land before blue-green so the deploy's drain can unwind parked
fibers as part of its own acceptance; blue-green (12) closes the story —
its plan is authored only after 9–10 ship.

Review protocol: the developer reads one iteration, approves or amends;
the next starts only after approval. Each iteration is an unsplittable
value slice with its own acceptance criteria (Given/When/Then), out-of-scope
list, and a pointer to the plan document that already sequences its tasks.

## Related Links

- OOP core spec: `docs/superpowers/specs/2026-08-01-oop-compiler-vm-design.md`
- Systems track spec: `docs/superpowers/specs/2026-08-01-systems-track-design.md`
- Blue-green spec: `docs/superpowers/specs/2026-08-03-blue-green-vm-design.md`
- Sample+principles spec: `docs/superpowers/specs/2026-08-07-logwatcher-sample-and-principles-design.md`
- Plan documents: `docs/superpowers/plans/` (plans 1–10), `docs/plan/compiler/` (2, 3, 8 + architecture)
- Roadmap map: `docs/08-project-structure.md` (build sequence)
- Behavioral reference workload: `~/projects/log-watcher` (Haxe daemon)

## Notes

- Acceptance criteria live in each iteration file; this story frames the
  outcome.
- Recorded future stories, deliberately outside this one: the web framework
  as a `.wo` library over the recipe-box runtime; UI (`##ui` SSR + live
  patches); script-based destructive schema migrations; MCP/agent wrapper
  over the management plane.
