# Phase 5 — Go Client SDK

> A typed Go client with subscription-first design — subscribe to a live query in 5 lines, deltas arrive on a channel.

**Previous**: [Phase 4 — Client API](./04-client-api.md) | **Next**: [Phase 6 — Low-Code Full-Stack](./06-lowcode-fullstack.md) | **Index**: [database.md](../database.md)

---

Concrete scenario: a developer writes a Go backend that serves a web frontend, and uses the `.wo` database as the store. They must be able to **subscribe to a query in ~5 lines of idiomatic Go** and have deltas arrive on a channel.

Everything else the SDK does — connect, query, mutate, transact — is table stakes covered by every existing Go DB driver. Subscriptions are what this SDK has to get right.

## Target API Surface

The engine speaks `.wo` on the wire. Every SDK method — typed or untyped — is a thin wrapper over one primitive: **send a `.wo` source string with `$name` parameters, get back a uniform `Result`**.

```go
import "go.writeonce.dev/wo"

// 1. Connect
client, err := wo.Connect(ctx, "wo://db.example.com:5555",
    wo.WithAPIKey(os.Getenv("WO_KEY")),
    wo.WithTLS(tlsConfig),
)
defer client.Close()

// 2. Wo — the primitive: run ANY .wo source (one statement or a whole
//    BEGIN...COMMIT block mixing SQL, Cypher, and document updates). Params
//    use the $name rule from the .wo language spec.
result, err := client.Wo(ctx, `
  UPDATE products
    SET inventory.on_hand -= $qty
    WHERE id = $pid AND inventory.on_hand >= $qty
    RETURNING id AS pid;

  INSERT INTO orders (user_id, total_cents, status)
    VALUES ($uid, $total, 'pending')
    RETURNING id AS oid;

  MATCH (u:user {id: $uid}), (p:product {id: $pid})
    CREATE (u)-[:PURCHASED {order_id: $oid, qty: $qty, at: now()}]->(p);
`, wo.Params{
    "uid": uid, "pid": 42, "qty": 2, "total": 9800,
})

// Result layout:
//   result.Rows       — rows from trailing SELECT/MATCH/RETURNING statements
//   result.Aliases    — the RETURNING alias table: {"pid": 42, "oid": 17}
//   result.Affected   — rows touched by INSERT/UPDATE/DELETE, per-statement

// 3. Query — sugar over Wo that scans trailing rows into a typed destination.
var products []Product
err = client.Query(ctx,
    "SELECT id, sku, price_cents, meta FROM products WHERE price_cents < $max",
    wo.Params{"max": 5000},
).Scan(&products)

// 4. Exec — sugar for writes that don't return rows.
_, err = client.Exec(ctx,
    "UPDATE products SET price_cents = $new WHERE id = $id",
    wo.Params{"new": 4900, "id": 42},
)

// 5. Tx — wraps Wo/Query/Exec in a server-side BEGIN...COMMIT. The callback's
//    return value decides commit vs rollback. Useful when the program needs
//    to branch between statements on intermediate results.
err = client.Tx(ctx, func(tx *wo.Tx) error {
    r, err := tx.Wo(ctx,
        `UPDATE products SET inventory.on_hand -= $qty
         WHERE id = $pid AND inventory.on_hand >= $qty
         RETURNING id AS pid;`,
        wo.Params{"pid": 42, "qty": 2})
    if err != nil { return err }
    if r.Affected[0] == 0 { return wo.ErrInsufficientInventory }

    _, err = tx.Wo(ctx, `
      INSERT INTO orders (user_id, status) VALUES ($uid, 'pending') RETURNING id AS oid;
      MATCH (u:user {id: $uid}), (p:product {id: $pid})
        CREATE (u)-[:PURCHASED {order_id: $oid, qty: $qty}]->(p);
    `, wo.Params{"uid": uid, "pid": 42, "qty": 2})
    return err
})
```

`Wo` is the primitive; `Query`, `Exec`, and `Subscribe` are typed sugar. If the engine accepts the `.wo` source on disk, `client.Wo` accepts the same string over the wire.

### When to use raw `.wo` vs typed codegen

Both styles coexist in the same program; they share the connection pool.

| Use raw `.wo` (`client.Wo`) when | Use typed codegen (`client.Orders.Create`, etc.) when |
| --- | --- |
| Ad-hoc queries, admin tools, one-off scripts | The app's hot path — compile-time schema checking + IDE autocomplete pay for themselves |
| Cross-cutting queries that join multiple generated types | Per-type CRUD + subscriptions |
| Multi-statement transactions threading `RETURNING` aliases | Single-statement operations |
| DB repair, data migration, ad-hoc analytics | Anything the codegen already covers |
| You want to paste a block from a `.wo` source file straight into Go | You want refactor-safe struct field access |

The canonical rule: **write typed code first, drop to raw `.wo` when the type system gets in the way**. They interleave freely — a typed `client.Orders.Subscribe(...)` can run next to a raw `client.Wo(...)` admin query in the same handler.

## Subscribe — The Primary Use Case

Idiomatic Go for a stream of values is a channel read inside a `for` loop, cancelled by `context.Context`. That is the exact shape a `.wo` subscription should take:

```go
sub, err := client.Subscribe(ctx,
    "LIVE SELECT sku, inventory.on_hand FROM products WHERE sku IN $skus",
    wo.Params{"skus": cartSkus},
)
if err != nil { return err }
defer sub.Close()

for delta := range sub.Deltas() {
    switch d := delta.(type) {
    case wo.Insert:
        log.Printf("new row: %+v", d.Row)
    case wo.Update:
        log.Printf("sku=%s on_hand=%d -> %d", d.Key, d.Old["on_hand"], d.New["on_hand"])
    case wo.Delete:
        log.Printf("removed: %s", d.Key)
    case wo.Resync:
        // server dropped our queue — refetch and resume
        currentState = refetch()
    }
}

// loop exits when:
//   - ctx cancelled (client shutdown)
//   - sub.Close() called (defer)
//   - server sent COMPLETE (schema change, permission revoked)
// sub.Err() returns the reason
if err := sub.Err(); err != nil { log.Fatal(err) }
```

**Contract:**

- `sub.Deltas()` returns `<-chan wo.Delta` — standard read-only channel. The SDK closes it when the subscription ends.
- `ctx` cancellation immediately stops deliveries and closes the channel. No leaked goroutines.
- Ordering: deltas arrive in commit order. A `DELTA` on the wire always reflects a committed transaction.
- Back-pressure: the channel has a bounded buffer (default 1024). If it fills, the SDK's policy kicks in (see below).

## Typed SDK via `.wo` Schema Codegen

The `.wo` **schema layer** ([Phase 2](./02-wo-language.md)) declares types. `wo-gen` reads the type DSL — not the underlying `##sql`/`##doc`/`##graph` blocks — as its input; that way one Go struct corresponds to one entity, with embedded documents and graph-edge projections folded in naturally.

```wo
type Product {
  id:        Id
  sku:       SKU @unique
  price:     Money
  meta:      { title: Text, description: Markdown, images: [Url], reviews: [Review] }
  inventory: { on_hand: Int @check(>= 0), reserved: Int = 0, reorder_at: Int }
  purchased_by: multi User via Purchase     -- inverse graph link
}
```

```bash
wo-gen --schema ./schema.wo --out ./internal/wodb
```

Produces:

```go
package wodb

// from `type Product` — embedded structs compile from the inline `{...}` fields
type Product struct {
    ID           int64           `wo:"id"`
    SKU          string          `wo:"sku"`
    Price        Money           `wo:"price"`
    Meta         ProductMeta     `wo:"meta"`
    Inventory    InventoryLvl    `wo:"inventory"`
    PurchasedBy  []PurchaseEdge  `wo:"purchased_by"`   // link-with-props → edge struct
}

// embedded document inside Product.Meta
type ProductMeta struct {
    Title       string              `wo:"title"`
    Description string              `wo:"description"`
    Images      []string            `wo:"images"`
    Attributes  map[string]string   `wo:"attributes"`
    Reviews     []Review            `wo:"reviews"`
}

// graph link carrying properties — target + edge props in one struct
type PurchaseEdge struct {
    Target User       `wo:"target"`
    Order  int64      `wo:"order"`
    Qty    int        `wo:"qty"`
    At     time.Time  `wo:"at"`
}

// registered live queries become typed helpers
func InventoryChanged(ctx context.Context, c *wo.Client, skus []string) (*wo.TypedSubscription[InventoryLvl], error)
```

One type declaration → one Go struct. Zero-property graph edges (`multi User @edge(:FOLLOWS)`) generate `Friends []User`; link-with-properties types generate `[]EdgeStruct`; computed fields become read-only struct fields populated by the planner.

**Transition path.** While the Phase 2 prototype is still authored directly in `##sql/##doc/##graph` blocks (the query layer), `wo-gen` accepts either — a file of type declarations, or the raw paradigm blocks — and emits the same Go output. The type DSL becomes mandatory only once Phase 6 full-stack blocks (which attach to types) start shipping.

Typed subscription loop loses all `interface{}` ceremony:

```go
sub, err := wodb.InventoryChanged(ctx, client, cartSkus)
if err != nil { return err }
defer sub.Close()

for d := range sub.C {
    switch d.Kind {
    case wo.DeltaUpdate:
        log.Printf("sku=%s now %d in stock", d.Key, d.New.OnHand)
    }
}
```

Generics (Go 1.18+) make `TypedSubscription[T]` a single parameterized type — no per-query generated struct. Only the `T` struct itself is generated.

## Connection Lifecycle

```go
type Client struct {
    // opaque; holds a connection pool, codec, session registry
}

func Connect(ctx context.Context, dsn string, opts ...Option) (*Client, error)
func (c *Client) Close() error
func (c *Client) Ping(ctx context.Context) error
```

Inside the SDK, one TCP connection per client is fine for native protocol (multiplexed), but a small pool (2–4) helps when one connection's receive goroutine is saturated decoding a large result set. Connection state:

- **Connecting** → `HELLO` sent, waiting for `WELCOME`
- **Ready** → normal operation
- **Reconnecting** → transient network error; automatic exponential backoff; all subscriptions queued for re-registration
- **Closed** → terminal

**Reconnection semantics for subscriptions** (the subtle part): on reconnect, the SDK re-sends every active `SUBSCRIBE` frame. The server replies with a `RESYNC` marker and the current matching state. The app's subscription channel emits a single `wo.Resync{}` value so the consumer knows to rebuild local state. No delta is silently lost, no delta is silently duplicated.

## Options

Fluent options, not a bloated config struct:

```go
wo.WithAPIKey(key string)
wo.WithJWT(token string)
wo.WithMTLS(cert tls.Certificate)
wo.WithTLS(cfg *tls.Config)
wo.WithPoolSize(n int)                // default 2
wo.WithSubscriptionBuffer(n int)      // default 1024
wo.WithOverflowPolicy(wo.DropAndResync | wo.Coalesce | wo.Disconnect)
wo.WithLogger(l *slog.Logger)
wo.WithRetry(wo.RetryPolicy{...})
wo.WithProtocol(wo.ProtocolNative | wo.ProtocolGraphQL)  // native default
```

## Transactions

`client.Tx` maps to [Phase 2's](./02-wo-language.md) `BEGIN ... COMMIT`. The callback's return value decides commit vs rollback:

- `return nil` → `COMMIT` sent, error only if server rejects commit
- `return err` → `ROLLBACK` sent, original `err` surfaced to caller
- `panic` → `ROLLBACK` sent, panic re-raised
- `ctx` cancel → `ROLLBACK` sent, `ctx.Err()` returned

Nested `tx.Wo`/`tx.Query`/`tx.Exec` route to the same server-side transaction — no connection hopping. The SDK enforces this by pinning the transaction to one connection for its lifetime. `RETURNING` aliases bound by one statement in the txn are visible to every later statement in the same txn through the server-side alias table (see [Phase 2 — Transaction Coordinator](./02-wo-language.md#cross-paradigm-transaction-coordinator)), so a Go `tx.Wo` call can leave `$oid` set and the next `tx.Wo` call can use it.

## Back-Pressure Handling in the SDK

The server's back-pressure policy from [Phase 4](./04-client-api.md) is mirrored client-side:

| Client situation | SDK behavior |
| --- | --- |
| Consumer reading channel fast enough | Normal delivery |
| Channel buffer full (1024 unread deltas) | Per `WithOverflowPolicy`: drop buffered + emit `wo.Resync`, or coalesce same-key updates, or close the subscription with `ErrOverflow` |
| Network stalled | `ctx.Deadline` + keepalive `PING` every 10s; stall > 30s → disconnect and reconnect |
| Server closed subscription | Channel closed, `sub.Err()` returns reason (schema change, permission revoked, engine shutdown) |

**Never block the receive goroutine on a full channel.** The SDK drains the socket no matter what; overflow policy decides what to do with the deltas it can't deliver.

## Full Cart-Inventory Example

End-to-end: Go HTTP handler that renders a cart page and keeps its inventory line live via Server-Sent Events to the browser. The SDK drives the upstream subscription to `.wo`:

```go
func (h *Handler) cartInventoryStream(w http.ResponseWriter, r *http.Request) {
    skus := parseSkus(r.URL.Query().Get("skus"))

    w.Header().Set("Content-Type", "text/event-stream")
    w.Header().Set("Cache-Control", "no-cache")
    flusher := w.(http.Flusher)

    sub, err := wodb.InventoryChanged(r.Context(), h.db, skus)
    if err != nil { http.Error(w, err.Error(), 500); return }
    defer sub.Close()

    for d := range sub.C {
        payload, _ := json.Marshal(d)
        fmt.Fprintf(w, "event: inventory\ndata: %s\n\n", payload)
        flusher.Flush()
    }
}
```

The browser connects once with `new EventSource('/cart/inventory?skus=...')`. The Go handler holds one subscription to `.wo`. When inventory commits in the database, the delta flows: engine → subscription registry → Go SDK channel → SSE stream → DOM update. Zero polling anywhere in the chain.

## Go-Specific Design Details

| Go idiom | Application |
| --- | --- |
| `context.Context` threading | Every method takes `ctx` as first arg; cancellation propagates to the wire |
| `io.Closer` | `Client`, `Tx`, `Subscription` all implement `Close() error` |
| Small interfaces | `type Runner interface { Wo(ctx, src, params) (wo.Result, error) }` — `*Client` and `*Tx` both satisfy it, so helper functions compose cleanly |
| `database/sql`-style `Scan` | `client.Query(...).Scan(&dest)` accepts struct, slice of struct, or primitives |
| `sql.Null*` analogues | `wo.NullString`, `wo.NullInt64`, `wo.NullDoc` for optional doc columns |
| Struct tags | `wo:"column_name"` + JSON-style for nested doc fields (`wo:"meta.title"`) |
| `errors.Is` / `errors.As` | `errors.Is(err, wo.ErrConflict)`, `wo.AsError(err, &woErr)` |
| No goroutine leaks | Every background goroutine tied to ctx or a sync.WaitGroup closed in `Client.Close()` |
| Testing via interfaces | `wo.DB` interface; provide `wotest.NewMock()` for unit tests; real embedded engine for integration |

## Comparison With Existing Go DB SDKs

| SDK | Query style | Subscriptions | Transactions | Typed results |
| --- | --- | --- | --- | --- |
| `database/sql` + `pq` | SQL strings | No | Yes | Manual `Scan` |
| `pgx` | SQL strings | `LISTEN/NOTIFY` only (no row-level) | Yes | Manual or `pgxscan` |
| `sqlc` | Generated Go funcs from `.sql` | No | Yes | Generated structs |
| `ent` | ORM | No | Yes | Generated |
| `go-redis` | Commands | Pub/sub + keyspace notifications (no query) | Multi/Exec | Manual |
| `surrealdb/surrealdb.go` | Raw queries | `Live()` returning channel | Yes | Manual |
| `gqlgen` / `machinebox/graphql` | GraphQL docs | WebSocket subscriptions | N/A | Generated |
| **`sa`** (this design) | `.wo` queries | **Native `LIVE` → typed channel** | Yes | Codegen from schema |

The reference points are `sqlc` (for the codegen pipeline) and `surrealdb-go` (for the subscription channel API). Combining their best ideas and tightening the subscription contract is what this SDK is.

## Reference Implementations To Steal From

- **surrealdb/surrealdb.go** — `Live()` returns a channel; closest API precedent. <https://github.com/surrealdb/surrealdb.go>
- **jackc/pgx** — reference quality for a Go database driver. Connection pool, copy protocol, prepared statements all done right. <https://github.com/jackc/pgx>
- **sqlc-dev/sqlc** — codegen from SQL to typed Go. The model for `.wo` → Go. <https://github.com/sqlc-dev/sqlc>
- **Khan/genqlient** — generated typed GraphQL client. Ergonomic precedent for typed query helpers. <https://github.com/Khan/genqlient>
- **nats-io/nats.go** — subscription-first API, back-pressure handled well. `sub.NextMsg(ctx)` and channel-based `ChanSubscribe` both supported. <https://github.com/nats-io/nats.go>
- **hasura/go-graphql-client** — GraphQL subscriptions over WebSocket in Go. <https://github.com/hasura/go-graphql-client>

## SDK Delivery

| Artifact | Purpose |
| --- | --- |
| `go.writeonce.dev/wo` | Runtime package: client, query, subscribe |
| `go.writeonce.dev/wo/wotest` | Mock client + in-memory engine for unit tests |
| `wo-gen` binary | Reads `schema.wo`, emits typed Go code |
| Go module example repo | Cart + inventory demo wired end-to-end |
| Generated docs | `go doc` + hosted examples |

Publishing strategy: semantic versioning, `v0.x` while the wire protocol is unstable, `v1.0` only after the protocol is frozen.

## Why The SDK Matters As Much As The Engine

A database with a beautiful engine and a painful client is a database no one uses. The e-commerce Go backends this targets are built under deadline — if `Subscribe` is not as easy as opening a channel, developers will reach for polling (`time.Tick` + `SELECT`) and defeat the whole architecture.

The success metric is blunt: **a developer who has never seen `.wo` before should have a working subscription to a live query inside 15 minutes**, counting install, schema codegen, and the first delta landing on their channel. If the SDK is any harder than that, the rest of this doc is academic.

## Future SDKs

Same shape, other languages:

- **TypeScript / browser** — fetch + WebSocket for GraphQL subscriptions; types via codegen from `.wo`. Highest priority after Go for a web-first product.
- **Rust** — direct native protocol, `tokio`-friendly, `impl Stream<Item = Delta>` for subscriptions.
- **Python** — async/await, `async for delta in sub` idiom.
- **Java / Kotlin** — Flow (Kotlin) or Reactive Streams (Java) for subscriptions.

Each follows the same rule: subscribe-to-query must be the shortest, most obvious thing in the API.
