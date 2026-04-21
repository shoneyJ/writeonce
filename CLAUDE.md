# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repo is

writeonce is a **declarative full-stack programming language**. You write `.wo` files; the runtime compiles them into a single binary that owns the database, serves REST, and (Stage 3+) pushes live subscriptions. Think: Go + Postgres + `net/http` + Phoenix LiveView folded into one language.

## The end goal — zero external deps, kernel primitives only

The runtime's north star — documented in [`docs/01-problem.md`](docs/01-problem.md), [`docs/02-recovery.md`](docs/02-recovery.md), and [`docs/plan/linux/00-linux.md`](docs/plan/linux/00-linux.md) — is **one binary, no external Rust crates, all I/O driven directly by Linux kernel primitives**. `epoll` (or `io_uring`), `inotify`, `eventfd`, `timerfd`, `signalfd`, `sendfile`, `mmap` — the kernel IS the subscription engine, the async runtime, and the file watcher.

Target end state of `crates/rt/Cargo.toml`:

```toml
[dependencies]
libc = "0.2"   # the unavoidable FFI bridge to syscalls
```

Stage 2 today carries six transitional deps (`anyhow`, `serde`, `serde_json`, `tokio`, `axum`, `tower`). [`docs/plan/02`](docs/plan/02-event-loop-epoll.md) through [`docs/plan/08`](docs/plan/08-sendfile-static-assets.md) sequence the removal of each one, replaced by hand-rolled modules ported from the v1 crates that already did exactly this (`reference/crates/wo-event`, `wo-http`, `wo-route`, `wo-serve`, `wo-watch`). **When working on the runtime, default to direct-syscall solutions over reaching for new crates** — the `docs/plan/` docs name the port source for every module.

## Layout

The repo holds three cuts of the same project plus one research reference:

1. **`crates/rt/`** — the **active Rust runtime** (Stage 2 shipped). Monolithic on purpose for now: lexer, parser, AST, in-memory engine, axum REST server all in one crate. The `wo` binary lives at `crates/rt/src/bin/wo.rs`.
2. **`crates/{ql,value,engine,txn,db,wal,sub,http,gen,policy,logic,service,ui,app}/`** — 14 **empty sibling crates** scaffolded to match the 7-phase design. Each has a `Cargo.toml` + `src/lib.rs` with just a doc comment pointing at its phase spec. Real code moves in from `rt` as each phase activates; do NOT refactor `rt` to use these today — it would break Stage 2.
3. **`reference/crates/`** — the **v1 writeonce blog** (13 crates: `wo-seg`, `wo-index`, `wo-store`, `wo-htmlx`, etc.). This is a **nested Cargo workspace**, deliberately excluded from the root workspace. The v1 crates keep their `wo-` prefix; the new runtime crates dropped theirs. `cd reference/crates && cargo build` builds v1 standalone. See `docs/runtime/database/07-wo-seg-migration.md` for the plan replacing v1 with the new runtime.
4. **`reference/linux/`** — **symlink to the Linux kernel source tree** (`/home/shoney/projects/linux`). Not committed (see `.gitignore`). Research resource for the kernel-primitives work: read `io_uring/`, `fs/notify/inotify/`, `kernel/eventfd.c`, `include/uapi/linux/*.h` when designing the runtime's kernel-facing modules. Each contributor sets their own target via `ln -s <path-to-linux-src> reference/linux`.

There is also **`prototypes/wo-db/`** — a ~2k-line **C++ prototype** of the query-layer engine (SQL + Cypher + document paths, `RETURNING` aliases, `LIVE` stub). It keeps its `wo-db` directory name (C++ project, separate from the Rust crate `db`). It's the reference implementation the Rust port follows; `make test` still passes.

## Commands

```bash
# Build + test the runtime
cargo build                                              # compiles all 15 crates
cargo test --lib                                         # 14 unit tests (all in rt today)
cargo test --lib parses_inline_struct -- --nocapture     # single named test

# Run the runtime against a sample project
cargo run --bin wo -- run docs/examples/blog       # :8080 — blog sample
cargo run --bin wo -- run docs/examples/ecommerce  # :8080 — ecommerce sample
WO_LISTEN=127.0.0.1:9000 cargo run --bin wo -- run docs/examples/blog   # override port

# v1 blog codebase (nested workspace — must cd first)
cd reference/crates && cargo build && cargo test

# C++ prototype of the query-layer engine
cd prototypes/wo-db && make test     # smoke.wo + checkout.wo
cd prototypes/wo-db && make run      # interactive REPL

# Manual HTTP smoke against a running `wo run ...`
# Open reference/rest/blog.rest or ecommerce.rest in VS Code (with REST Client)
# or JetBrains (built-in HTTP client). Or run curl per reference/rest/README.md.
```

## Architecture — what requires reading multiple files to understand

### Naming convention

- **New runtime crates are unprefixed.** `ql`, `value`, `engine`, `txn`, `db`, `wal`, `sub`, `http`, `gen`, `policy`, `logic`, `service`, `ui`, `app`, `rt`. Internal imports read cleanly: `use ql::Parser`, `use db::Tx`, `use http::router`.
- **V1 crates keep the `wo-` prefix.** `wo-seg`, `wo-index`, `wo-store`, `wo-htmlx`, `wo-md`, and v1's own `wo-rt`/`wo-http`/`wo-sub`. These live in `reference/crates/`.
- **The C++ prototype directory is `prototypes/wo-db/`** — unchanged, not a Rust crate.
- **The binary is `wo`** — defined in `crates/rt/Cargo.toml` `[[bin]]`. Independent of the crate name.

### The two-layer `.wo` language

Covered in `docs/runtime/database/02-wo-language.md`:

- **Schema layer** — unified `type Name { ... }` DSL (fields, embedded structs, `ref`, `multi @edge`, `multi via`, `backlink`, tagged unions, `policy`, `on <event>`, `service`). This is what developers write day-to-day.
- **Query layer** — hybrid SQL + Cypher with five "fixed-glue" rules that make the three grammars share semantics: `$name` parameters everywhere, cross-paradigm `RETURNING col AS alias` visible to later statements in the same `BEGIN … COMMIT`, dotted paths identical in SQL/doc/Cypher, one transaction block syntax, one `LIVE` prefix on subscriptions.

Both layers are `.wo` files. The schema layer compiles down to query-layer operations — but only when Phase 5 codegen and Phase 6 full-stack blocks need a single authoritative input. Stage 2 ships with the schema layer only.

### Single-threaded event loop

Covered in `docs/runtime/database/02-wo-language.md § Concurrency Model` and `03-inmemory-engine.md`. The runtime is Redis/TigerBeetle-style: **one userland thread owns everything** — connection accept, parser, engine, subscription registry. The only non-userland thread is the kernel-owned io_uring SQPOLL helper. This is pinned architecturally — group commit still applies (loop drains many commits into one fsync SQE per tick), and scaling past one core is done by **sharding** independent engine processes, not by adding worker threads. Keep this in mind before proposing `Arc<Mutex<...>>` anything beyond what's already there.

### What's in `rt` today vs. what the empty crates promise

`rt`'s modules deliberately mirror the future crate names so the extraction is mechanical when each phase activates:

| `rt` module | Will move to | Phase |
| --- | --- | --- |
| `token.rs` + `lexer.rs` + `ast.rs` + `parser.rs` | `ql` | 2 |
| `engine.rs` (Value + Row helpers) | `value` | 2 |
| `engine.rs` (Engine + Catalog) | `engine` | 2 |
| `compile.rs` | `engine` | 2 |
| `server.rs` | `http` + `service` | 4 / 6 |
| `bin/wo.rs` | stays in `rt` (the binary) | — |

The `sub`, `wal`, `txn`, `policy`, `logic`, `ui`, `app`, `gen` crates have no `rt` counterpart yet — they land when their phase activates.

### Sample projects drive the grammar

`docs/examples/blog/` and `docs/examples/ecommerce/` are **both docs artifacts and the de facto integration tests**. The parser survives these because specific features in them (nested `{...}` object literals inside trigger actions, `count(...)` / `words(...)` computed defaults, unions like `Pending | Paid | Shipped`) forced real fixes. When changing the parser, run the full end-to-end against both samples, not just `cargo test`.

The ecommerce sample in particular uses features that are deliberately **parse-and-discard** in Stage 2: `fn checkout(...) in txn snapshot`, type-attached `on update` triggers with multi-line `do` actions, `policy read for role ...`. These are part of the `.wo` language but Stage 2 does not execute them.

### The migration story

`docs/runtime/database/07-wo-seg-migration.md` specifies **phased coexistence**: abstract the v1 article store behind an `ArticleStore` trait, stand up the `.wo` engine as a second implementation, dual-write, cut over, decommission v1. Phase A (trait abstraction) hasn't started — the plan is on paper, the v1 code is still monolithic in `reference/crates/wo-store/`. Do not remove anything from `reference/crates/` without checking the migration doc.

### Stage progress

| Stage | Status |
| --- | --- |
| 1 — `wo run <dir>` discovers `.wo` files | ✅ shipped |
| 2 — parser + engine + REST CRUD | ✅ shipped (`cargo run -- run docs/examples/blog`) |
| 3 — LIVE subscriptions over WebSocket | pending — `/api/<type>/live` returns 501 as a stub |
| 4+ — transactional `fn`, policies, triggers, `##ui`, WAL, codegen | design-only (see `docs/runtime/database.md`) |

Stage-3 stubs (501) and policy-shaped 405/404 responses are **intentional and documented** in `reference/rest/*.rest`. Don't "fix" them without checking those files first.

## Non-obvious gotchas

- **`rt` is monolithic on purpose.** Splitting it into the 14 sibling crates is Phase-by-Phase work, not a Stage-2 refactor.
- **`reference/crates/` is its own workspace.** Running `cargo build` at the root does not build v1. Running it in `reference/crates/` does.
- **Parser identifiers vs. keywords.** `subscribe`, `receive`, `expect_abort`, `me` are NOT keywords in the lexer — they stay as plain idents so they can appear as operation names in `expose` lists. Adding them to the keyword map breaks `service rest "..." expose subscribe`.
- **Parser skip-on-block.** Unknown triggers (`on update do ...`) are parsed-and-discarded by brace-depth-aware skipping. Object literals like `{ article_id: self.id }` inside trigger actions contain `}` that must not be mistaken for the type's outer close brace — the depth counter exists specifically because of this.
- **Newline significance.** The lexer emits `Kind::Newline` tokens and the parser uses them to end policy/trigger lines. Do not filter newlines globally.
- **Default-value parsing.** `= now()` is recognised explicitly as `DefaultExpr::Now`; anything else falls into an opaque-expression path that `engine::eval_default` then **omits from created rows** (computed fields display as empty, not as debug-printed tokens).
- **Binary variable shadowing.** `crates/rt/src/bin/wo.rs` has `let rt = ...` (a tokio runtime handle) inside `run()` that shadows the crate named `rt`. Inside `run()` the variable wins; inside `serve()` (a different function) `rt::` refers to the crate. Don't rename the variable without also auditing the crate-path references.

## Where to read next

- `docs/runtime/wo-language.md` — user-facing language overview
- `docs/runtime/database.md` — 7-phase engineering series index
- `docs/plan/linux/00-linux.md` — catalogue of kernel primitives the runtime leans on
- `docs/plan/02-event-loop-epoll.md` through `08-sendfile-static-assets.md` — the dependency-removal phase sequence
- `docs/plan/done/01-scafolding-crates.md` — the completed crate-scaffolding phase
- `docs/examples/blog/README.md` — the canonical worked example
- `prototypes/wo-db/README.md` — the C++ prototype that shows the query layer
- `reference/rest/README.md` — how to exercise the running prototype
- `reference/README.md` — what's in the v1 archive and why it's preserved
- `reference/linux/` (symlink) — the Linux kernel source tree itself; grep `io_uring/`, `fs/notify/inotify/`, `include/uapi/linux/*.h` when designing kernel-facing modules
- `crates/README.md` — inventory of all 15 crates with phase assignments
