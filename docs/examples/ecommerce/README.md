# `ecommerce` — a sample writeonce e-commerce app

A storefront + checkout + live ops dashboard in **~300 lines of `.wo`**. Exercises the features that make `.wo` distinct from a plain REST app: **cross-paradigm ACID transactions**, **type-attached lifecycle triggers**, **link types with properties**, and a **live-updating operations table**.

> Like the [blog sample](../blog/), this is a **docs artifact** — illustrative `.wo` source showing the shape of a real `wo init`'d project. Toolchain specified in [`../../runtime/wo-language.md`](../../runtime/wo-language.md).

## What's here

| File | What it shows |
| --- | --- |
| [`types/product.wo`](./types/product.wo) | Relational scalars + embedded doc (`meta`, `inventory`) + computed field (`available`) + graph edge (`similar_to`) + inventory-low trigger |
| [`types/order.wo`](./types/order.wo) | Tagged union status, array-of-struct `line_items`, computed `total`, four lifecycle triggers setting timestamp columns atomically |
| [`types/customer.wo`](./types/customer.wo) | Role union + `multi Product via Purchase` (link with properties) + `backlink Order.customer` |
| [`types/purchase.wo`](./types/purchase.wo) | `link Customer -> Product` — a graph edge **type** with its own columns (`order`, `qty`, `unit_price`) |
| [`logic/checkout.wo`](./logic/checkout.wo) | The canonical cross-paradigm transaction: reserve inventory + insert order + create graph edge, atomic across all three engines |
| [`ui/admin_orders.wo`](./ui/admin_orders.wo) | **The live order-ops table** — role-gated, auto-subscribes, delta-in-place updates |
| [`ui/storefront.wo`](./ui/storefront.wo) | Customer-facing product list with live inventory |
| [`ui/order_tracker.wo`](./ui/order_tracker.wo) | Customer-facing order history, same live engine, policy-filtered source |
| [`app.wo`](./app.wo) | Route table, Admin/Ops bypass policy, idempotent `seed()` |
| [`tests/checkout_test.wo`](./tests/checkout_test.wo) | Three tests covering the atomic checkout, the abort-without-partial-state guarantee, and the live-subscription delta stream |

## Project layout

```
ecommerce/
├── wo.toml
├── app.wo
├── types/
│   ├── customer.wo
│   ├── product.wo
│   ├── order.wo
│   └── purchase.wo      # link type — graph edge with properties
├── logic/
│   └── checkout.wo      # transactional functions (fn … in txn snapshot)
├── ui/
│   ├── storefront.wo
│   ├── order_tracker.wo
│   └── admin_orders.wo  # the live ops table
└── tests/
    └── checkout_test.wo
```

## Run it

```bash
$ cd docs/examples/ecommerce
$ wo run
[wo] parsing: 10 files, 4 types + 1 link type, 3 ui screens, 4 fns
[wo] compiling schema: 3 sql tables, 2 doc collections, 2 graph edge types
[wo] starting runtime (engine: in-memory, data_dir: ./data, isolation: snapshot)
[wo] on startup: seed() — 1 customer, 2 products
[wo] HTTP listening on :8080

  GET    /api/products              list
  GET    /api/products/:id          get
  WS     /api/products/live         subscribe
  GET    /api/orders                list
  GET    /api/orders/:id            get
  WS     /api/orders/live           subscribe
  GET    /api/customers/:id         get
  GET    /api/customers/me          me
  PATCH  /api/customers/:id         update
  WS     /api/customers/live        subscribe
  POST   /api/fn/checkout           fn checkout(customer, product, qty) -> Order
  POST   /api/fn/mark_paid          fn mark_paid(order)
  POST   /api/fn/mark_shipped       fn mark_shipped(order)

  GET    /                          ui.storefront
  GET    /product/:sku              ui.product-detail
  GET    /orders                    ui.order-tracker
  GET    /admin/orders              ui.admin-orders       (Admin | Ops)
```

> **Runtime model.** The engine is a single-threaded event loop today ([Phase 2 concurrency](../../runtime/database/02-wo-language.md#concurrency-model)). Snapshot isolation is trivially correct because there are no concurrent writers — the checkout, mark_paid, and mark_shipped fns run sequentially even when fired in quick succession. The throughput ceiling is ~one core (plenty for the sample); sharding across independent engine processes is the horizontal-scale path.

## Exercise the cross-paradigm checkout

The `fn checkout(...)` in [`logic/checkout.wo`](./logic/checkout.wo) is the canonical Phase 2 test case: one transaction that mutates relational, document, and graph state atomically.

```bash
# Place an order — one HTTP call runs the whole BEGIN ... COMMIT block
$ curl -X POST localhost:8080/api/fn/checkout \
    -H "Authorization: Bearer $CUSTOMER_TOKEN" \
    -d '{"customer":1, "product":2, "qty":3}'

{"id":1, "status":"Pending", "total":5997, "line_items":[{...}], "placed_at":"..."}

# Verify inventory was reserved (not yet decremented)
$ curl localhost:8080/api/products/2
{"sku":"SKU-GIZMO", "inventory":{"on_hand":12, "reserved":3, "reorder_at":3}, "available":9, ...}

# Verify the graph edge was created in the same transaction
$ curl localhost:8080/api/customers/1/purchased
[{"target":{"sku":"SKU-GIZMO"}, "order":1, "qty":3, "unit_price":1999, "at":"..."}]
```

If the inventory check failed inside `checkout`, **none** of the above writes happen — the order isn't created, the reservation isn't made, and the graph edge doesn't exist. That atomicity is the whole point of building your own engine instead of stitching Postgres + Neo4j.

## Watch the live admin ops table

Open the admin orders UI in a browser:

```bash
$ open http://localhost:8080/admin/orders     # authenticated as Admin or Ops
```

The page renders a table with the columns declared in [`ui/admin_orders.wo`](./ui/admin_orders.wo). Behind the scenes, the client runtime has opened one WebSocket to the engine's subscription endpoint:

```
WS /api/orders/live  ?  status!=Cancelled
```

Now, from another terminal, fire a sequence of state changes:

```bash
# 1. New customer places an order — admin table gains a row, highlighted for 2s
$ curl -X POST localhost:8080/api/fn/checkout -d '{"customer":2,"product":1,"qty":1}'

# 2. Payment webhook flips status Pending → Paid — row updates in place, paid_at fills in
$ curl -X POST localhost:8080/api/fn/mark_paid -d '{"order":2}'

# 3. Ops ships the order — status → Shipped, shipped_at fills in
$ curl -X POST localhost:8080/api/fn/mark_shipped -d '{"order":2}'
```

The browser table re-renders each row delta as it arrives, without a full list refetch. `status` cell swaps its pill colour; timestamp cells populate. No polling anywhere in the path — the deltas are emitted by the transaction coordinator on commit, routed through the subscription registry, and pushed down the socket ([Phase 4](../../runtime/database/04-client-api.md)).

A filtered subscription — the admin clicking the **"Ready to ship"** quick-filter — doesn't rebuild state client-side. It sends the new predicate to the server, which replies with a `SNAPSHOT` frame of just the matching rows, then streams deltas that match the new predicate. Also zero-polling.

## Generate a typed Go client

```bash
$ wo gen sdk --lang go --out ./client
[wo] reading types from ./types/ and fns from ./logic/
[wo] writing ./client/sdk.go (4 types, 13 endpoints, 4 fns, 3 subscriptions)
```

The generated client speaks the native wire protocol:

```go
import "myshop/client"

c, _ := client.Connect(ctx, "wo://localhost:8080", client.WithToken(token))

// Typed transactional function call
order, err := c.Checkout(ctx, client.CheckoutArgs{
    Customer: 1,
    Product:  2,
    Qty:      3,
})

// Typed live subscription — same wire as the admin UI uses
sub, _ := c.Orders.Subscribe(ctx, client.Where{Status: client.Ne(client.Cancelled)})
for d := range sub.C {
    switch d.Kind {
    case client.Insert:
        fmt.Printf("new order #%d from %s — $%.2f\n", d.Row.ID, d.Row.Customer.Name, float64(d.Row.Total)/100)
    case client.Update:
        fmt.Printf("order #%d → %s\n", d.Row.ID, d.Row.Status)
    }
}
```

## Checkout from Go without codegen

For ad-hoc scripts, admin tools, or client paths not on the app's hot loop, send raw `.wo` DML with `client.Wo(...)`. The server parses the block exactly like `wo run` would — same parser, same transaction coordinator, same `RETURNING` alias table — so the **cross-paradigm checkout runs in one round trip**:

```go
import "go.writeonce.dev/wo"

c, _ := wo.Connect(ctx, "wo://localhost:8080", wo.WithToken(token))

// Same logic as fn checkout(), but authored at the Go call site.
// BEGIN SNAPSHOT ... COMMIT runs server-side; RETURNING aliases ($pid, $oid)
// thread from the SQL UPDATE/INSERT into the Cypher CREATE within the txn.
result, err := c.Wo(ctx, `
  BEGIN SNAPSHOT;

    UPDATE products
      SET inventory.reserved = inventory.reserved + $qty
      WHERE id = $pid AND available >= $qty
      RETURNING id AS pid;

    INSERT INTO orders (customer, status, line_items)
      VALUES ($uid, 'Pending', [{product: $pid, qty: $qty, unit_price: $unit}])
      RETURNING id AS oid;

    MATCH (u:Customer {id: $uid}), (p:Product {id: $pid})
      CREATE (u)-[:PURCHASED {order: $oid, qty: $qty, unit_price: $unit}]->(p);

  COMMIT;
`, wo.Params{"uid": 1, "pid": 2, "qty": 3, "unit": 4999})

if err != nil { log.Fatal(err) }
orderID := result.Aliases["oid"].(int64)
fmt.Printf("created order #%d\n", orderID)
```

**When to reach for this form** — see the [raw-vs-typed guidance in the Go SDK doc](../../runtime/database/05-go-sdk.md#when-to-use-raw-wo-vs-typed-codegen). Rule of thumb: typed `c.Checkout(...)` for the app's storefront; raw `c.Wo(...)` for an ops console that runs a custom report, or when you want to paste a block from [`logic/checkout.wo`](./logic/checkout.wo) straight into Go.

## Run the tests

```bash
$ wo test
=== tests/checkout_test.wo ===
  checkout atomically reserves inventory, creates order, and creates graph edge  OK (8ms)
  checkout aborts without partial state when inventory is insufficient           OK (4ms)
  admin live-orders subscription receives deltas across the order lifecycle      OK (14ms)

PASS  3/3 tests, 0 failures (26ms)
```

The third test is the important one for the docs: it proves that the same engine that serves `/admin/orders` in the browser delivers deltas in commit order through a programmatic `subscribe live` handle. One engine, one delta stream, two consumers (the browser and the test).

## Build a production binary

```bash
$ wo build --target linux-amd64 --out bin/shop
[wo] static binary: bin/shop (15 MB — database + HTTP + subscription engine embedded)
$ ./bin/shop
[wo] HTTP listening on :8080
```

Drop the binary on a server, give it a writable directory for `./data/` (WAL + engine state), run it behind nginx or let it terminate TLS itself. The admin ops table works on the first page-load — no Redis, no Kafka, no separate DB process, no ORM-and-migration dance.

## Compare to the blog sample

Both projects use the same language and runtime. They showcase different slices:

| Feature | [blog](../blog/) | ecommerce (this project) |
| --- | --- | --- |
| Embedded document | `article.meta` | `product.meta`, `product.inventory` |
| Graph edges (zero-prop) | tags, related | similar_to |
| Graph edges **with** properties | — | `type Purchase link Customer -> Product` |
| Tagged union | — | `Pending \| Paid \| Shipped \| ...` |
| Computed field | `word_count` | `available`, `total` (sum over line_items) |
| Array-of-struct column | — | `line_items: [{product, qty, unit_price}]` |
| Stored procedure (`fn ... in txn`) | seed only | full checkout + fulfillment |
| Cross-paradigm transaction | — | `checkout` (relational + doc + graph atomic) |
| Live subscription | list view | live **ops** table with in-place delta updates |
| Row-level policy | draft hiding | customer sees own orders; ops/admin sees all |

If the blog shows **what a CRUD app looks like in `.wo`**, the ecommerce sample shows **what a transactional business app looks like in `.wo`** — and why building the engine as part of the language is the differentiator.

## What to read next

- [`../../runtime/wo-language.md`](../../runtime/wo-language.md) — language overview and toolchain
- [`../../runtime/database/02-wo-language.md`](../../runtime/database/02-wo-language.md) — the schema/query two-layer spec
- [`../../runtime/database/04-client-api.md`](../../runtime/database/04-client-api.md) — the wire protocol and subscription engine behind the live ops table
- [`../../runtime/database/06-lowcode-fullstack.md`](../../runtime/database/06-lowcode-fullstack.md) — `##ui` / `##app` block spec
- [`../../../prototypes/wo-db/tests/checkout.wo`](../../../prototypes/wo-db/tests/checkout.wo) — the C++ prototype's smoke test that exercises the same cross-paradigm transaction at the query layer
