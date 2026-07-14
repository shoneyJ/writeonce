# 13 — Class model + live pricing: state and methods, no inheritance

> **Kanban: 🔄 in progress** — 13a ✅ shipped; 13b ✅ shipped (methods execute over RPC); 13c (LIVE push) is next; 13d ⏸ parked (frontend); 13e ⬜. Board: [00-kanban.md](00-kanban.md)

**Context sources:** [`../runtime/database/02-wo-language.md`](../runtime/database/02-wo-language.md) (schema layer, § Schema-Layer DML brace disambiguation, § Cross-Paradigm Transaction Coordinator), [`../runtime/database/04-client-api.md`](../runtime/database/04-client-api.md) (subscription engine), [`./09-concurrency-scaleout.md`](./09-concurrency-scaleout.md) (thread-per-core scale-out), [`./exploration/ui/00-overview.md`](./exploration/ui/00-overview.md) + [`./exploration/ui/01-htmlx-format-spec.md`](./exploration/ui/01-htmlx-format-spec.md) (live UI), [`../examples/pricing/`](../examples/pricing/) (the demo this phase makes real), [`../examples/ecommerce/shared/logic/checkout.wo`](../examples/ecommerce/shared/logic/checkout.wo) (the existing `fn … in txn snapshot` signature style methods reuse).

## Context

writeonce is declarative by design — `type`, `service`, `policy`, `on <event>` — and the docs explicitly reject OO. But developers arriving from OO languages keep reaching for "a class with methods", and the request has a legitimate core: **behavior that belongs to a row** (`product.set_price(amount)`) is today only expressible as a free `fn` or a trigger. This phase adds the smallest class model that satisfies it:

> **`class` = state + methods. No inheritance, no override, no polymorphic dispatch — ever.** Composition via `ref` / `multi`, exactly like `type`. Go-style encapsulation, not Java-style hierarchies.

The driving workload is the [`pricing` demo](../examples/pricing/): a `Price` class and a `Product` class with methods, products owning prices, a `##ui pricing` screen showing live prices of selected products — RAM-resident data, one product readable by millions of customers at once, price updates pushed live to millions of subscribers, all I/O on kernel primitives.

This doc is a **master plan** in the style of [`09-concurrency-scaleout.md`](./09-concurrency-scaleout.md): sub-phases 13a–13e at a high level, each landing as its own plan doc when implementation starts. No code changes in this pass — the demo project ships as a design artifact alongside this doc.

## Goal

`cargo run --bin wo -- run docs/examples/pricing` serves the demo fully live: `class` declarations parse and store like types, methods execute as row-scoped transactions over RPC, every `set_price` commit pushes a delta to all subscribed clients, the `##ui pricing` screen patches price cells in place, and the read path scales per the phase-09 architecture.

## Design decisions (locked)

1. **`class` is the behavior-bearing sibling of `type`.** Identical field grammar — scalars, defaults, `@unique`/`@check`, embedded docs, `ref`, `multi`, `backlink`, unions, plus the same attachable blocks (`service`, `policy`, `on <event>`). One addition: `fn` methods.
2. **Methods are row-scoped transactional functions.** `fn name(args) -> Ret [in txn [snapshot]]` with an implicit `self` bound to the receiving row. Same signature grammar and execution machinery as the free-standing `fn checkout(...) in txn snapshot` already in the ecommerce sample — a method is a free `fn` with a hidden first parameter. No new transaction semantics.
3. **No inheritance.** No `extends`, no `override`, no virtual dispatch, no abstract classes. This kills the table-per-class/single-table storage mapping problem before it exists: a class IS one table (+ its doc/graph parts), exactly like a type. "Is-a" modelling uses tagged unions (already in the language); "has-a" uses `ref`/`multi`.
4. **`self` stays an identifier in the lexer.** Same gotcha as `subscribe`/`receive`/`me` (CLAUDE.md): it must remain usable as a plain name in expose lists and expressions. The parser binds it positionally inside method bodies.
5. **Storage and REST are class-blind.** `Catalog::from_schemas` treats a class exactly like a type; `service rest` blocks generate the same CRUD routes. Methods add RPC routes on top (13b). A migration from `type` to `class` (or back, if no methods) is a no-op for stored data.
6. **Spec wording is amended in 13a, not before.** `docs/runtime/wo-language.md` ("Isn't OO") and `docs/writeonce-pl.md` ("no class model") change in the same commit that makes the parser accept the syntax, so docs never describe an unparseable language.

## The class surface (normative example)

```wo
@table(name: "prices", index: [product, at])
class Price {
  id:       Id
  product:  ref Product
  amount:   Money                          -- minor units, stdlib scalar
  currency: Text = "EUR"
  at:       Timestamp = now()

  -- Pure method: computes from self, touches nothing else. No txn needed.
  fn discounted(pct: Int) -> Money {
    return self.amount * (100 - pct) / 100;
  }
}


@table
class Product {
  id:     Id
  sku:    SKU @unique
  name:   Text
  prices: multi Price                      -- products have prices (append-only history)

  fn current_price() -> Money in txn {
    return latest(self.prices).amount;
  }

  fn set_price(amount: Money) in txn {
    insert Price { product: self.id, amount: amount };
  }

  service rest "/api/products"
    expose list, get, create, update, delete, subscribe
}
```

**`@table` — storage configuration, never storage declaration** (✅ shipped, with DML support). Every `type`/`class` IS a table (decisions 3/5 stand); the optional type-level `@table(...)` annotation *configures* it: `name: "prices"` sets the storage/table name (catalog-unique; the surface plans 10–12 and the SQL layer consume — WAL records keep the type name as the stable identifier), and each `index: [product, at]` declares a composite secondary index that the engine maintains on every mutation path (CRUD, method-txn undo, WAL replay) and that accelerates `self.prices` relation reads, the schema-layer `select Price{ product == self.id }` expression in method bodies, and `GET /api/prices?product=1` REST filters — all through one `Engine::find_by` path with scan fallback. Bare `@table` is a legal no-op. Unknown keys (`shard_key`, `retention`) are parse errors until their phases land; unknown annotation *names* skip silently. Spec: [`02-wo-language.md § Type-Level Annotations`](../runtime/database/02-wo-language.md).

## Sub-phase sequence

Each lands as its own numbered plan doc (`13a-…`, `13b-…`) when ready. The blog + ecommerce smoke stays green after every sub-phase.

### `13a-class-surface.md` — lexer, parser, AST, spec amendments — ✅ shipped

`class` joins the keyword map (`crates/rt/src/lexer.rs` keyword match, ~line 202 — note `self` stays an ident per decision 4). `parse_type` (`crates/rt/src/parser.rs:120`) takes the leading keyword as a parameter and serves both constructs; `fn` members inside the body parse-and-discard through the existing brace-depth skip — the same mechanism that already swallows `on update … do { … }` triggers. `ast::TypeDecl` gains `is_class: bool`; `Catalog::from_schemas` ignores it (decision 5), so REST CRUD works the moment parsing does. Docs amended in the same change: a "Class Model" subsection in [`02-wo-language.md`](../runtime/database/02-wo-language.md) next to § Schema-Layer DML, the "Isn't OO" paragraph in [`wo-language.md`](../runtime/wo-language.md), the class line in [`writeonce-pl.md`](../writeonce-pl.md), and `just pricing` / `just pricing-demo` recipes.
**Exit (met):** `wo run docs/examples/pricing` parses 2 classes, serves `/api/products` CRUD; parser unit tests (`parses_class_with_methods`, `class_method_braces_do_not_truncate_body`) green; blog/ecommerce/hello unchanged.

### `13b-method-execution.md` — methods over RPC — ✅ shipped

Method bodies compile into a real AST (`ast::{MethodDecl, Stmt, Expr}` — `let`, `insert Type{…}` construction, `return`, `assert … otherwise abort`, `if/else`, arithmetic/comparison/boolean expressions, `self.<relation>` resolution, built-ins `latest`/`count`/`now`) and execute in `crates/rt/src/method.rs` on the shard that owns the receiving row (`run_on(owner_of(id))` — inserts mint locally there, so a method owns everything it writes). Every call runs inside an engine **method transaction** (`Engine::{begin,commit,abort}_txn`): mutations journal undo entries and defer their WAL records; commit emits **one `WalRec::Txn` frame** — the frame's CRC makes a method's mutations replay whole-or-not-at-all, so a crash mid-method can never half-apply — and abort reverts RAM in reverse order. Group-commit acks gate on the batch fsync exactly like CRUD (`Response.gate` / parked replies). Routes: `POST <service-path>/:id/<method>` for every method of a class with a `service rest` block; errors map NoSuchRow→404, BadArgs→400, Abort→409, Exec→500. Lowercase `insert` stays an identifier in the lexer (same rule as `subscribe`/`me`/`self`); only the class-side `fn` arm parses bodies — `fn` inside a plain `type` keeps the 13a skip.
**Exit (met):** `curl -X POST /api/products/1/set_price -d '{"amount": 4999}'` inserts a Price atomically (verified on 2 shards incl. the cross-shard hop, in both WAL modes, and across restart — the Txn frame replays); `current_price` returns it; `set_price {"amount": 0}` trips the sample's assert → 409 and rolls back completely (price unchanged). 12 new unit tests (parser AST, executor, WAL atomicity, route statuses); blog/ecommerce/hello unchanged; `just pricing-demo` runs the whole sequence.

### `13c-live-pricing-push.md` — LIVE deltas on commit

The subscription registry from [`04-client-api.md`](../runtime/database/04-client-api.md): keyed by type + predicate, matched on commit, deltas framed over WebSocket. Replaces the 501 stub at `/api/<type>/live` (`crates/rt/src/server.rs`) with a real upgrade for the pricing demo's needs — `LIVE select Product{ name, prices }` and the `subscribe` expose. This is the Stage 3 milestone scoped to one workload; the full wire protocol stays in Phase 4.
**Exit:** two terminals — `websocat /api/products/live` in one, `set_price` via curl in the other — the delta frame arrives on the open socket within one commit tick, no polling.

### `13d-pricing-ui.md` — the `/pricing` screen, MVC

The screen ships as an **MVC triplet** per [`exploration/ui/08-mvc-structure.md`](./exploration/ui/08-mvc-structure.md), built in the sub-phase sequence of [`14-mvc-ui-implementation.md`](./14-mvc-ui-implementation.md): model = the classes themselves, view = [`pricing.htmlx`](../examples/pricing/ui/pricing/pricing.htmlx) (plain htmlx, logic-free) + external [`pricing.scss`](../examples/pricing/ui/pricing/pricing.scss) (strict SCSS subset compiled at `wo build`, no external deps), controller = [`pricing.wo`](../examples/pricing/ui/pricing/pricing.wo) (`route:`/`view:`/`styles:`, `model:` bindings, `actions:` calling the 13b class methods). SSR per [`exploration/ui/01-htmlx-format-spec.md`](./exploration/ui/01-htmlx-format-spec.md), compiler glue per [`02-ui-compiler.md`](./exploration/ui/02-ui-compiler.md), and the vanilla-JS client runtime ([`03-client-runtime.md`](./exploration/ui/03-client-runtime.md)) patches the price cell when the 13c delta lands. The controller's `model:` block is the M→V binding; the watchlist narrows the subscription predicate server-side.
**Exit:** browser at `/pricing` shows selected products; a `set_price` commit from curl changes the price cell in every open browser without reload.

### `13e-pricing-at-scale.md` — millions of readers, millions of live updates

No new architecture — this sub-phase wires the demo to [`09-concurrency-scaleout.md`](./09-concurrency-scaleout.md) and adds one mechanism:

- **RAM-resident:** the engine is the in-memory design of [`03-inmemory-engine.md`](../runtime/database/03-inmemory-engine.md); disk (phases 10–12) is durability behind it, never the read path.
- **Read fan-out:** thread-per-core, shared-nothing shards behind `SO_REUSEPORT` (09a/09b). A single hot product row is owned by one shard but **read-replicated to every shard**: each thread keeps a read-only copy of hot rows, refreshed by the same per-thread broadcast that 09d uses for subscriber fan-out. Millions of concurrent `GET /api/products/1` spread across all cores and never contend — writes still serialize on the owning shard, preserving the single-writer model.
- **Live fan-out to millions:** one `set_price` commit → one delta message per thread (09d, not per subscriber) → each thread predicate-matches its local subscription table and batches socket writes on its own `io_uring` ring ([`exploration/linux/07-io_uring.md`](./exploration/linux/07-io_uring.md)). Kernel primitives only: epoll today ([`done/02-event-loop-epoll.md`](./done/02-event-loop-epoll.md), edge-triggered + group commit), io_uring per phase 09.

**Exit: verification targets defined and a load harness scripted** (not necessarily met on dev hardware):

| Metric                                      | Target                                         | How measured                                                      |
| ------------------------------------------- | ---------------------------------------------- | ----------------------------------------------------------------- |
| Concurrent readers of one product           | 1 M req/s aggregate on 16 cores                | `wrk -c 10000` against `GET /api/products/1`, hot-row replicas on |
| Live subscribers receiving one price update | 1 M open sockets, delta delivered p99 < 250 ms | `websocat` fan-out harness, timestamped frames                    |
| Commit→first-delta latency                  | p99 < 10 ms                                    | in-process timestamp at commit vs first socket write              |
| Memory                                      | ~2 KB/connection + engine working set          | RSS under subscriber load                                         |
| Dep count                                   | 1 (`libc`)                                     | `crates/rt/Cargo.toml` unchanged by this phase                    |

## Non-scope

- **No inheritance, ever, under this plan.** If hierarchy modelling pressure appears, the answer is tagged unions and composition; a future interfaces/traits proposal would be its own phase with its own doc.
- **No method overloading, no statics, no constructors.** Row creation stays `insert` / REST `create`; one method name per class.
- **No client-side method stubs** — `wo gen sdk` method support belongs to Phase 5 (Go SDK), not here.
- **No multi-node distribution.** Same stance as phase 09: single box, threads-as-shards.

## Cross-references

- [`../examples/pricing/`](../examples/pricing/) — the demo project this plan makes real, file-by-file phase map in its README.
- [`../runtime/database/02-wo-language.md`](../runtime/database/02-wo-language.md) — schema layer the class grammar extends; transaction coordinator methods reuse.
- [`../runtime/database/04-client-api.md`](../runtime/database/04-client-api.md) — subscription engine 13c scopes down.
- [`./09-concurrency-scaleout.md`](./09-concurrency-scaleout.md) — the scale architecture 13e instantiates.
- [`./exploration/ui/00-overview.md`](./exploration/ui/00-overview.md) — UI track 13d draws on.
- [`../examples/hello/main.wo`](../examples/hello/main.wo) — the minimal example whose `Revision`-trigger pattern is the declarative ancestor of methods.
