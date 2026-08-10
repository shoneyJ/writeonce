# `crates/` — the `.wo` runtime

Fifteen crates make up the new runtime. Only `rt/` carries real code today (Stage 2); the other fourteen are **planned** per the 7-phase design — scaffolds were deleted 2026-08-08; recreate each crate when its phase activates, so extraction from `rt` stays a mechanical move.

> The crate-name prefix `wo-` was dropped when the active project namespaced itself under `wo` (the binary, the file extension, the language). Internal imports read cleanly: `use ql::Parser`, `use db::Tx`, `use http::router`. The v1 codebase keeps its `wo-*` prefix in [`.dev/reference/crates/`](../.dev/reference/crates/) to distinguish the generations.

## Map

| Phase | Crate | Purpose | Status |
| --- | --- | --- | --- |
| 2 | `ql`       | `.wo` grammar — lexer, parser, AST | planned |
| 2 | `value` | tagged `Value` + dotted-path helpers | planned |
| 2 | `engine` | in-memory executor (rel / doc / graph) + schema catalog | planned |
| 2 | `txn`     | transaction coordinator — MVCC, `RETURNING` alias table | planned |
| 2 | `db`       | top-level facade — `open()`, `Tx`, `Query`, `Subscribe` | planned |
| 3 | `wal`     | write-ahead log — io_uring + fsync + recovery | planned |
| 4 | `sub`     | live subscriptions — delta frames on commit | planned |
| 4 | `http`   | wire protocol — REST / GraphQL-over-WS / native codec | planned |
| 5 | `gen`     | codegen — `.wo type` → Go / TS / Rust / Python clients | planned |
| 6 | `policy` | RBAC + row-level rules compiled into planner rewrites | planned |
| 6 | `logic` | `on <event>` triggers + `fn ... in txn` interpreter | planned |
| 6 | `service` | `service rest/graphql/native` endpoint dispatch | planned |
| 6 | `ui`       | `##ui` screens → SSR HTML + client runtime | planned |
| 6 | `app`     | `##app` route manifest + startup hooks | planned |
| — | [`rt`](./rt/)       | **active** — Stage-2 monolith + the `wo` binary | **shipped** |

## Why `rt/` is monolithic right now

`rt/` currently holds every module the runtime needs — lexer, parser, AST, in-memory engine, axum REST server — because **shipping working Stage 2 was more important than hitting the final crate layout on day one**. Each module inside `rt/src/` is written with a target home in mind:

| `rt` module | Moves to | Phase |
| --- | --- | --- |
| `token.rs` + `lexer.rs` + `ast.rs` + `parser.rs` | `ql/` | 2 |
| `engine.rs` (Value + Row helpers) | `value/` | 2 |
| `engine.rs` (Engine + Catalog) | `engine/` | 2 |
| `compile.rs` | `engine/` | 2 |
| `server.rs` | `http/` + `service/` | 4 / 6 |
| `bin/wo.rs` | stays in `rt/` (the binary) | — |

Extractions happen phase-by-phase — first one lands when a second caller appears (likely when Stage 3 needs the parser for raw-`.wo` HTTP requests).

## Build & test

```bash
cargo build                                   # compiles all 15 crates
cargo test --lib                              # 14 unit tests (all in rt today)
cargo run --bin wo -- run docs/examples/blog  # serve the blog sample
```

## What's outside this directory

- [`../.dev/reference/crates/`](../.dev/reference/crates/) — the v1 writeonce blog (13 crates, nested workspace). Preserved for reference per [docs/runtime/database/07-wo-seg-migration.md](../docs/runtime/database/07-wo-seg-migration.md). Keeps its `wo-*` prefix.
- [`../prototypes/wo-db/`](../prototypes/wo-db/) — C++ prototype of the query-layer engine (~2k lines). The reference implementation this Rust port follows at the language level.
- [`../docs/plan/`](../docs/plan/) — planning documents for in-flight work (the `.md` files directly under `plan/` are upcoming phases; `plan/done/` holds completed ones). [`plan/done/01-scafolding-crates.md`](../docs/plan/done/01-scafolding-crates.md) is the authoritative scope doc for the 14 new placeholders.
