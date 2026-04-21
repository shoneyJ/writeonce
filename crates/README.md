# `crates/` — the `.wo` runtime

Fifteen crates make up the new runtime. Only `rt/` carries real code today (Stage 2); the other fourteen are **empty placeholders** scaffolded to match the 7-phase design so each phase's extraction work becomes a mechanical code move into an existing home.

> The crate-name prefix `wo-` was dropped when the active project namespaced itself under `wo` (the binary, the file extension, the language). Internal imports read cleanly: `use ql::Parser`, `use db::Tx`, `use http::router`. The v1 codebase keeps its `wo-*` prefix in [`reference/crates/`](../reference/crates/) to distinguish the generations.

## Map

| Phase | Crate | Purpose | Status |
| --- | --- | --- | --- |
| 2 | [`ql`](./ql/)       | `.wo` grammar — lexer, parser, AST | placeholder |
| 2 | [`value`](./value/) | tagged `Value` + dotted-path helpers | placeholder |
| 2 | [`engine`](./engine/) | in-memory executor (rel / doc / graph) + schema catalog | placeholder |
| 2 | [`txn`](./txn/)     | transaction coordinator — MVCC, `RETURNING` alias table | placeholder |
| 2 | [`db`](./db/)       | top-level facade — `open()`, `Tx`, `Query`, `Subscribe` | placeholder |
| 3 | [`wal`](./wal/)     | write-ahead log — io_uring + fsync + recovery | placeholder |
| 4 | [`sub`](./sub/)     | live subscriptions — delta frames on commit | placeholder |
| 4 | [`http`](./http/)   | wire protocol — REST / GraphQL-over-WS / native codec | placeholder |
| 5 | [`gen`](./gen/)     | codegen — `.wo type` → Go / TS / Rust / Python clients | placeholder |
| 6 | [`policy`](./policy/) | RBAC + row-level rules compiled into planner rewrites | placeholder |
| 6 | [`logic`](./logic/) | `on <event>` triggers + `fn ... in txn` interpreter | placeholder |
| 6 | [`service`](./service/) | `service rest/graphql/native` endpoint dispatch | placeholder |
| 6 | [`ui`](./ui/)       | `##ui` screens → SSR HTML + client runtime | placeholder |
| 6 | [`app`](./app/)     | `##app` route manifest + startup hooks | placeholder |
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

- [`../reference/crates/`](../reference/crates/) — the v1 writeonce blog (13 crates, nested workspace). Preserved for reference per [docs/runtime/database/07-wo-seg-migration.md](../docs/runtime/database/07-wo-seg-migration.md). Keeps its `wo-*` prefix.
- [`../prototypes/wo-db/`](../prototypes/wo-db/) — C++ prototype of the query-layer engine (~2k lines). The reference implementation this Rust port follows at the language level.
- [`../docs/plan/`](../docs/plan/) — planning documents for in-flight work (the `.md` files directly under `plan/` are upcoming phases; `plan/done/` holds completed ones). [`plan/done/01-scafolding-crates.md`](../docs/plan/done/01-scafolding-crates.md) is the authoritative scope doc for the 14 new placeholders.
