# Phase 7 — Replacing `wo-seg` with the writeonce Database

> A phased coexistence plan: abstract the article store behind a trait, stand up the `.wo` engine as a second implementation, dual-run, cut over, decommission.

**Previous**: [Phase 6 — Low-Code Full-Stack](./06-lowcode-fullstack.md) | **Index**: [database.md](../database.md)

---

## Context

Today's writeonce runtime stores articles in a hand-rolled append-only file format:

- **`crates/wo-seg`** (~475 LOC) — `.seg` binary file: magic + header + `[u32 length][u8 flags][payload]` records serialized with `bincode`. `SegWriter::append()` returns a byte offset usable as an index pointer. Tombstoning flips a flag byte. No transactions, no MVCC, no concurrent writers.
- **`crates/wo-index`** — sidecar `title.idx`, `date.idx`, `tags.idx` files built from the `.seg`. Queries hit the index to resolve to a byte offset, then the `.seg` to load the record.
- **`crates/wo-store`** — composes the two above, owns cold-start (rebuild from `content/`), and exposes the query API the rest of the system calls: `get_by_title`, `list_published`, `list_by_tag`, `list_by_date_range`, `count_published`, `ingest_article`, `article_version`.

This is the Phase 1 answer ("no external DB — add a petgraph-backed `mappings.idx`") made flesh. It is correct for a single-writer blog and wrong for everything Phases 2–6 want to deliver: no ACID across multiple shapes, no live subscriptions, no cross-paradigm queries, no declarative schema, no codegen.

The six-phase `.wo` design is the replacement. This doc plans the migration — how to swap wo-seg for the `.wo` engine **without halting writeonce** while the engine is built over multiple quarters.

## Intended Outcome

- `crates/wo-seg` is deleted.
- `crates/wo-store` either (a) becomes a thin facade over the `.wo` engine or (b) disappears, with callers depending directly on the engine's Rust SDK.
- Writeonce's serving path is unchanged from the user's perspective throughout the migration.
- The `.wo` engine reaches production-ready status incrementally; each milestone is independently shippable.

## Strategy — Phased Coexistence

Do **not** big-bang. The seg-based store works; replacing it takes many months. Instead:

1. **Abstract** the existing store behind a Rust trait — one weekend of mechanical refactor, zero behavior change.
2. **Build** the `.wo` engine crates next to seg, not in its place. Port the C++ prototype (`prototypes/wo-db/`) to Rust so the engine lives in the same Cargo workspace as the blog.
3. **Dual-run** — writes go to both backends, reads to seg. Compare results in CI and on production data. This surfaces engine bugs without user impact.
4. **Cut over** reads once the engine passes dual-run. Writes still hit seg as a cold standby.
5. **Decommission** seg when enough time has passed without rollback and a restore-from-seg fallback is no longer load-bearing.

```
wo-seg + wo-store  ─────────► trait-abstracted  ─────► dual-write, read seg  ─────► read wo-db, write both  ─────► wo-db only, delete wo-seg
     (today)                   (phase A)               (phase B)                      (phase C)                    (phase D)
```

Each transition is reversible — flip one feature flag or swap one trait object back.

## Proposed Crate Layout

Port the C++ prototype (`prototypes/wo-db/src/*`) to Rust, split along the natural seams. New-runtime crates are unprefixed; v1 crates keep `wo-` in `reference/crates/`.

| Crate | Purpose | Prototype source | Phase |
| --- | --- | --- | --- |
| `ql`      | `.wo` grammar: lexer, parser, AST | `src/lexer.*`, `src/parser.*`, `src/ast.hpp` | 2 |
| `value`   | tagged `Value` + path utilities                | `src/value.hpp`, path helpers in `src/storage.cpp` | 2 |
| `engine`  | in-memory executor (sql / doc / graph), schema catalog | `src/storage.*`, `src/executor.*` | 2 |
| `txn`     | MVCC, snapshot isolation, `RETURNING` alias table | new (Phase 2 milestone 3) | 2 |
| `wal`     | write-ahead log + fsync + crash recovery        | new ([Phase 3](./03-inmemory-engine.md)) | 3 |
| `sub`     | live subscriptions — delta frames on commit | new ([Phase 4](./04-client-api.md)) | 4 |
| `http`    | wire protocol — REST / GraphQL-over-WS / native codec | new ([Phase 4](./04-client-api.md)) | 4 |
| `db`      | top-level facade: `open()`, `Tx`, `Query`, `Subscribe` — the Rust SDK | integrates the above | 2–4 |
| `gen`     | codegen: `.wo type` → Rust structs, Go structs, TypeScript | `sa-gen`/`wo-gen` in [Phase 5](./05-go-sdk.md) | 5 |

All 15 crates (these 14 plus the existing `rt` binary crate) now exist as empty skeletons in `crates/`. See [`crates/README.md`](../../../crates/README.md) and [`docs/plan/done/01-scafolding-crates.md`](../../plan/done/01-scafolding-crates.md) for the scaffolding plan that landed them.

Today's `reference/crates/wo-seg` and `reference/crates/wo-index` remain in the v1 nested workspace for the entire migration window. They disappear only at the end of Phase D.

`reference/crates/wo-store` evolves but survives — it becomes the writeonce-specific glue layer (trait, article domain model, content-directory cold-start) whose backend is swappable.

## Phase A — Abstract the Article Store

**Goal**: every caller depends on a trait, not on `wo_store::Store` directly. Zero behavior change.

**Work**:

- Define `trait ArticleStore` in `wo-store/src/lib.rs` with the existing public API:
  ```rust
  pub trait ArticleStore: Send + Sync {
      fn get_by_title(&self, sys_title: &str) -> io::Result<Option<Article>>;
      fn list_published(&self, skip: usize, limit: usize) -> io::Result<Vec<Article>>;
      fn list_by_tag(&self, tag: &str) -> io::Result<Vec<Article>>;
      fn list_by_date_range(&self, start: i64, end: i64) -> io::Result<Vec<Article>>;
      fn count_published(&self) -> io::Result<usize>;
      fn ingest_article(&mut self, json_path: &Path) -> io::Result<String>;
      fn article_version(&self, sys_title: &str) -> Option<u64>;
      fn content_dir(&self) -> &Path;
  }
  ```
- Rename the existing `Store` struct to `SegStore` and implement `ArticleStore` for it. Re-export `SegStore as Store` for one release to avoid churn at call sites.
- Change `wo-route`, `wo-serve`, `wo-sub`, `wo-htmlx` to take `&dyn ArticleStore` (or generic `<S: ArticleStore>`). The trait import stays in `wo-store`; concrete impls move to sibling crates.
- Add a tiny `wo-store::open(content_dir, data_dir) -> Arc<dyn ArticleStore>` factory that picks the backend based on a config env var (`WO_STORE_BACKEND=seg|db|dual`).

**Exit criteria**: `cargo test` passes; `wo serve` boots unchanged; git log shows one PR.

## Phase B — Stand Up `wo-db` in Rust

**Goal**: a Rust `wo-db` crate that speaks the full `.wo` grammar from the prototype, stored in memory, with an `ArticleStore` impl mapping writeonce's `Article` onto the relational paradigm.

**Work**:

- Port `prototypes/wo-db/` (C++) to Rust crates per the layout table above. The `wo` namespace becomes the `wo_*` crate family; the test suites (`tests/smoke.wo`, `tests/checkout.wo`) run as Rust integration tests.
- Define a `.wo` schema for the writeonce domain (in a new file, `crates/wo-store/schema.wo`):
  ```wo
  type Article {
    id:           Id
    sys_title:    Slug @unique
    title:        Text
    published:    Bool = false
    published_at: Timestamp?
    author:       Text
    tags:         [Text]
    meta:         { excerpt: Text, body_md: Markdown }
  }
  ```
- Add `DbStore` — a second `ArticleStore` impl that translates calls into `.wo` queries:
  - `get_by_title(t)` → `SELECT * FROM Article WHERE sys_title = $t` (one row)
  - `list_published(skip, limit)` → `SELECT * FROM Article WHERE published = true ORDER BY published_at DESC LIMIT $limit OFFSET $skip`
  - `list_by_tag(t)` → `SELECT * FROM Article WHERE $t IN tags`
  - `list_by_date_range(a, b)` → `SELECT * FROM Article WHERE published_at BETWEEN $a AND $b`
  - `count_published` → `SELECT COUNT(*) FROM Article WHERE published = true`
  - `ingest_article(path)` → load JSON → `INSERT INTO Article (…)`
- Cold-start path: when the data dir is empty, `DbStore::open` loads all `content/*.json` the same way `SegStore::open` does today and inserts into the engine.
- Gate behind `#[cfg(feature = "db-backend")]` so seg-only builds keep working until Phase C.

**Exit criteria**: `DbStore` passes the same unit tests as `SegStore` (rename `Store` → `ArticleStore` in test assertions). Memory footprint and per-query latency measured against seg; both within an order of magnitude.

## Phase C — Dual-Write, Read Seg

**Goal**: every mutation hits both backends; reads stay on seg; a differ flags mismatches.

**Work**:

- Add `DualStore` — a third `ArticleStore` impl that forwards writes to both `SegStore` and `DbStore` and returns `SegStore` results for reads.
- Add a background task (`wo-store::differ`) that on every ingest runs every query method against both backends and compares results. Mismatches → structured log entry (`store_mismatch` event) + a Prometheus counter.
- Set `WO_STORE_BACKEND=dual` on staging for two weeks, then on prod behind a rollout flag.

**Exit criteria**: zero `store_mismatch` events for 14 consecutive days on production traffic.

## Phase D — Cut Over Reads, Keep Seg as Fallback

**Goal**: reads served from `DbStore`; seg still receives writes and is kept queryable as a cold standby.

**Work**:

- Invert `DualStore`: writes to both, reads from `DbStore`.
- Add an admin command `wo db verify --against seg` that re-runs the differ on demand (for post-incident checks).
- After a stable month, remove `DualStore` entirely. `WO_STORE_BACKEND=db` becomes the only supported value.

**Exit criteria**: one month with no read-path regressions; no active rollback capability needed for routine ops.

## Phase E — Decommission `wo-seg`

**Goal**: delete `crates/wo-seg`, shrink `crates/wo-store` to the trait + content-directory cold-start.

**Work**:

- Delete `crates/wo-seg`. Remove `wo-seg` from `Cargo.toml` workspace members and from `wo-store/Cargo.toml` deps.
- Delete `SegStore` from `wo-store`. The trait `ArticleStore` and `DbStore` remain.
- Delete the `.seg` file from production data directories (via a migration: verify `DbStore` has every record, then `rm`).
- Delete `crates/wo-index` **if and only if** `DbStore` has replaced its indexes with the engine's internal ones. If the LSM/graph indexes inside `wo-db` cover the three sidecar indexes (title, date, tags) — expected — then wo-index goes too. If any index is still load-bearing outside the engine, keep it.

**Exit criteria**: CI is green with the deletions; production runs a release cycle without rollback; `rg "wo-seg\|wo_seg"` returns zero hits.

## Integration Touchpoints

These crates reference the store today and will need light updates for Phase A (trait swap):

| Crate | Current coupling | Change |
| --- | --- | --- |
| `wo-store` | owns `Store`, depends on `wo-seg` + `wo-index` | gains trait + factory + dual-write impl (A–C); shrinks to facade in E |
| `wo-route` | likely consumes `&Store` | accept `&dyn ArticleStore` |
| `wo-serve` | HTTP handlers read the store | accept `Arc<dyn ArticleStore>` |
| `wo-sub`   | subscription layer | later — see below |
| `wo-htmlx` | may read article state during render | accept trait or projection |
| `wo-watch` | inotify-driven ingest | unchanged; still calls `ingest_article` |
| `wo-rt`    | runtime glue | pass the trait object through |

`wo-sub` is a special case. Today it likely polls or reacts to `article_version` monotonic counters. When Phase 4 activates `LIVE` queries inside `wo-db`, `wo-sub` should stop doing its own diffing and become a pass-through for engine-emitted deltas. That transition happens in Phase C/D, not Phase A — it's not required for the trait refactor.

## Risks

1. **Cold-start cost.** `SegStore` builds its indexes in one pass over `.seg`. `DbStore` has to parse JSON from `content/` the same way but also commit through the engine's WAL. If this is slow, add a `wo db import --from-seg <path>` shortcut that bulk-loads from an existing `.seg` without going through the ingest path.
2. **Memory footprint.** Today's seg-based path `mmap`s the file; the `.wo` engine is RAM-primary. For a blog with hundreds of articles, immaterial; for a larger dataset, Phase 3's SSD-backed variant is what's needed.
3. **Article → `.wo` type drift.** `wo_model::Article` is the canonical domain type today. The `.wo` schema mirrors it, but if the two diverge (a new field is added to `Article` but not to the schema), queries silently drop that field. Mitigation: `wo-gen` should include a `--verify wo_model::Article` mode in Phase 5 that fails CI on drift.
4. **Dual-write contention.** If ingest becomes the bottleneck during Phase C, time-box dual-write: drop it after 14 clean days rather than running it indefinitely.
5. **Feature flag sprawl.** `WO_STORE_BACKEND` should be the only config knob. Resist per-method flags.

## What's Out of Scope for This Doc

- The engine internals themselves — those live in Phases 2–4.
- The Phase 5 SDK (`wo-gen`, typed Go client) — wo-store callers are Rust, and Rust codegen is part of `wo-gen` but not a blocker.
- `##ui` / `##policy` / `##logic` / `##service` — those are Phase 6 and assume the engine is already running.
- Any graph-first features (mappings, `RELATED_TO` traversal) — they become trivially available once `DbStore` is live, but don't need to gate the seg → db cutover.

## Verification

Each phase has its own exit criteria above. End-to-end verification for the whole migration:

1. **Parity** — After Phase B: a shadow script replays one week of production ingest through `DbStore` in a sandbox; every query from the shadow matches seg.
2. **Latency** — After Phase D: p50/p95/p99 of `get_by_title`, `list_published`, `list_by_tag` are at or below the seg baseline. Measured by the existing request-timing middleware, not synthetic benchmarks.
3. **Crash safety** — After Phase 3 WAL ships: `kill -9` during write, reopen, confirm the committed state matches and uncommitted writes are gone. Automated test.
4. **Decommission audit** — After Phase E: `rg 'wo-seg|wo_seg|\.seg\b' crates/` returns zero; `cargo deny check` passes; production restart ingests from `content/` with no `.seg` file present.

## Related Documents

- [02-wo-language.md](./02-wo-language.md) — the two-layer `.wo` language the engine speaks
- [03-inmemory-engine.md](./03-inmemory-engine.md) — the storage engine behind `wo-db`
- [04-client-api.md](./04-client-api.md) — wire protocol and `LIVE` subscriptions
- [05-go-sdk.md](./05-go-sdk.md) — the Go SDK built from `.wo` types via `wo-gen`
- [01-evaluation.md](./01-evaluation.md) — why writeonce built `wo-seg` in the first place, and why that choice still looks right for the blog even as the platform grows past it
- [../05-datalayer.md](../05-datalayer.md) — current `.seg` + `.idx` implementation details
- `prototypes/wo-db/` — the C++ prototype of the `.wo` engine, the reference implementation the Rust port follows
