# Status board — what is done, what is next

The single place to learn where this project stands. Organised in six buckets:
**stories** (the narrative arc), **in progress**, **done**, **pending**,
**discarded**, **learnings**. The buckets are **sections of this board, not
folders** — a doc stays where it was authored when its work lands; only its
banner and this board change. Every plan and phase doc opens with a
`> **Status:**` banner linking back here; normative contracts
(`plan/oop-vm/`), exploration studies, reference docs and the
discarded/learnings registers carry none by design.

Update this board in the same change that finishes work — move the item to done
with _what actually landed_, set the next in-progress item, and record any
rejection in [`discarded.md`](plan/discarded.md) with its reason.

Statuses: ✅ **done** · 🔄 **in progress** · ⬜ **pending** · ⏸ **hold**

---

## ▶ NEXT PLAN

**Story iteration 5 — language surface (Haxe-parity adoptions).**
Plan: [`plan/compiler/2026-08-01-haxe-parity-language.md`](plan/compiler/2026-08-01-haxe-parity-language.md) ·
Story slice: [`docs/stories/language-runtime-database/05-language-surface.md`](stories/language-runtime-database/05-language-surface.md)

The language grows from milestone grammar to a daily-driver surface: every
**adopt** row of the systems-track verdict table (switch expressions, typedef
records, `?T` optionals, enum payloads, try/catch, statics, `using`, modules,
`is`, `pub(read)`, `#if`) lands with a golden + must-fail fixture pair; every
**reject** row refuses with a doctrine-citing diagnostic. **First task: `?T`
forced handling (plan 8 Task 6)** — plumbed since iteration 3 but unenforced
(`WO-E211`–`E213` dead), and the log-watcher port (iterations 6–7) uses
optionals throughout in place of the Haxe original's sentinel values, so
nothing else in this plan can land ahead of it.

Two tracks run in this repo. The critical path is the **language track**:
iterations 3 → 4 → 5 → 6 → 7, ending at _compile and run log-watcher_. The
Rust-runtime track is shipped-and-maintained, not advancing.

---

## Stories

[`docs/stories/language-runtime-database/`](stories/language-runtime-database/00-story.md)
— one language, one runtime, one database, one binary. Twelve iterations, each
an unsplittable slice with Given/When/Then acceptance and a pointer to the plan
that sequences its tasks. Read one, approve, then the next starts.

| #   | Iteration                                                                                    | State                        |
| --- | -------------------------------------------------------------------------------------------- | ---------------------------- | ---- |
| 1   | [Principles doc](stories/language-runtime-database/01-principles-doc.md)                     | ✅                           |
| 2   | [VM core (`wovm`)](stories/language-runtime-database/02-vm-core.md)                          | ✅                           |
| 3   | [Compiler front (`woc`)](stories/language-runtime-database/03-compiler-front.md)             | ✅ (known gaps below)        |
| 4   | [Single binary end-to-end](stories/language-runtime-database/04-single-binary-e2e.md)        | ✅ (known gaps below)        |
| 5   | [Language surface](stories/language-runtime-database/05-language-surface.md)                 | 🔄 **next**                  |
| 6   | [Program mode + stdlib](stories/language-runtime-database/06-program-mode-stdlib.md)         | ⬜                           |
| 7   | [log-watcher proof](stories/language-runtime-database/07-logwatcher-proof.md)                | ⬜ acceptance                |
| 7b  | [Inferred GC + mark-sweep](stories/language-runtime-database/07b-inferred-gc-mark-sweep.md)  | ⬜ closes iteration 4's gate |
| 8   | [Shard-actor runtime](stories/language-runtime-database/08-shard-actor-runtime.md)           | ⬜                           |
| 9   | [Database engine](stories/language-runtime-database/09-database-engine.md)                   | ⬜                           |
| 9b  | [`@table`, relations, query](stories/language-runtime-database/09b-table-relations-query.md) | ⬜ needs a spec first        |
| 10  | [HTTP service layer](stories/language-runtime-database/10-http-service.md)                   | ⬜                           | Hold |
| 11  | [Fibers](stories/language-runtime-database/11-fibers.md)                                     | ⬜                           | Hold |
| 12  | [Blue-green deploy](stories/language-runtime-database/12-blue-green-deploy.md)               | ⬜                           | Hold |

---

## In progress

| Track    | Item                                                                   | Where                                                      |
| -------- | ---------------------------------------------------------------------- | ---------------------------------------------------------- |
| Language | Iteration 5 — Haxe-parity language surface, `?T` forced handling first | [plan 8](plan/compiler/2026-08-01-haxe-parity-language.md) |

Nothing else should be started until iteration 5 lands. Off-critical-path work
is parked by explicit scope directive (2026-08-08).

---

## Done

### Language track — compiler + VM (OOP track)

| Status | Item                                 | Doc                                                              | What actually landed                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          |
| ------ | ------------------------------------ | ---------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| ✅     | Principles                           | [`../00-principles.md`](00-principles.md)                        | 13 principles, each with a why and a link to the doc that enforces it                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                         |
| ✅     | `wovm` VM core                       | [plan 1](superpowers/plans/2026-08-01-wob-format-and-vm-core.md) | `.wob` v1 loader with full static validation, register interpreter (computed-goto + ISO-C fallback), arena with size-class free lists, borrow word, RC + budgeted Bacon–Rajan cycle collector, drop-map trap unwinding, containers, builtins, ICALL, CLI. 13 suites × 2 dispatch flavors + CLI smoke, ASan/UBSan clean                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                        |
| ✅     | `.wob` format contract               | [`oop-vm/00-wob-format.md`](plan/oop-vm/00-wob-format.md)        | Normative; twinned with `runtime/src/wob.h`                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   |
| ✅     | `woc` compiler front                 | [plan 2](plan/compiler/2026-08-01-woc-compiler-front.md)         | Tasks 1–8: dune scaffold, `diag` (WO-E codes, two-site related errors, ordered dedup), newline-significant lexer at rt parity, declaration + statement/expression parser with skip-on-block and multi-error recovery, typechecker (field kinds, `?T` plumbing, W201, E225, E214), MVS ownership pass with the four emitter tables, driver with directory discovery + cross-file programs. 14 + 264 checks                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                     |
| ✅     | Error catalog                        | [`oop-vm/01-error-catalog.md`](plan/oop-vm/01-error-catalog.md)  | 14 emitted codes + 10 reserved, each with the reason it is not yet emitted                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    |
| ✅     | log-watcher `.wo` sample             | [`../examples/log-watcher/`](examples/log-watcher/README.md)     | Eight-file port authored docs-first with its `.hx` mapping table; compiles for real in iteration 7                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            |
| ✅     | Scalar cleanup                       | [`discarded.md`](plan/discarded.md)                              | `Money`/`SKU`/`Float` and the abstract allowlist removed; `abstract` flipped adopt → reject                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   |
| ✅     | `woc` emitter, corpus, single binary | [plan 3](plan/compiler/2026-08-01-wob-emit-e2e-single-binary.md) | Tasks 1–6 + 8 (Task 7, a parity harness against the Rust runtime, **deferred by explicit user decision** — the two stacks diverge by design). Bytecode emitter (`emit.ml`) + disassembler (`disasm.ml`, `--dump-bc`); three-kind conformance harness (`scripts/oop-e2e.sh`, `just oop-e2e`) over `tests/corpus/{run,compile-fail,trap,gc}`; pricing-demo + ownership/trap corpora (19 fixtures); `@gc` cycle collector's post-exit pump (`WO_GC_BUDGET`/`WO_GC_TRACE`) + 2 gc fixtures (`gc/held-cycle` retired — see criterion-3 closure below); `woc build` single-binary output + relocation/corrupt-trailer smoke; `WO-E405` closing criterion 3's ASan leak (entry must return `Int`); `just oop-accept` wiring all five spec criteria + both unit gates into one command. 14 + 399 compiler checks; `oop-e2e` 25/25 against the release `wovm`. **Milestone-1 acceptance gate is fully green — all five criteria met** (see the dated acceptance note in `docs/superpowers/specs/2026-08-01-oop-compiler-vm-design.md`) |

**Known gaps carried out of iteration 3** — recorded, not silently owed:

- **`?T` is plumbed but unenforced.** Lexer/token/AST/parser/dump all handle
  `?T`; the semantics do not exist (`WO-E211`/`E212`/`E213` declared, never
  emitted — a probe returning `?Int` as `Int` exits 0). Owned by iteration 5,
  plan 8 Task 6, which is that iteration's first task because it blocks the
  log-watcher port. See [`compiler/nullable-types-implementation.md`](plan/compiler/nullable-types-implementation.md).
- **Structural interface satisfaction is not checked** (`WO-E205` dead), along
  with type mismatch, bad arity, and unknown-fn (`E201`/`E203`/`E204`) — all
  named in plan 2 Task 6's own must-fail list. Gaps in shipped work, catalogued
  as reserved.
- Six further narrowings (W201 heuristic, E225 reach, dead code after `return`,
  unresolved-callee drops, RC table ordering, residual b-side role) are listed
  in the plan-2 SDD ledger and in the affected files' own comments.

**Known gaps carried out of iteration 4** — recorded, not silently owed:

- **`WO-E205` (unsatisfied interface) is reachable but unenforced — a real
  hybrid-boundary inversion, not just a dead code path.** A class that does
  not structurally satisfy an interface it's passed as compiles clean (exit
  0, zero diagnostics) even though the violation is statically provable, and
  the mismatched call reaches `wovm` as an `ICALL` with no matching vtable
  entry, trapping `WO_T_BOUNDS` (6) at runtime instead of failing at compile
  time. Pinned by `tests/corpus/trap/unsatisfied-interface/`; when `WO-E205`
  is wired, that fixture must move to `compile-fail/` in the same change.
- **`set(m, k, v)`'s `@gc` retention gap on map keys/values is open** — the
  twin of the `push` bug Task 5 fixed for `multi`. `set` has no equivalent
  special case in `owner.ml`'s `analyze_call`, so a `@gc` key or value handed
  to `set` is under-counted and the collector can free it while the map still
  points at it. Nothing in the corpus exercises this yet. See
  [`oop-vm/08-builtin-surface.md`](plan/oop-vm/08-builtin-surface.md).
- **E201/E203 and seven other `WO-E2xx` codes remain declared but unemitted**
  — see [`oop-vm/01-error-catalog.md`](plan/oop-vm/01-error-catalog.md).
- **CLOSED — milestone-1's ASan gate (`just oop-accept`) failing on
  `gc/held-cycle`.** Root cause (Task 8's finding, restated): `main.c`'s
  entry-method return value (`uint64_t ret`, `src/main.c:158`) is stored
  but never released, so `gc/held-cycle`'s "permanent external hold" was
  actually a permanent refcount inflation — LeakSanitizer's "definite
  leak" (1184 bytes / 3 allocations) was correctly reporting exactly
  that, not a false positive. Fixing it by releasing `ret` was rejected:
  the `.wob` method table carries no return-type/kind metadata, so
  `main.c` has no way to know `ret` is a pointer rather than a scalar,
  and adding that metadata is a format change out of scope here. Fixed
  instead at the source: the systems-track spec already requires the
  entry to return `Int` (its return value is the process exit code), so
  a class-returning `main` was never legal — `WO-E405`
  (`compiler/src/emit.ml`, `01-error-catalog.md`) now rejects it at
  compile time, and `gc/held-cycle` is retired because its premise (an
  externally-held cycle survives a _post-exit_ pump) is no longer
  expressible — see `oop-vm/02-corpus.md`'s "Retired" note for why, and
  for where the scenario it meant to cover is actually proven
  (`runtime/test/test_cycle.c`, plus a proper in-flight fixture scheduled
  for story iteration 7b). Spec success criterion 3 is now **MET**;
  `just oop-accept` passes all five criteria.

### Rust runtime track — Stage 2 shipped, maintained

| Status | Phase                    | Doc                                                                       | Notes                                                                                            |
| ------ | ------------------------ | ------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------ |
| ✅     | 01 crate scaffolding     | [done/01](plan/done/01-scafolding-crates.md)                              | 15 crates                                                                                        |
| ✅     | 02 epoll event loop      | [done/02](plan/done/02-event-loop-epoll.md)                               | `runtime/netpoll_epoll.rs`                                                                       |
| ✅     | 03 hand-rolled HTTP      | [done/03](plan/done/03-hand-rolled-http.md)                               | + keep-alive & pipelining                                                                        |
| ✅     | 04 tokio/axum cutover    | [done/04](plan/done/04-cutover-remove-tokio-axum.md)                      | deps now: anyhow, serde, serde_json, libc                                                        |
| ✅     | 09a thread-per-core      | [09](plan/09-concurrency-scaleout.md)                                     | `scheduler.rs`, `SO_REUSEPORT`, pinned `wo-shard-<t>` workers                                    |
| ✅     | 09b sharded engine       | [09](plan/09-concurrency-scaleout.md)                                     | `shard.rs` bus; `Arc<Mutex<Engine>>` deleted; interleaved ids                                    |
| ✅     | 09c per-shard WAL        | [09](plan/09-concurrency-scaleout.md)                                     | ack-after-fsync; boot replay; `meta` shard guard                                                 |
| ✅     | — keep-alive follow-up   | [09](plan/09-concurrency-scaleout.md)                                     | reads ×3.4 → 770k/s                                                                              |
| ✅     | — io_uring group commit  | [09](plan/09-concurrency-scaleout.md)                                     | raw ring; 4.7× durable writes on real disk                                                       |
| ✅     | 16a PG wire client       | [16](plan/16-postgres-mirror.md)                                          | hand-rolled protocol v3, zero crates                                                             |
| ✅     | 16b PG backup mirror     | [16](plan/16-postgres-mirror.md)                                          | async JSONB upserts behind the WAL ack; RAM authoritative                                        |
| ✅     | 13a class surface        | [13](plan/13-class-model-live-pricing.md)                                 | `class` parses, CRUD serves                                                                      |
| ✅     | 13b method execution     | [13](plan/13-class-model-live-pricing.md)                                 | row-scoped txn per call; abort → 409 rollback                                                    |
| ✅     | — `@table` + indexed DML | [13](plan/13-class-model-live-pricing.md)                                 | secondary indexes, `find_by`, `select Type{…}`, REST filters                                     |
| ✅     | C proving ground A–F     | [exploration/c-runtime/00-plan.md](plan/exploration/c-runtime/00-plan.md) | 859k reads/s, 618k durable commits/s; found the ack-ordering + fd-ABA bugs the Rust port avoided |

Ecommerce sample (verified 2026-06-13): `api.rest` 17/17 expected statuses pass.

---

## Pending

### Language track — sequenced, on the critical path

| #   | Item                                                                                                                                                                           | Plan                                                                                               |
| --- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | -------------------------------------------------------------------------------------------------- |
| 5   | Haxe-parity language surface — **`?T` forced handling first**, then switch expressions, records, enum payloads, try/catch, statics, `using`, modules, `is`, `pub(read)`, `#if` | [plan 8](plan/compiler/2026-08-01-haxe-parity-language.md)                                         |
| 6   | Program mode + systems stdlib — `fn main`, exit codes, `fs`/`proc`/`net`/`time`/`json`                                                                                         | [plan 9](superpowers/plans/2026-08-01-program-mode-stdlib.md)                                      |
| 7   | log-watcher proof — the sample compiles and detects a silent death live                                                                                                        | [plan 10](superpowers/plans/2026-08-01-log-watcher-sample.md)                                      |
| 7b  | Inferred GC + incremental mark-sweep — `@gc` removed, GC-ness inferred, RC retired                                                                                             | [spec](superpowers/specs/2026-08-11-inferred-gc-mark-sweep-design.md) — plan to be written         |
| 8   | Shard-actor runtime                                                                                                                                                            | [plan 4](superpowers/plans/2026-08-01-shard-actor-vm-runtime.md)                                   |
| 9   | Database engine binding                                                                                                                                                        | [plan 5](superpowers/plans/2026-08-01-db-engine-binding.md)                                        |
| 9b  | `@table` + relations + language-integrated query                                                                                                                               | **no spec yet** — three open forks recorded in the iteration; brainstorm before planning           |
| 10  | HTTP service layer                                                                                                                                                             | [plan 6](superpowers/plans/2026-08-01-http-service-layer.md)                                       |
| 11  | Fibers                                                                                                                                                                         | vision §3, [blue-green exploration](plan/exploration/blue-green-vm/00-vision.md)                   |
| 12  | Blue-green deploy                                                                                                                                                              | [spec](superpowers/specs/2026-08-03-blue-green-vm-design.md) — plan authored after iterations 9–10 |

### Language track — parked until after iteration 12

Recorded 2026-08-08 by scope directive; nothing here lands before the
log-watcher proof.

- `WO-W201` `@gc`-suggestion refinement beyond the self-reference heuristic
- `WO-E225` broadened to `ref`/`multi`/`map` element types and fn signatures
- ADT container roster adoption (Stack, Queue, Set, Tree, Graph, …) — see the
  roster in [`compiler/nullable-types-implementation.md`](plan/compiler/nullable-types-implementation.md)
- Web framework as a `.wo` library; UI (`##ui` SSR + live patches);
  script-based destructive migrations; MCP/agent wrapper over the management plane
- `throw` (explicit raise) — cut 2026-08-10, 0 uses in the driving workload
  (log-watcher); catch frames ship without it
- `time.mono` — cut 2026-08-10, 0 uses in the driving workload; returns when a
  workload needs monotonic math
- `is` — cut 2026-08-10, 0 uses in the driving workload; emptied plan 8's old
  Task 7, which is deleted rather than deferred

### Rust runtime track — not advancing while the language track runs

| Status | Phase                                                     | Doc                                                                                                           | Notes                                                  |
| ------ | --------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------ |
| ⬜     | 05 hand-rolled JSON                                       | [05](plan/05-hand-rolled-json.md)                                                                             | removes serde/serde_json                               |
| ⬜     | 06 bespoke error type                                     | [06](plan/06-bespoke-error.md)                                                                                | removes anyhow                                         |
| ⬜     | 07 inotify content watcher                                | [07](plan/07-inotify-content-watcher.md)                                                                      | `wo dev` hot reload                                    |
| ⬜     | 08 sendfile static assets                                 | [08](plan/08-sendfile-static-assets.md)                                                                       | needed by the parked UI track                          |
| ⬜     | 09d cross-shard subscriptions                             | [09](plan/09-concurrency-scaleout.md)                                                                         | LIVE fan-out; pairs with Stage 3                       |
| ⬜     | 09e cross-shard transactions (2PC)                        | [09](plan/09-concurrency-scaleout.md)                                                                         | needed by `fn checkout` spanning shards                |
| ⬜     | 09f observability & reshard                               | [09](plan/09-concurrency-scaleout.md)                                                                         | per-shard metrics, `WO_RESHARD`                        |
| ⬜     | 10–12 storage completion                                  | [10](plan/10-storage-foundations.md), [11](plan/11-wal-and-recovery.md), [12](plan/12-engine-disk-cutover.md) | snapshots, compaction, WAL rotation, mmap arena engine |
| ⬜     | 13c LIVE pricing push · Stage 3 wire layer · 13e at scale | [13](plan/13-class-model-live-pricing.md)                                                                     | replaces the 501 stub                                  |
| ⬜     | 15a–15e MCP over streamable HTTP                          | [15](plan/15-mcp-streamable-http.md)                                                                          | 15e needs 13c + 09d                                    |
| ⬜     | 16c–16f typed columns, lossless resync, restore, SCRAM    | [16](plan/16-postgres-mirror.md)                                                                              |                                                        |

### Frontend — parked

| Status | Phase                            | Doc                                                                 |
| ------ | -------------------------------- | ------------------------------------------------------------------- |
| ⏸      | 13d pricing UI                   | [13](plan/13-class-model-live-pricing.md)                           |
| ⏸      | 14 MVC UI implementation (14a–f) | [14](plan/14-mvc-ui-implementation.md)                              |
| ⏸      | UI exploration track             | [exploration/ui/00-overview.md](plan/exploration/ui/00-overview.md) |

---

## Discarded

Settled rejections with their reasons live in [`discarded.md`](plan/discarded.md) —
inheritance, `abstract` newtypes, `Money`/`SKU`/`Float`, `Dynamic`/`cast`/
`macro`/`extern`, AOT-to-C, Menhir, shared mutable engine state, external
deployer daemon, destructive migrations in v1, and more. Argue against the
recorded reason rather than re-opening an entry as new.

## Learnings

What attempts taught, shipped or not, in [`learnings.md`](plan/learnings.md) —
plumbed-is-not-enforced, vacuously-passing goldens, exit-0-with-wrong-output,
the malloc-path ASan trick, deferred checks that never reach the runtime,
validate-once-at-the-boundary, and reference-implement-in-C-first.
