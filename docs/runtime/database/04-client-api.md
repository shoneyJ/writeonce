# Phase 4 — Client API: Wire Protocol and Subscriptions

> How remote clients connect, query, and subscribe to live changes — no polling anywhere in the chain.

**Previous**: [Phase 3 — In-Memory Engine](./03-inmemory-engine.md) | **Next**: [Phase 5 — Go Client SDK](./05-go-sdk.md) | **Index**: [database.md](../database.md)

---

Once the engine is complete it is a **traditional database server**: remote clients connect over the network, issue queries, receive results, and (critically for e-commerce UX) **subscribe to changes without polling**.

The client API has three problems to solve:

1. **Wire protocol** — how bytes move between client and server.
2. **Query surface** — what queries look like from the client's perspective (raw `.wo`, SQL, GraphQL, REST).
3. **Subscriptions** — how the server pushes change notifications to clients when matching data mutates, with no client-side polling.

## The Polling Problem This Must Avoid

Every naive realtime system reaches for polling first. For an e-commerce platform it is disqualifying:

| Polling | Subscriptions |
| --- | --- |
| Client asks "anything new?" every N ms | Server tells client "here's what changed" when it changes |
| Wasted RTTs when nothing changed | Zero traffic when nothing changes |
| Stale data up to N ms old | Sub-ms latency after commit |
| `O(clients × poll_rate)` server load | `O(mutations × matched_subscribers)` — scales with real change, not client count |
| Inventory display lies for up to N ms | Inventory display reflects the commit |
| "Order shipped" email triggered by cron | Fired by a committed status-change |

Every subscription-based design in this section follows the same rule already set by writeonce in [05-datalayer.md](../../05-datalayer.md) and [03-data.md](../../03-data.md): **the client registers a query once, the server pushes deltas on commit, the client never asks again.**

## Protocol Layer — Pick One or Both

Two protocol tiers make sense: a **native binary protocol** for app servers and ORMs that want every microsecond, and a **GraphQL-over-WebSocket layer** for browsers, mobile apps, and third parties. They are not alternatives — they share the same planner and subscription registry underneath.

| Option | Best for | Trade-off |
| --- | --- | --- |
| **Custom binary over TCP** | App servers, in-house clients, highest throughput | Need to ship client libs in every language |
| **Postgres wire protocol** (libpq) | Reuse the Postgres client ecosystem (psql, pgx, node-postgres, JDBC) | Locked into Postgres's shape — no native graph/live-query verbs |
| **gRPC (HTTP/2)** | Cross-language, well-tooled, server-streaming RPC covers subscriptions | Protobuf schema overhead; HTTP/2 stack cost |
| **GraphQL over HTTP + WebSocket** | Web/mobile clients, schema-aware tooling, built-in `subscription` operation | Parser/resolver overhead, N+1 risks |
| **REST + SSE** | Simplest to integrate (curl, browser `fetch`) | Verb-per-endpoint sprawl, SSE is unidirectional |

**Recommended combination:**

- **Native binary protocol** for first-party app servers (cart service, checkout, fulfillment).
- **GraphQL over WebSocket** for everything else (web, mobile, partner APIs).

Both terminate at the same **session layer** inside the server, which delegates to the `.wo` planner.

## Native Binary Protocol — Shape

A minimal framing that's compatible with io_uring on both ends:

```
┌────────┬────────┬──────────┬─────────────────────────────┐
│ opcode │ req_id │   len    │          payload            │
│  u8    │  u64   │   u32    │    bincode / msgpack        │
└────────┴────────┴──────────┴─────────────────────────────┘
```

Opcode set:

| Opcode | Direction | Purpose |
| --- | --- | --- |
| `HELLO` | C → S | Protocol version + auth credentials |
| `WELCOME` | S → C | Session id + server capabilities |
| `PREPARE` | C → S | Compile a `.wo` query, cache plan on server |
| `EXECUTE` | C → S | Run prepared plan with bound parameters |
| `RESULT` | S → C | Full result set for one query |
| `BEGIN` / `COMMIT` / `ROLLBACK` | C → S | Explicit transaction control |
| `SUBSCRIBE` | C → S | Register a live query, get a subscription id |
| `UNSUBSCRIBE` | C → S | Cancel a subscription |
| `DELTA` | S → C | Pushed change matching a subscription |
| `COMPLETE` | S → C | Subscription terminated server-side (schema change, etc.) |
| `ERROR` | S → C | Typed error with query context |
| `PING` / `PONG` | bidirectional | Dead connection detection (no polling for data — just keepalive) |

Multiplexed: many in-flight `req_id`s per connection, responses interleaved. Matches io_uring's async nature naturally — a connection never blocks on a slow query.

## GraphQL — Schema, Queries, Mutations, Subscriptions

GraphQL has the three verbs e-commerce actually uses:

| GraphQL operation | `.wo` mapping |
| --- | --- |
| `query` | `SELECT` / `MATCH` over the engine, single response |
| `mutation` | `INSERT` / `UPDATE` / `DELETE` / `CREATE` inside an implicit transaction |
| `subscription` | `LIVE SELECT` / `LIVE MATCH` — server pushes on match |

**Schema generation.** The `.wo` DDL is the source of truth; the GraphQL SDL is generated from it:

```
##sql #products  (id, sku, price_cents, meta, inventory)
##doc #product-meta (title, description, reviews, ...)
##graph (user)-[:PURCHASED]->(product)
          │
          ▼  generator
          │
type Product {
  id: ID!
  sku: String!
  priceCents: Int!
  meta: ProductMeta!
  inventory: InventoryLevel!
  similarTo(limit: Int = 10): [Product!]!       # graph traversal
  purchasedBy: [User!]!                          # graph traversal
}
type Subscription {
  productUpdated(id: ID!): Product!
  inventoryChanged(sku: String!): InventoryLevel!
  orderStatus(orderId: ID!): Order!
}
```

**Subscription example (e-commerce checkout feedback loop):**

```graphql
subscription CartInventory($skus: [String!]!) {
  inventoryChanged(sku_in: $skus) {
    sku
    onHand
    reserved
  }
}
```

A web client opens this WebSocket subscription when the cart renders. The server only pushes when a committed transaction changes any of those SKUs' inventory — the cart's "2 left!" badge is always live, no polling.

**Transport: `graphql-ws` protocol over WebSocket.** Standard, well-tooled (Apollo, urql, Relay, Hasura all speak it). Falls back to HTTP POST for plain queries and mutations.

## Subscription Engine — How Push Actually Works

This is the mechanism that makes polling unnecessary. It lives inside the transaction coordinator from [Phase 2](./02-wo-language.md):

```
     ┌─────────────────────────────────────────────────────┐
     │           Transaction Coordinator (MVCC)            │
     │                                                     │
     │   on COMMIT(txn):                                   │
     │     delta = collect_changes(txn)                    │
     │     matched = subscription_registry.match(delta)    │
     │     for (sub, rows) in matched:                     │
     │        sub.writer.push(DELTA { sub.id, rows })      │
     └─────────────────────────────────────────────────────┘
                │                        │
                ▼                        ▼
     ┌─────────────────────┐   ┌──────────────────────────┐
     │ Subscription        │   │ Session Writer (per conn)│
     │ Registry            │   │  - native: io_uring send │
     │                     │   │  - graphql: ws frame     │
     │ predicate → [subs]  │   │  - grpc: server stream   │
     └─────────────────────┘   └──────────────────────────┘
```

**Matching strategies**, in order of cost:

| Subscription shape | Matching cost | Example |
| --- | --- | --- |
| Keyed (primary key) | O(1) hash lookup on commit | `productUpdated(id: 42)` |
| Tag / secondary index | O(1) index lookup + scan of matched rows | `orderStatusByUser(userId: 17)` |
| Range | O(log n) index range + filter | `ordersPlaced(between: [start, end])` |
| Graph traversal | O(edges visited) — bound by depth/limit | `recommendationsFor(userId: 17)` |
| Arbitrary predicate | O(subs) — evaluate each against the delta | `LIVE SELECT ... WHERE complex` |

The engine indexes subscriptions by their shape so the common cases (keyed, tag-based) don't pay the arbitrary-predicate price. This is **incremental view maintenance** — the same idea that SurrealDB live queries, Materialize, Hasura, and Feldera all implement at different levels of generality.

## Connection I/O — io_uring All the Way

The same `io_uring` that drives the WAL (per [Phase 3](./03-inmemory-engine.md)) also drives client sockets. One scheduler, not a mix of epoll for networking and io_uring for storage:

| Operation | io_uring opcode |
| --- | --- |
| Accept new client | `IORING_OP_ACCEPT` |
| Read request frame | `IORING_OP_RECV` (with registered buffers) |
| Write result / delta | `IORING_OP_SEND` (with `IOSQE_IO_LINK` to chain writes) |
| TLS handshake | Userland ring integrated with `IORING_OP_RECV`/`SEND` (e.g., rustls or BoringSSL in non-blocking mode) |
| Close | `IORING_OP_CLOSE` |
| Keepalive | `IORING_OP_TIMEOUT` per connection |

A subscription push is one SQE: `SEND(client_fd, delta_frame)`. Thousands of in-flight pushes across thousands of subscribers is just thousands of SQEs — the kernel batches the actual NIC writes. No thread-per-connection, no blocking send.

## Session State

Each connected client has server-side state:

| State | Lifetime | Notes |
| --- | --- | --- |
| Identity / principal | Session | JWT or mTLS validated at `HELLO` |
| Current transaction | One txn at a time per session | Auto-rollback on disconnect |
| Prepared statements | Session | Plan cached, re-parameterized per `EXECUTE` |
| Active subscriptions | Session | All torn down on disconnect (free registry slots, stop pushing) |
| Role / RBAC context | Session | Feeds row-level policies into the planner |
| Back-pressure credits | Per-subscription | Client advertises how many outstanding `DELTA` frames it can buffer |

On disconnect (TCP close, keepalive failure, `EPOLLHUP`-equivalent from io_uring completion): all sessions state is freed, all subscriptions unregistered. Same philosophy as `wo-sub`'s `EPOLLHUP` → automatic `unsubscribe(fd)` from [05-datalayer.md](../../05-datalayer.md), scaled up to a real server.

## Back-Pressure

A slow client cannot be allowed to stall commits. The push path must never block on a socket write:

1. Each subscription has a **bounded outbound queue** (say, 1024 deltas).
2. Writer thread drains the queue via `io_uring_send`.
3. On queue overflow, the engine has three policies:
   - **Drop + resync**: mark the subscription as "behind", push a single `RESYNC` marker, client re-requests current state.
   - **Coalesce**: fold consecutive deltas for the same key into one (last-writer-wins).
   - **Disconnect**: close the connection; clients with a stale subscription reconnect.
4. The coordinator never waits on a subscription — it hands the delta to the writer and moves on.

This is the same trade-off Kafka makes with consumer lag: fast producers, independent consumers, bounded buffer, spillover policy.

## Authentication and Authorization

Covered briefly in [Phase 2](./02-wo-language.md); the wire protocol is where it bites:

- **Transport**: TLS mandatory for any non-loopback connection. Offload to `rustls` / `boringssl` userland; io_uring handles only the underlying sockets.
- **Authentication** at `HELLO`: JWT (stateless), API key (server-validated), or mTLS (cert-based).
- **Authorization**: RBAC + row-level policies evaluated inside the planner. A subscription's registered predicate is **intersected with the user's access policy at registration time** — if the policy says user 17 only sees their own orders, the subscription's effective predicate becomes `(original) AND user_id = 17`. Enforced once, not per push.
- **Rate limiting**: per-session token bucket enforced before any query work. Cheap to implement in the io_uring accept/recv path.

## Comparison: This Design vs. Existing Products

| Aspect | This design | Postgres + Hasura | Supabase Realtime | SurrealDB | Firebase |
| --- | --- | --- | --- | --- | --- |
| Transport | Custom binary + GraphQL/WS | SQL wire + GraphQL/WS | Postgres WAL → WS | HTTP + WS | Custom WS |
| Subscriptions | Native, planner-integrated | Live queries via polling Postgres | Logical replication fan-out | Native live queries | Native |
| Storage coupling | In-process | External Postgres | External Postgres | In-process | Proprietary |
| Cross-paradigm | Yes (`.wo`: rel + doc + graph) | Partial (JSONB, no graph) | Relational only | Yes (rel + doc + graph) | Doc only |
| Polling internally? | No | **Yes** (Hasura polls Postgres) | No (uses WAL) | No | No |
| io_uring throughout | Yes | No | No | Partial | No |

Hasura is the instructive one — it gives clients push subscriptions, but internally it polls Postgres because Postgres has no commit-time subscription hook. Building the subscription engine *inside* the database (as this design does) is what eliminates polling end-to-end.

## Reference Implementations

- **SurrealDB** — the tightest match: custom engine, WebSocket transport, native `LIVE SELECT`. <https://github.com/surrealdb/surrealdb>. Also in [surreal-case-study.md](../surreal-case-study.md).
- **Hasura GraphQL Engine** — production-quality GraphQL over Postgres with subscriptions. Read their `graphql-engine/server/src-lib/Hasura/GraphQL/Transport/` for subscription multiplexing. <https://github.com/hasura/graphql-engine>
- **Supabase Realtime** — Phoenix/Elixir server that tails Postgres logical replication and fans out over WebSocket. Cleanest demo of "subscriptions as a layer over an existing DB." <https://github.com/supabase/realtime>
- **PostgREST** — auto-generated REST from Postgres schema. Simpler than GraphQL, same spirit. <https://github.com/PostgREST/postgrest>
- **EdgeDB** — custom binary protocol, custom query language (EdgeQL), compiles to Postgres underneath. Good reference for protocol framing. <https://github.com/edgedb/edgedb>
- **Materialize** — incremental view maintenance as a product; every query is implicitly a subscription. <https://github.com/MaterializeInc/materialize>
- **Phoenix Channels** (Elixir) — mature pub/sub-over-WebSocket with presence, back-pressure, and reconnection baked in. Worth reading even if the server is Rust/C++.
- **graphql-ws** protocol — <https://github.com/enisdenjo/graphql-ws>. The WebSocket sub-protocol every modern GraphQL client speaks.
- **Apollo Router** — GraphQL gateway with subscription multiplexing, federation. <https://github.com/apollographql/router>

## Scope Addition to Phase 2

The client API is a sizable addition to the [Phase 2](./02-wo-language.md) component list:

| Component | New work |
| --- | --- |
| Native wire codec | Binary framing, opcode dispatch, session lifecycle |
| Postgres-wire compatibility (optional) | libpq protocol v3 parser — reuse clients |
| GraphQL layer | SDL generation from `.wo`, resolver dispatch, `graphql-ws` subscriptions |
| REST/SSE gateway (optional) | Thin translation to native protocol |
| Subscription registry | Indexed by subscription shape; matched on commit |
| Push writer pool | io_uring-backed, per-connection outbound queues, back-pressure policy |
| TLS / auth | rustls or boringssl, JWT/mTLS at connection open |
| Connection manager | Accept, keepalive, graceful shutdown, fd limits |
| Observability | Per-session stats, slow query log, subscription lag, push-queue depth |

Rough incremental effort on top of Phase 2: **6–12 months** for a production-quality client layer with both native and GraphQL protocols, assuming the engine underneath is working.

## Why This Matters for E-commerce

Every hot user-facing screen is a subscription in disguise:

| Screen | Subscription |
| --- | --- |
| Product page | `productUpdated(id)` — price/stock changes reflect instantly |
| Cart | `inventoryChanged(sku_in: cartSkus)` — "out of stock!" appears the moment it's true |
| Order status | `orderStatus(orderId)` — pending → paid → shipped, no refresh |
| Admin dashboard | `LIVE SELECT COUNT(*) FROM orders WHERE placed_at > NOW() - 1h` |
| Recommendations sidebar | `LIVE MATCH (me)-[:VIEWED]->-[:SIMILAR_TO]->(p)` |
| Seller notifications | `LIVE MATCH (order)-[:CONTAINS]->(p) WHERE p.seller_id = $me` |

Each of these is `O(1)` server work per commit — the matching subscription is indexed by the thing that changed. Without subscriptions, every one of those screens would be a polling loop hammering the database. With subscriptions, server load scales with **actual state change**, not with client count × poll rate.

That is the whole argument for building the subscription engine into the database rather than bolting a message bus onto the side: **the engine already knows when something committed. Publishing the delta is a function call, not another system.**
