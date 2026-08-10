# Status board — what is done, what is next

The single place to learn where this project stands. Organised in six buckets:
**stories** (the narrative arc), **in progress**, **done**, **pending**,
**discarded**, **learnings**. Every phase doc carries a matching status banner;
this board is the index.

Update this board in the same change that finishes work — move the item to done
with *what actually landed*, set the next in-progress item, and record any
rejection in [`discarded.md`](discarded.md) with its reason.

Statuses: ✅ **done** · 🔄 **in progress** · ⬜ **pending** · ⏸ **parked**

---

## ▶ NEXT PLAN

**Story iteration 4 — single binary end-to-end.**
Plan: [`compiler/plan/2026-08-01-wob-emit-e2e-single-binary.md`](compiler/2026-08-01-wob-emit-e2e-single-binary.md) ·
Story slice: [`docs/stories/language-runtime-database/04-single-binary-e2e.md`](../stories/language-runtime-database/04-single-binary-e2e.md)

The bytecode emitter, the three-kind conformance corpus, and `woc build`. This
is the milestone where `.wo` source becomes a running self-contained binary —
compiler front (iteration 3) and VM core (iteration 2) both shipped, so it is
unblocked. Its inputs are the four ownership tables `owner.ml` now produces;
`dump.ml`'s format-contract comments are normative for it, **including the
requirement to coalesce borrow guards per operand**.

Two tracks run in this repo. The critical path is the **language track**:
iterations 3 → 4 → 5 → 6 → 7, ending at *compile and run log-watcher*. The
Rust-runtime track is shipped-and-maintained, not advancing.

---

## Stories

[`docs/stories/language-runtime-database/`](../stories/language-runtime-database/00-story.md)
— one language, one runtime, one database, one binary. Twelve iterations, each
an unsplittable slice with Given/When/Then acceptance and a pointer to the plan
that sequences its tasks. Read one, approve, then the next starts.

| # | Iteration | State |
| --- | --- | --- |
| 1 | [Principles doc](../stories/language-runtime-database/01-principles-doc.md) | ✅ |
| 2 | [VM core (`wovm`)](../stories/language-runtime-database/02-vm-core.md) | ✅ |
| 3 | [Compiler front (`woc`)](../stories/language-runtime-database/03-compiler-front.md) | ✅ (known gaps below) |
| 4 | [Single binary end-to-end](../stories/language-runtime-database/04-single-binary-e2e.md) | 🔄 **next** |
| 5 | [Language surface](../stories/language-runtime-database/05-language-surface.md) | ⬜ |
| 6 | [Program mode + stdlib](../stories/language-runtime-database/06-program-mode-stdlib.md) | ⬜ |
| 7 | [log-watcher proof](../stories/language-runtime-database/07-logwatcher-proof.md) | ⬜ acceptance |
| 8 | [Shard-actor runtime](../stories/language-runtime-database/08-shard-actor-runtime.md) | ⬜ |
| 9 | [Database engine](../stories/language-runtime-database/09-database-engine.md) | ⬜ |
| 10 | [HTTP service layer](../stories/language-runtime-database/10-http-service.md) | ⬜ |
| 11 | [Fibers](../stories/language-runtime-database/11-fibers.md) | ⬜ |
| 12 | [Blue-green deploy](../stories/language-runtime-database/12-blue-green-deploy.md) | ⬜ |

---

## In progress

| Track | Item | Where |
| --- | --- | --- |
| Language | Iteration 4 — emitter, conformance corpus, `woc build` | [plan 3](compiler/2026-08-01-wob-emit-e2e-single-binary.md) |

Nothing else should be started until iteration 4 lands. Off-critical-path work
is parked by explicit scope directive (2026-08-08).

---

## Done

### Language track — compiler + VM (OOP track)

| Status | Item | Doc | What actually landed |
| --- | --- | --- | --- |
| ✅ | Principles | [`../00-principles.md`](../00-principles.md) | 13 principles, each with a why and a link to the doc that enforces it |
| ✅ | `wovm` VM core | [plan 1](../superpowers/plans/2026-08-01-wob-format-and-vm-core.md) | `.wob` v1 loader with full static validation, register interpreter (computed-goto + ISO-C fallback), arena with size-class free lists, borrow word, RC + budgeted Bacon–Rajan cycle collector, drop-map trap unwinding, containers, builtins, ICALL, CLI. 13 suites × 2 dispatch flavors + CLI smoke, ASan/UBSan clean |
| ✅ | `.wob` format contract | [`oop-vm/00-wob-format.md`](oop-vm/00-wob-format.md) | Normative; twinned with `runtime/src/wob.h` |
| ✅ | `woc` compiler front | [plan 2](compiler/2026-08-01-woc-compiler-front.md) | Tasks 1–8: dune scaffold, `diag` (WO-E codes, two-site related errors, ordered dedup), newline-significant lexer at rt parity, declaration + statement/expression parser with skip-on-block and multi-error recovery, typechecker (field kinds, `?T` plumbing, W201, E225, E214), MVS ownership pass with the four emitter tables, driver with directory discovery + cross-file programs. 14 + 264 checks |
| ✅ | Error catalog | [`oop-vm/01-error-catalog.md`](oop-vm/01-error-catalog.md) | 14 emitted codes + 10 reserved, each with the reason it is not yet emitted |
| ✅ | log-watcher `.wo` sample | [`../examples/log-watcher/`](../examples/log-watcher/README.md) | Eight-file port authored docs-first with its `.hx` mapping table; compiles for real in iteration 7 |
| ✅ | Scalar cleanup | [`discarded.md`](discarded.md) | `Money`/`SKU`/`Float` and the abstract allowlist removed; `abstract` flipped adopt → reject |

**Known gaps carried out of iteration 3** — recorded, not silently owed:

- **`?T` is plumbed but unenforced.** Lexer/token/AST/parser/dump all handle
  `?T`; the semantics do not exist (`WO-E211`/`E212`/`E213` declared, never
  emitted — a probe returning `?Int` as `Int` exits 0). Owned by iteration 5,
  plan 8 Task 6, which is that iteration's first task because it blocks the
  log-watcher port. See [`compiler/nullable-types-implementation.md`](compiler/nullable-types-implementation.md).
- **Structural interface satisfaction is not checked** (`WO-E205` dead), along
  with type mismatch, bad arity, and unknown-fn (`E201`/`E203`/`E204`) — all
  named in plan 2 Task 6's own must-fail list. Gaps in shipped work, catalogued
  as reserved.
- Six further narrowings (W201 heuristic, E225 reach, dead code after `return`,
  unresolved-callee drops, RC table ordering, residual b-side role) are listed
  in the plan-2 SDD ledger and in the affected files' own comments.

### Rust runtime track — Stage 2 shipped, maintained

| Status | Phase | Doc | Notes |
| --- | --- | --- | --- |
| ✅ | 01 crate scaffolding | [done/01](done/01-scafolding-crates.md) | 15 crates |
| ✅ | 02 epoll event loop | [done/02](done/02-event-loop-epoll.md) | `runtime/netpoll_epoll.rs` |
| ✅ | 03 hand-rolled HTTP | [done/03](done/03-hand-rolled-http.md) | + keep-alive & pipelining |
| ✅ | 04 tokio/axum cutover | [done/04](done/04-cutover-remove-tokio-axum.md) | deps now: anyhow, serde, serde_json, libc |
| ✅ | 09a thread-per-core | [09](09-concurrency-scaleout.md) | `scheduler.rs`, `SO_REUSEPORT`, pinned `wo-shard-<t>` workers |
| ✅ | 09b sharded engine | [09](09-concurrency-scaleout.md) | `shard.rs` bus; `Arc<Mutex<Engine>>` deleted; interleaved ids |
| ✅ | 09c per-shard WAL | [09](09-concurrency-scaleout.md) | ack-after-fsync; boot replay; `meta` shard guard |
| ✅ | — keep-alive follow-up | [09](09-concurrency-scaleout.md) | reads ×3.4 → 770k/s |
| ✅ | — io_uring group commit | [09](09-concurrency-scaleout.md) | raw ring; 4.7× durable writes on real disk |
| ✅ | 16a PG wire client | [16](16-postgres-mirror.md) | hand-rolled protocol v3, zero crates |
| ✅ | 16b PG backup mirror | [16](16-postgres-mirror.md) | async JSONB upserts behind the WAL ack; RAM authoritative |
| ✅ | 13a class surface | [13](13-class-model-live-pricing.md) | `class` parses, CRUD serves |
| ✅ | 13b method execution | [13](13-class-model-live-pricing.md) | row-scoped txn per call; abort → 409 rollback |
| ✅ | — `@table` + indexed DML | [13](13-class-model-live-pricing.md) | secondary indexes, `find_by`, `select Type{…}`, REST filters |
| ✅ | C proving ground A–F | [exploration/c-runtime/00-plan.md](exploration/c-runtime/00-plan.md) | 859k reads/s, 618k durable commits/s; found the ack-ordering + fd-ABA bugs the Rust port avoided |

Ecommerce sample (verified 2026-06-13): `api.rest` 17/17 expected statuses pass.

---

## Pending

### Language track — sequenced, on the critical path

| # | Item | Plan |
| --- | --- | --- |
| 5 | Haxe-parity language surface — **`?T` forced handling first**, then switch expressions, records, enum payloads, try/catch, statics, `using`, modules, `is`, `pub(read)`, `#if` | [plan 8](compiler/2026-08-01-haxe-parity-language.md) |
| 6 | Program mode + systems stdlib — `fn main`, exit codes, `fs`/`proc`/`net`/`time`/`json` | [plan 9](../superpowers/plans/2026-08-01-program-mode-stdlib.md) |
| 7 | log-watcher proof — the sample compiles and detects a silent death live | [plan 10](../superpowers/plans/2026-08-01-log-watcher-sample.md) |
| 8 | Shard-actor runtime | [plan 4](../superpowers/plans/2026-08-01-shard-actor-vm-runtime.md) |
| 9 | Database engine binding | [plan 5](../superpowers/plans/2026-08-01-db-engine-binding.md) |
| 10 | HTTP service layer | [plan 6](../superpowers/plans/2026-08-01-http-service-layer.md) |
| 11 | Fibers | vision §3, [blue-green exploration](exploration/blue-green-vm/00-vision.md) |
| 12 | Blue-green deploy | [spec](../superpowers/specs/2026-08-03-blue-green-vm-design.md) — plan authored after iterations 9–10 |

### Language track — parked until after iteration 12

Recorded 2026-08-08 by scope directive; nothing here lands before the
log-watcher proof.

- `WO-W201` `@gc`-suggestion refinement beyond the self-reference heuristic
- `WO-E225` broadened to `ref`/`multi`/`map` element types and fn signatures
- ADT container roster adoption (Stack, Queue, Set, Tree, Graph, …) — see the
  roster in [`compiler/nullable-types-implementation.md`](compiler/nullable-types-implementation.md)
- Web framework as a `.wo` library; UI (`##ui` SSR + live patches);
  script-based destructive migrations; MCP/agent wrapper over the management plane

### Rust runtime track — not advancing while the language track runs

| Status | Phase | Doc | Notes |
| --- | --- | --- | --- |
| ⬜ | 05 hand-rolled JSON | [05](05-hand-rolled-json.md) | removes serde/serde_json |
| ⬜ | 06 bespoke error type | [06](06-bespoke-error.md) | removes anyhow |
| ⬜ | 07 inotify content watcher | [07](07-inotify-content-watcher.md) | `wo dev` hot reload |
| ⬜ | 08 sendfile static assets | [08](08-sendfile-static-assets.md) | needed by the parked UI track |
| ⬜ | 09d cross-shard subscriptions | [09](09-concurrency-scaleout.md) | LIVE fan-out; pairs with Stage 3 |
| ⬜ | 09e cross-shard transactions (2PC) | [09](09-concurrency-scaleout.md) | needed by `fn checkout` spanning shards |
| ⬜ | 09f observability & reshard | [09](09-concurrency-scaleout.md) | per-shard metrics, `WO_RESHARD` |
| ⬜ | 10–12 storage completion | [10](10-storage-foundations.md), [11](11-wal-and-recovery.md), [12](12-engine-disk-cutover.md) | snapshots, compaction, WAL rotation, mmap arena engine |
| ⬜ | 13c LIVE pricing push · Stage 3 wire layer · 13e at scale | [13](13-class-model-live-pricing.md) | replaces the 501 stub |
| ⬜ | 15a–15e MCP over streamable HTTP | [15](15-mcp-streamable-http.md) | 15e needs 13c + 09d |
| ⬜ | 16c–16f typed columns, lossless resync, restore, SCRAM | [16](16-postgres-mirror.md) | |

### Frontend — parked

| Status | Phase | Doc |
| --- | --- | --- |
| ⏸ | 13d pricing UI | [13](13-class-model-live-pricing.md) |
| ⏸ | 14 MVC UI implementation (14a–f) | [14](14-mvc-ui-implementation.md) |
| ⏸ | UI exploration track | [exploration/ui/00-overview.md](exploration/ui/00-overview.md) |

---

## Discarded

Settled rejections with their reasons live in [`discarded.md`](discarded.md) —
inheritance, `abstract` newtypes, `Money`/`SKU`/`Float`, `Dynamic`/`cast`/
`macro`/`extern`, AOT-to-C, Menhir, shared mutable engine state, external
deployer daemon, destructive migrations in v1, and more. Argue against the
recorded reason rather than re-opening an entry as new.

## Learnings

What attempts taught, shipped or not, in [`learnings.md`](learnings.md) —
plumbed-is-not-enforced, vacuously-passing goldens, exit-0-with-wrong-output,
the malloc-path ASan trick, deferred checks that never reach the runtime,
validate-once-at-the-boundary, and reference-implement-in-C-first.
