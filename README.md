# writeonce

A declarative full-stack programming language. You write `.wo` files; the runtime compiles them into a binary that owns the database, serves REST, and pushes live subscriptions — no external database, no external web server, no frontend framework.

Think **Go + Postgres + `net/http` + Phoenix LiveView, folded into one language and one binary.**

## Quickstart

```bash
git clone https://github.com/shoneyJ/writeonce
cd writeonce
cargo run --bin wo -- run docs/examples/blog     # serve the sample blog on :8080
curl http://127.0.0.1:8080/api/articles          # it's a real REST API now
```

See [`reference/rest/blog.rest`](reference/rest/blog.rest) for a preconfigured HTTP-request file that drives the whole sample — open it in VS Code (with the REST Client extension) or JetBrains and click "Send Request" on each block.

## What this repository contains

| Path | What it is |
| --- | --- |
| [`crates/rt/`](crates/rt/) | The new `.wo` language runtime — lexer, type-DSL parser, in-memory engine, axum REST server. Produces the `wo` binary. |
| [`crates/{ql,value,engine,txn,db,wal,sub,http,gen,policy,logic,service,ui,app}/`](crates/) | 14 empty placeholder crates scaffolded for Phases 2–6. Real code extracts from `rt/` as each phase activates. |
| [`docs/runtime/wo-language.md`](docs/runtime/wo-language.md) | **Start here.** The language overview: toolchain, hello-world, stdlib, client model. |
| [`docs/runtime/database.md`](docs/runtime/database.md) | The 7-phase engineering series that drives the runtime's design. |
| [`docs/examples/blog/`](docs/examples/blog/) | Sample `.wo` project: blog with articles, authors, tags, comments. ~200 lines. |
| [`docs/examples/ecommerce/`](docs/examples/ecommerce/) | Sample `.wo` project: storefront + live order-ops table + cross-paradigm checkout. ~300 lines. |
| [`prototypes/wo-db/`](prototypes/wo-db/) | C++ prototype of the query-layer engine (SQL + Cypher + document paths, `RETURNING` aliases, `LIVE` stub). ~2k lines, smoke tests pass. Reference implementation the Rust port follows. |
| [`reference/rest/`](reference/rest/) | `.rest` files (VS Code REST Client / JetBrains HTTP format) for manually testing the running prototype. |
| [`reference/crates/`](reference/crates/) | The v1 writeonce blog — 13 Rust crates implementing the original `.seg` + sidecar-index storage engine and `.htmlx` templating. Preserved as a nested workspace; see [`reference/README.md`](reference/README.md). |

## Current stage

The runtime is under active development. Each stage lands as an independently shippable cut:

| Stage | What works | Status |
| --- | --- | --- |
| **1** | `wo run <dir>` discovers every `.wo` file under a directory | ✅ shipped |
| **2** | Type-DSL parser, in-memory engine, REST CRUD (`list` / `get` / `create` / `update` / `delete`) generated from `service rest` blocks, JSON bodies with auto-id, default-value seeding, partial-update PATCH | ✅ shipped — `cargo run -- run docs/examples/blog` |
| **3** | LIVE subscriptions over WebSocket, delta frames on commit, `me` / session layer | pending |
| **4+** | Transactional fns (`fn checkout in txn snapshot`), row-level policies, type-attached triggers, `##ui` SSR, WAL durability, codegen | see [docs/runtime/database.md](docs/runtime/database.md) |

`cargo test --lib` at the root runs 14 unit tests covering the lexer, parser, compiler, and engine. Stage-3 endpoints respond `501 Not Implemented` until they land.

## Build & test

```bash
cargo build                                         # builds all 15 crates (only `rt` has real code)
cargo test --lib                                    # 14 unit tests

cargo run --bin wo -- run docs/examples/blog       # serve the blog sample
cargo run --bin wo -- run docs/examples/ecommerce  # serve the ecommerce sample

# Override the listen address
WO_LISTEN=127.0.0.1:9000 cargo run --bin wo -- run docs/examples/blog
```

## The v1 codebase (reference)

The original writeonce blog engine — 13 crates, flat-file `.seg` storage, sidecar indexes, `.htmlx` templates, hand-rolled `epoll` event loop — moved to [`reference/crates/`](reference/crates/) when the new runtime was scaffolded. It's a nested Cargo workspace:

```bash
cd reference/crates
cargo build                   # all 13 v1 crates still compile
cargo test                    # 12 unit tests, 1 ignored integration test
```

V1 crates keep the `wo-` prefix (`wo-seg`, `wo-store`, …). The new runtime crates dropped it (`ql`, `value`, `engine`, …). [`docs/runtime/database/07-wo-seg-migration.md`](docs/runtime/database/07-wo-seg-migration.md) is the phased coexistence plan for replacing v1 with the new runtime — abstract behind a trait, dual-write, cut over, decommission.

## License & status

Work in progress. Nothing here is stable. Read the language overview in [`docs/runtime/wo-language.md`](docs/runtime/wo-language.md) if you want to know the shape; read the phase docs if you want to see the engineering plan; look in [`docs/examples/`](docs/examples/) if you want to see what the end product feels like.
