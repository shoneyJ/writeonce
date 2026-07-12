# `pricing` — class model + live pricing demo

A `Price` class and a `Product` class with methods — products have prices — and a `/pricing` screen that shows **live price data for selected products**. Data stays in RAM; one product is readable by millions of customers at once; a price update pushes live to millions of subscribers; all concurrency is Linux kernel I/O (epoll today, io_uring per the scale-out plan).

> **Status: 13a + 13b shipped.** `class` declarations parse, the demo serves real REST CRUD, **and methods execute over RPC** — `wo run docs/examples/pricing` (or `just pricing-demo`) serves `POST /api/products/:id/set_price` and `:id/current_price` as row-scoped transactions: the whole body commits as one WAL frame, an `assert … otherwise abort` rolls everything back (409). `subscribe` is a 501 stub until 13c; the UI is design-only until 13d/plan 14. The master plan is [`docs/plan/13-class-model-live-pricing.md`](../../plan/13-class-model-live-pricing.md).

## The class model in one paragraph

`class` = **state + methods, no inheritance**. Fields, defaults, `ref`/`multi`, `service`, `policy`, `on <event>` — all exactly as in `type` — plus `fn` methods with an implicit `self` receiver that run as row-scoped transactions (`in txn [snapshot]`, the same machinery as the ecommerce sample's free-standing `fn checkout`). No `extends`, no override, no virtual dispatch: "is-a" is a tagged union, "has-a" is a `ref`/`multi` edge. Go-style encapsulation, not Java-style hierarchies.

## Layout

The UI follows **MVC** ([`docs/plan/exploration/ui/08-mvc-structure.md`](../../plan/exploration/ui/08-mvc-structure.md)), with the same screen anatomy as the v1 Angular app (`reference/writeonce-app/src/app/article/`) collapsed into the single binary: the **model** is the class itself, the **view** is plain `.htmlx` with external `.scss`, and the **controller** is a `.wo` file that binds the model into the view and is the only place UI may call class methods.

```
pricing/
├── wo.toml                   # app manifest
├── main.wo                   # entry point: insert product, LIVE subscribe, set_price
├── types/                    # MODEL — classes: schema + methods
│   ├── price.wo              #   class Price  — amount/currency/at + fn discounted(pct)
│   └── product.wo            #   class Product — prices: multi Price + fn current_price /
│                             #     fn set_price + service rest /api/products
└── ui/
    └── pricing/              # one screen = one MVC triplet
        ├── pricing.wo        #   CONTROLLER — route, model: bindings, actions → class methods
        ├── pricing.htmlx     #   VIEW — plain htmlx (Mustache + wo:live/wo:bind), logic-free
        └── pricing.scss      #   VIEW styles — external SCSS, compiled at `wo build`
```

## What runs when

| File / feature | Goes live in | Plan |
| --- | --- | --- |
| `class` parses; `/api/products` CRUD serves | **13a ✅ shipped** | lexer/parser/AST + spec amendments |
| `set_price` / `current_price` over RPC (`POST /api/products/:id/set_price`) | **13b ✅ shipped** | method execution, row-scoped txn, one WAL frame per call |
| `subscribe` / `LIVE select` — delta on every commit, WebSocket at `/api/products/live` | **13c** | subscription registry (scoped Stage 3) |
| `/pricing` screen patches price cells in open browsers | **13d** | SSR + `wo:live`/`wo:bind` client runtime |
| Millions of readers + millions of live recipients | **13e** | scale targets + load harness |

## The scale story (13e)

How "one product, millions of customers" works — all of it existing design, instantiated for this demo:

- **RAM-resident.** The engine is the in-memory design of [`03-inmemory-engine.md`](../../runtime/database/03-inmemory-engine.md); the WAL/disk phases (10–12) sit *behind* the read path for durability, never in front of it.
- **Reads scale across cores.** Thread-per-core, shared-nothing shards behind `SO_REUSEPORT` ([`09-concurrency-scaleout.md`](../../plan/09-concurrency-scaleout.md)). A hot product row is owned by one shard but read-replicated to every thread, refreshed by the same per-thread broadcast that feeds subscribers — so `GET /api/products/1` spreads over all cores with zero contention, while writes keep a single owner.
- **Live updates fan out in two stages.** One `set_price` commit → one delta message **per thread** (not per subscriber, per plan 09d) → each thread predicate-matches its local subscription table and batches socket writes on its own ring. Kernel primitives only: edge-triggered epoll today ([`done/02-event-loop-epoll.md`](../../plan/done/02-event-loop-epoll.md)), per-thread io_uring next ([`exploration/linux/07-io_uring.md`](../../plan/exploration/linux/07-io_uring.md)).

Verification targets (1 M aggregate reads/s of one product on 16 cores, 1 M live subscribers with p99 delta delivery < 250 ms, commit→first-delta p99 < 10 ms) are defined in [plan 13 § 13e](../../plan/13-class-model-live-pricing.md).

## Try it today

```bash
just pricing-demo            # scripted CRUD round-trip, self-contained
# or:
cargo run --bin wo -- run docs/examples/pricing
# [wo] compiled catalog — 2 types
curl -X POST localhost:8080/api/products \
     -H "Content-Type: application/json" -d '{"sku":"WO-001","name":"writeonce mug"}'
curl localhost:8080/api/products
```

Methods and live push are the next sub-phases (13b/13c). For the `type`-based minimal program, see [`../hello/`](../hello/); for the full workspace shape, [`../../examples/ecommerce/`](../ecommerce/).
