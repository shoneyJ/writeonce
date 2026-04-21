# 01 — Scaffolding the Crate Tree

**Context sources:** [`../../CLAUDE.md`](../../CLAUDE.md), [`../../README.md`](../../README.md), [`../../crates/README.md`](../../crates/README.md), [`../runtime/database.md`](../runtime/database.md), [`../runtime/database/07-wo-seg-migration.md`](../runtime/database/07-wo-seg-migration.md).

## Goal

Lay out the full `crates/` directory tree that the 7-phase `.wo` runtime design implies, **without moving any active code**. Every target crate the design docs name gets an empty-but-compilable home so that:

- every doc reference to `ql`, `wal`, `policy`, etc. resolves to a real directory
- each phase's extraction work (move X out of `rt` into its target crate) is a mechanical copy into an existing skeleton rather than a net-new crate creation
- IDE workspace views, dependency graphs, and `cargo doc` show the project's intended shape on day one

## Design decisions (locked)

1. **Scope: all 7 phases.** 14 new empty library crates covering Phases 2 → 6 land in one pass. `rt` (already shipping Stage 2) is the 15th.
2. **`rt` stays monolithic.** Today's Stage 2 code — lexer / parser / AST / compile / engine / server — stays inside `crates/rt/` and continues to satisfy the 14 existing unit tests. Code migrates into the new crates as each phase activates, not in this pass.
3. **Contents: `Cargo.toml` + `src/lib.rs` doc-comment only.** Each `lib.rs` is one module-level `//!` doc block pointing at the phase doc, naming the responsibilities, and flagging which modules in `rt` migrate here later. No placeholder types, no stub traits.
4. **No `wo-` prefix.** New runtime crates are `ql`, `value`, `engine`, etc. — not `wo-ql`, `wo-value`. The prefix is redundant inside the project's own `wo` namespace and noisy in imports (`use ql::Parser` beats `use wo_ql::Parser`). The v1 crates in `reference/crates/` keep their `wo-` prefix — the distinct prefix makes the v1/v2 split visible at a glance.
5. **Workspace membership: root `Cargo.toml` lists every new crate as a member.** `reference/crates` stays `exclude`-d (nested workspace, separate v1 code).

Rationale and alternatives considered: see [`../../CLAUDE.md`](../../CLAUDE.md) "What's in `rt` today vs. what the empty crates promise" and the recorded `AskUserQuestion` answers that preceded this plan.

## Crate map

All names are stable — documented in [`../runtime/database/07-wo-seg-migration.md`](../runtime/database/07-wo-seg-migration.md) (Phase 2–5) and derived from [`../runtime/database/06-lowcode-fullstack.md`](../runtime/database/06-lowcode-fullstack.md) component tables (Phase 6).

| Phase | Crate | One-line purpose |
| --- | --- | --- |
| 2 | `ql`      | `.wo` grammar — lexer, parser, AST |
| 2 | `value`   | tagged `Value` + dotted-path helpers |
| 2 | `engine`  | in-memory executor (rel / doc / graph) + schema catalog |
| 2 | `txn`     | transaction coordinator — MVCC, `RETURNING` alias table |
| 2 | `db`      | top-level facade — `open()`, `Tx`, `Query`, `Subscribe` |
| 3 | `wal`     | write-ahead log — io_uring + fsync + recovery |
| 4 | `sub`     | live subscriptions — delta frames on commit |
| 4 | `http`    | wire protocol — REST / GraphQL-over-WS / native codec |
| 5 | `gen`     | codegen — `.wo type` → Go / TS / Rust / Python clients |
| 6 | `policy`  | RBAC + row-level rules compiled into planner rewrites |
| 6 | `logic`   | `on <event>` triggers + `fn ... in txn` interpreter |
| 6 | `service` | `service rest/graphql/native` endpoint dispatch |
| 6 | `ui`      | `##ui` screens → SSR HTML + client runtime |
| 6 | `app`     | `##app` route manifest + startup hooks |
| — | `rt`      | **existing** — Stage-2 monolith + the `wo` binary |

## Status

✅ **Done.** All 15 crates exist, the root workspace members list includes them, `cargo build` and `cargo test --lib` both pass, and `cargo run --bin wo -- run docs/examples/blog` still serves the blog sample (Stage 2 behaviour unchanged).

| Artifact | Status |
| --- | --- |
| 14 new crate skeletons (`Cargo.toml` + `src/lib.rs`) | ✅ |
| Root `Cargo.toml` lists all 15 crates as members | ✅ |
| `crates/README.md` with the phase-mapped inventory | ✅ |
| `cargo build` at root (compiles 15 crates) | ✅ |
| `cargo test --lib` at root (14 existing `rt` tests) | ✅ |
| `cd reference/crates && cargo build && cargo test` (v1 still green) | ✅ |
| `cargo run --bin wo -- run docs/examples/blog` (Stage 2 still serves) | ✅ |

## Non-scope

- **No code extraction.** Moving `lexer.rs` / `parser.rs` / `engine.rs` out of `rt` into `ql` / `engine` is explicitly deferred. That happens incrementally as each phase activates.
- **No `gen` binary target.** `gen` ships as a library-only crate in this pass. The `[[bin]]` lands when Phase 5 starts.
- **No test scaffolding.** The empty crates don't get unit-test stubs. When a crate gets real code, it gets real tests.
- **No cross-crate `pub use` reexports from `db`.** The facade crate documents its future surface in its doc comment but doesn't import anything yet.

## After this plan lands

Next planning documents in this directory should describe the first real extraction — likely `02-extract-ql.md` when Stage 3 begins and the subscription engine needs the parser from a second call site. Until then, the 14 placeholders sit unmodified alongside `rt`.
