# Phase 2 — The `.wo` Language & ACID Engine

> A two-layer multi-paradigm language for an e-commerce platform with ACID transactions across relational, document, and graph storage.

**Previous**: [Phase 1 — Database Evaluation](./01-evaluation.md) | **Next**: [Phase 3 — In-Memory Engine](./03-inmemory-engine.md) | **Index**: [database.md](../database.md)

---

## Creating Your Own Runtime Language for Database

- language parser
- file saved as `database.wo`

Early sketch of the schema — three paradigms in one file:

```wo
##sql
#users
    id   bigint
    name varchar

#article
  title varchar
  meta  article-meta

##doc
#article-meta

##graph
#context-map
    ##sql-article
```

queries (relational)

```wo
SELECT name FROM users

SELECT meta.sys_title FROM article
```

queries (graph)

```wo
MATCH (a:article), (b:article)
WHERE a.meta.sys_title = "Alice" AND b.meta.sys_title = "Bob"
CREATE (a)-[:DEFINES]->(b)
```

That three-paradigm sketch is **the execution substrate** — the thing the engine actually parses and runs. But it is not the right *authoring* surface for a full-stack platform. The language is designed as two layers, described next.

## The Two-Layer Design

```
┌────────────────────────────────────────────────────────┐
│  Schema Layer  —  unified type DSL (SAP-CDS-style)     │   ← source of truth
│  type User { ... }  /  type Purchase link … / policy … │   (Phases 5 + 6)
└──────────────────────┬─────────────────────────────────┘
                       │  compiled to ↓
┌──────────────────────▼─────────────────────────────────┐
│  Query Layer  —  hybrid SQL + Cypher with fixed glue   │   ← execution substrate
│  SELECT / UPDATE / MATCH / CREATE / BEGIN … COMMIT     │   (Phase 2 prototype)
└────────────────────────────────────────────────────────┘
```

Both layers are `.wo` files — same extension, same tooling, same parser front-end. They differ in role:

- The **schema layer** names the data model once. One `type` declaration per entity covers what the three paradigm blocks cover today (relational columns, embedded documents, graph edges) plus constraints, computed fields, policies, and triggers. It is the source of truth for codegen ([Phase 5](./05-go-sdk.md)) and for the full-stack blocks ([Phase 6](./06-lowcode-fullstack.md)).
- The **query layer** is the operational surface. SQL and Cypher stay as-is — they are universally legible, every backend developer already reads them — but five things are tightened so the three grammars share semantics (parameters, `RETURNING`, dotted paths, transactions, `LIVE`).

The two layers ship on different timelines. The query layer is Phase 2 (already prototyped at [`prototypes/wo-db/`](../../../prototypes/wo-db/)). The schema layer enters when Phase 5 codegen needs a single authoritative input.

### Why Not One Layer?

Two alternatives were considered and rejected:

- **Unified query language only** (EdgeQL-style path algebra replacing SQL and Cypher). Loses the Phase 2 adoption property — SQL+Cypher are universally legible; a novel path language is not. Reinvents 6+ years of EdgeDB planner work.
- **Three paradigm blocks only** (the original sketch). Works for Phase 2 but hits a wall at Phase 5: the SDK codegen has to invent a schema-above-schema layer anyway, because "a relational row with an embedded doc column plus an inverse graph edge" is one Go struct, not three. Also hits a wall at Phase 6: `##ui` / `##policy` / `##logic` want to attach to *entities*, not to tables-vs-collections-vs-edges.

The two-layer split captures the Phase 2 adoption win and the Phase 5/6 coherence win without committing to a single novel query grammar.

## Schema Layer — Unified Type DSL

One `type` construct describes an entity. The compiler chooses physical storage (relational page, document LSM, graph node) from the field declarations.

```wo
type User {
  id:        Id
  email:     Email         @unique
  meta:      { name: Text, avatar: Url? }         -- inline document
  friends:   multi User    @edge(:FOLLOWS)        -- anonymous edge (tag only)
  purchased: multi Product via Purchase           -- link type with props
  orders:    backlink Order.user                  -- inverse scalar ref
}

type Purchase link User -> Product {              -- edge with properties
  order: ref Order
  qty:   Int               @check(> 0)
  at:    Timestamp = now()
}

type Order {
  id:         Id
  user:       ref User
  status:     Pending | Paid | Shipped | Refunded    -- tagged union
  line_items: [{ product: ref Product, qty: Int, unit: Money }]
  total:      Money = sum(line_items.*.qty * line_items.*.unit)    -- computed
}

type Product {
  id:        Id
  sku:       SKU           @unique
  price:     Money
  meta:      { title: Text, description: Markdown, images: [Url], reviews: [Review] }
  inventory: { on_hand: Int @check(>= 0), reserved: Int = 0, reorder_at: Int }
}

type Review {
  user:       ref User
  stars:      Int @check(between 1 and 5)
  body:       Markdown
  written_at: Timestamp = now()
}
```

**Type-system primitives:**

| Primitive | Example | Compiles to |
| --- | --- | --- |
| Scalar | `Int Float Text Bool Timestamp Id Url Email Markdown Money SKU Slug` | relational column |
| Optional | `Url?` | nullable column |
| Array | `[Url]` | relational array or doc array |
| Struct | `{ k: V, ... }` | embedded document column (doc engine) |
| Scalar ref | `ref User` | foreign-key column |
| Zero-prop edge | `multi User @edge(:FOLLOWS)` | graph edge, no properties |
| Link-with-props | `multi Product via Purchase` | graph edge + linked row (`type Purchase link ...`) |
| Inverse | `backlink Order.user` | computed inverse of `Order.user: ref User` |
| Tagged union | `Pending \| Paid \| Shipped` | enum column |
| Computed | `total: Money = sum(...)` | view or materialized view |

**Annotations:** `@unique @check(...) @default(...) @index @search @immutable`.

**Full-stack blocks attach to types** — no separate `##ui`/`##logic`/`##policy`/`##service` paradigm markers. See [Phase 6](./06-lowcode-fullstack.md) for details; shape:

```wo
type Article {
  slug:   Slug @unique
  title:  Text
  body:   Markdown
  author: ref User

  policy read  anyone
  policy write when author == $session.user

  on update when old.status != "published" and new.status == "published"
     do emit "article.published"(self)

  service rest "/articles" expose list, get, subscribe
}
```

## Query Layer — Hybrid SQL + Cypher, Fixed Glue

Keep the syntax developers already know. Fix five things so the three grammars share semantics:

1. **One parameter rule.** `$name` everywhere — SQL, Cypher, document path expressions. Typed at prepare time from the surrounding function signature or session context.
2. **Cross-paradigm `RETURNING`.** `INSERT … RETURNING id AS oid` binds the alias into subsequent statements in the same `BEGIN … COMMIT`. Replaces `LAST_INSERT_ID()`.
3. **One path rule.** `a.b.c[i].d` reads the same inside SQL expressions, document `UPDATE … SET`, and Cypher projections (`RETURN u.meta.name`).
4. **One transaction block.** `BEGIN [SNAPSHOT|SERIALIZABLE] … [SAVEPOINT name; …] … COMMIT|ROLLBACK [TO name]`. No dialect split for stored procedures.
5. **One subscription prefix.** `LIVE <select|match>` returns a subscription handle. Same semantics on both sides.

Every e-commerce query mixes at least two paradigms:

```wo
-- Relational + document: "products under $50 with >4 stars"
SELECT id, meta.title, price_cents
FROM products
WHERE price_cents < 5000
  AND AVG(meta.reviews[].stars) > 4.0;

-- Graph + relational: "top sellers among products similar to what I bought"
MATCH (me:user {id: $uid})-[:PURCHASED]->(p:product)-[:SIMILAR_TO]->(rec:product)
WHERE rec.inventory.on_hand > 0
ORDER BY rec.meta.reviews.count DESC
LIMIT 20;

-- All three: atomic checkout, with RETURNING replacing LAST_INSERT_ID()
BEGIN SNAPSHOT
    UPDATE products
        SET inventory.on_hand = inventory.on_hand - $qty,
            inventory.reserved = inventory.reserved + $qty
        WHERE id = $pid AND inventory.on_hand >= $qty
        RETURNING id AS pid;

    INSERT INTO orders (user_id, total_cents, status, line_items)
        VALUES ($uid, $total, 'pending',
                [{product_id: $pid, qty: $qty, unit_cents: $unit}])
        RETURNING id AS oid;

    MATCH (u:user {id: $uid}), (p:product {id: $pid})
        CREATE (u)-[:PURCHASED {order_id: $oid, qty: $qty, at: now()}]->(p);
COMMIT;

-- Subscription: same predicate language, LIVE prefix
LIVE SELECT id, status, total_cents FROM orders WHERE user_id = $uid;
```

The checkout query is the whole argument. Those three statements **must** commit together or not at all. `RETURNING id AS oid` threads the inserted order's id into the Cypher `CREATE` without inventing a special function call. No off-the-shelf system executes that atomically across SQL + JSONB + a graph store without stitching multiple transaction managers together (or adopting SurrealDB, which is the existence proof that this can be built).

## Option C — Full `.wo` Language for an E-commerce Platform with ACID

The [evaluation phase](./01-evaluation.md) argued against external databases for a small, single-writer content project. An **e-commerce platform inverts every one of those assumptions**:

| writeonce (blog) | E-commerce platform |
| --- | --- |
| Hundreds of articles | Millions of products, orders, sessions |
| Single writer (author) | Thousands of concurrent writers (customers, fulfillment, admin) |
| Read-heavy | Write-heavy on the hot paths (cart, checkout, inventory) |
| Full rebuild on change is free | Full rebuild is impossible — mutations must commit in milliseconds |
| No transactions needed | ACID is the product |
| Data is one shape (article) | Data is genuinely three shapes: relational (orders, inventory), document (product descriptions, reviews), graph (recommendations, categories, affiliations) |

The three-paradigm case that was marginal for a blog becomes **the correct design** for e-commerce. And ACID is not a nice-to-have — an order that decrements inventory but loses the payment record is a lawsuit.

This is Option C: a full `.wo` language backed by a real storage engine, a real transaction manager, and a real planner. It is a database, and writeonce/the parent project becomes the vehicle for building it.

### ACID — What Each Letter Requires

**Atomicity.** The checkout above touches three storage areas. The engine needs a single transaction coordinator that owns writes to all three. On abort, every partial write rolls back. Implementation: one **write-ahead log (WAL)** records intents across all paradigms; commit flips a single on-disk marker; crash recovery replays or discards based on the marker.

**Consistency.** Domain invariants that span paradigms must hold:
- `inventory.on_hand >= 0` (document field, relational-style constraint — expressed as `@check(>= 0)` in the schema layer)
- Every `PURCHASED` edge must reference an existing `orders.id` (graph-to-relational FK — enforced because `Purchase.order: ref Order` in the schema layer)
- `orders.total_cents == sum(line_items[].qty * line_items[].unit_cents)` (denormalization check — expressed as a computed field)

The schema layer captures these as declarations; the compiler emits the planner-level checks. The query layer also supports explicit `CONSTRAINT` statements for invariants that don't fit the type system.

**Isolation.** Thousands of concurrent carts racing for the last unit of inventory. Two realistic models:

| Model | How it works | Trade-off |
| --- | --- | --- |
| **MVCC** (Postgres, SurrealDB, CockroachDB) | Each transaction sees a snapshot; conflicts detected at commit | Readers never block writers; abort rate rises under contention |
| **2PL with row/edge locks** (MySQL InnoDB) | Locks acquired on read/write, released at commit | Lower abort rate; deadlocks must be detected |

MVCC is the modern default and what you'd target. Minimum isolation level for e-commerce: **Snapshot Isolation** (Postgres `REPEATABLE READ`). Anything weaker (`READ COMMITTED`) permits write skew — two customers each passing the "inventory >= 1" check and both succeeding on the last unit.

**Durability.** On `COMMIT`, the WAL record must be `fsync`'d before the client gets acknowledgment. Lose fsync and you lose paid orders on power failure. The WAL is the primary storage commitment; data files are derived and can be rebuilt by replaying the log from the last checkpoint.

### Storage Engine

`.seg` + `.idx` rebuilt-on-change does not survive contact with e-commerce. The engine needs genuine mutable on-disk structures:

| Component | Responsibility | Reference |
| --- | --- | --- |
| **WAL** | Ordered, fsynced log of every committed mutation | Postgres `pg_wal/`, RocksDB `*.log` |
| **Relational pages** | Fixed-size pages (4–16 KB) with slot-directory row layout, B+ tree indexes | Postgres heap + btree, SQLite |
| **Document store** | LSM tree (SSTables + memtable + compaction) for append-friendly JSON blobs | RocksDB, SurrealKV |
| **Graph store** | Adjacency list on disk — doubly-linked edge records per node for O(1) traversal | Neo4j's native store |
| **Buffer pool** | Shared page cache with LRU/CLOCK eviction, dirty page tracking | Postgres `shared_buffers` |
| **Checkpointer** | Periodically flushes dirty pages, truncates WAL | Every major DB has one |
| **Vacuum / compaction** | Reclaim space from MVCC dead tuples / LSM tombstones | Postgres autovacuum, RocksDB compaction |

Three storage backends, one WAL, one transaction coordinator. That is the core of the project. The [In-Memory Engine](./03-inmemory-engine.md) phase details the RAM-primary variant of this.

### Cross-Paradigm Transaction Coordinator

The novel piece — nobody ships this exactly the way `.wo` would need it:

```
         ┌──────────────────────────────────────┐
         │         .wo Query Planner            │
         └──────────────────────────────────────┘
                         │
                         ▼
         ┌──────────────────────────────────────┐
         │   Transaction Coordinator (MVCC)     │
         │   - txn_id allocation                │
         │   - snapshot timestamp               │
         │   - commit ordering                  │
         │   - WAL append + fsync               │
         │   - RETURNING alias table per txn    │
         └──────────────────────────────────────┘
           │              │              │
           ▼              ▼              ▼
      ┌────────┐    ┌──────────┐   ┌──────────┐
      │ Rel    │    │ Doc      │   │ Graph    │
      │ Engine │    │ Engine   │   │ Engine   │
      │ (B+)   │    │ (LSM)    │   │ (adj)    │
      └────────┘    └──────────┘   └──────────┘
```

Every engine exposes the same transaction hooks: `begin(snapshot_ts)`, `stage(mutation)`, `prepare()`, `commit(wal_lsn)`, `abort()`. The coordinator drives a **two-phase commit internally** (not distributed 2PC — it's one process, so prepare+commit is cheap and deterministic).

Snapshot read across paradigms: each engine stores per-record `(created_txn_id, deleted_txn_id)` visibility info. A read at snapshot timestamp `T` sees only records visible at `T` — same rule in all three engines.

`RETURNING` aliases live in the transaction's scoped name table; each subsequent statement inside the same `BEGIN … COMMIT` resolves `$oid` etc. against it. This is how SQL results thread into Cypher `CREATE` atomically, without round-tripping to the client.

### Concurrency Model

**One process, one thread, one event loop.** Redis-style. The entire engine — connection accept, parser, planner, executor, buffer pool, subscription registry — runs on a single userland thread; the only non-userland thread is the kernel-owned io_uring SQPOLL helper, which is invisible to engine code.

| Subsystem | Where it runs |
| --- | --- |
| Connection accept | Event loop — non-blocking `accept()` via io_uring |
| Query execution | Event loop — parse, plan, execute inline |
| WAL fsync | Event loop submits SQEs to io_uring; kernel-owned SQPOLL thread drains; loop parks on CQE |
| Subscription dispatch | Event loop — predicates matched on commit, deltas pushed to per-subscription ring buffers |
| Compaction / checkpoint / vacuum | Event loop — scheduled as low-priority tasks between client work |

**Why single-threaded.** Three precedents:

- **Redis** ran single-threaded for its first decade (and still runs command execution single-threaded; 6.0 added threaded I/O only). It hits hundreds of thousands of ops/second on one core.
- **TigerBeetle** is single-threaded by design because determinism beats concurrency for financial workloads.
- **Node.js** is the proof that the event-loop model scales for I/O-bound work at web-scale.

Single-threaded execution removes entire failure modes: no lock ordering, no cross-thread memory ordering hazards, no MVCC visibility logic for reads racing with writes, no torn-page atomics. The transaction coordinator trivially serializes commits because there is only one of them at a time. Snapshot isolation reduces to a logical versioning scheme for live-query delta computation — not a multi-threaded correctness mechanism.

**Group commit still applies.** The loop drains many pending commits into one fsync SQE per tick — same amortization as Postgres's group-commit path, without a dedicated WAL-writer thread. Expected throughput on a modern NVMe server: ~200–500k simple-transaction commits per second per core. More than enough for every writeonce-sized workload.

**Scaling past one core.** The path is **sharding**: partition types across independent single-threaded engine processes (Redis Cluster is the reference). Each shard owns a disjoint set of types; cross-shard transactions use two-phase commit between shards. Revisit only when a production workload actually saturates the core — the multi-threaded single-engine alternative is years of work for the wrong kind of gain.

### Query Language Scope (`.wo` Full Spec)

The minimum grammar for an e-commerce workload is split by layer:

**Schema layer (enters at Phase 5):**

- `type Name { ... }` declarations with scalar fields, embedded structs, `ref`, `multi @edge`, `multi … via LinkType`, `backlink`
- `type Name link A -> B { ... }` for edge-with-properties
- Tagged unions `A | B | C`
- Annotations `@unique @check @default @index @search @immutable`
- Computed fields `total: Money = sum(…)`
- Per-type `policy` / `on <event>` / `service` blocks (full semantics in [Phase 6](./06-lowcode-fullstack.md))

**Query layer (Phase 2 prototype):**

- **DDL (interim)**: the existing `##sql / ##doc / ##graph` block syntax, as the compiler target for the schema layer and the prototype's direct authoring surface
- **DML relational**: `INSERT`, `UPDATE`, `DELETE`, `UPSERT` — all supporting `RETURNING col AS alias`
- **DML document**: path updates (`SET meta.reviews[3].stars = 5`), array operations (append, remove, splice)
- **DML graph**: `CREATE`, `MERGE`, `DELETE` on nodes and edges; variable-length path (`*1..5`)
- **Queries**: `SELECT` with joins, `MATCH` with traversal, subqueries, aggregations (`COUNT`, `SUM`, `AVG`)
- **Transactions**: `BEGIN [SNAPSHOT|SERIALIZABLE]` / `COMMIT` / `ROLLBACK`, `SAVEPOINT name` / `ROLLBACK TO name`
- **Parameters**: `$name` everywhere, typed at prepare
- **Cross-paradigm expressions**: dotted paths descend relational → document (`order.line_items[0].qty`); graph bindings resolve to relational rows (`(u:user {id: $uid})`); `RETURNING` aliases resolve across SQL → Cypher boundary
- **Subscriptions / live queries**: `LIVE SELECT` / `LIVE MATCH` — SurrealDB-style; predicate known at registration time for O(1) lookup matching on commit
- **Prepared statements + parameter binding**: `$name` with typed signatures; required for SQL injection resistance
- **Role-based auth + row-level policies**: schema-layer `policy` declarations compile to planner rewrite rules AND'd into every query at registration time

### Components to Build

```
.wo engine
├── parser              Pratt or LALR — handles both layers and all 3 paradigms
├── analyzer            name resolution, type checking, cross-paradigm dispatch
├── schema compiler     type-DSL → physical schema (Phase 5)
├── planner             cost-based: reorder joins, push predicates, choose index,
│                       merge policy predicates at registration time
├── executor            vectorized over relational, iterator over graph
├── txn manager         MVCC, snapshot isolation, group commit, RETURNING aliases
├── wal                 ordered log, fsync, checkpoint, recovery
├── rel engine          B+ tree heap, btree indexes, vacuum
├── doc engine          LSM with bloom filters, compaction
├── graph engine        native adjacency, property store
├── buffer pool         shared page cache, dirty tracking
├── catalog             schema metadata (from type DSL), evolvable at runtime
├── wire protocol       client connections (pick: Postgres wire, or custom)
├── auth + rbac         users, roles, row-level policies from type declarations
├── live queries        incremental view maintenance for subscriptions
├── backup / replication  physical log shipping; logical streaming for read replicas
└── observability       query stats, slow log, lock waits, WAL lag
```

Implementation language is genuinely open. Reasonable picks and what each implies:

| Language | Why | Precedent |
| --- | --- | --- |
| **Rust** | Memory safety without GC, good async story, zero-cost abstractions, fits writeonce's existing stack | SurrealDB, TiKV, Materialize, sled |
| **C++** | Lowest overhead, decades of mature DB internals literature | Postgres (C), MySQL, RocksDB, DuckDB, ClickHouse |
| **Zig** | C-like control with safer semantics, compile-time metaprogramming useful for query planner | TigerBeetle |
| **Go** | Fastest to productive, excellent concurrency primitives, some GC cost on hot paths | CockroachDB, InfluxDB, Dgraph |
| **OCaml / Haskell** | Query planner is a compiler; ML-family languages are excellent at compilers | Irmin, some research DBs |

For e-commerce ACID specifically, **Rust or C++** — GC pauses during a checkout fsync batch are the kind of latency spike that loses money. Go works and has the fastest developer velocity, but CockroachDB has spent years tuning around GC; budget for that.

### Reference Implementations to Study

- **SAP CDS** — <https://cap.cloud.sap/docs/cds/>. The canonical declaration-first application language; direct inspiration for the schema layer.
- **EdgeDB / EdgeQL** — path-based query language over a typed schema; studies the corner cases of computed fields, link properties, and tagged unions. Worth reading their planner learnings before re-implementing.
- **Postgres** — the canonical ACID RDBMS. Read `src/backend/access/transam/` for WAL, `src/backend/storage/buffer/` for the buffer pool, `src/backend/storage/lmgr/` for locking. Decades of battle-tested code.
- **SurrealDB** — closest living example of the query layer. Multi-model, multi-paradigm query language, Rust, embeddable. Already referenced in [surreal-case-study.md](../surreal-case-study.md).
- **SQLite** — smallest complete ACID database in the open-source world. `src/btree.c`, `src/pager.c`, `src/wal.c` are worth reading front-to-back.
- **CockroachDB** — distributed SQL with serializable isolation. Go, but the transaction protocol (Parallel Commits) is well-documented.
- **TigerBeetle** — financial-grade ACID, deterministic, written in Zig. Essay on why they rewrote: <https://tigerbeetle.com/blog/>.
- **DuckDB** — analytics-focused but single-file embeddable C++ engine, excellent reference for a modern vectorized executor.
- **Neo4j** — for the graph-storage side. Native adjacency, transaction log, lock manager.
- **Papers**:
  - *Architecture of a Database System* (Hellerstein, Stonebraker, Hamilton, 2007) — the canonical survey
  - *The Log-Structured Merge-Tree* (O'Neil 1996) — for the LSM document backend
  - *Serializable Snapshot Isolation in PostgreSQL* (Ports & Grittner 2012) — making SI safe
  - *A Critique of ANSI SQL Isolation Levels* (Berenson et al. 1995) — so you pick the right default

### Realistic Scope

This is a multi-person-year project. Concrete gates:

| Milestone | What's usable | Rough effort |
| --- | --- | --- |
| Parser + analyzer + in-memory executor | Single-user prototype, no durability (**shipped** at `prototypes/wo-db/`) | 2–4 months |
| Fixed-glue query layer (`$name`, `RETURNING`, `BEGIN/SAVEPOINT/COMMIT`, `LIVE` keyword) | Multi-statement cross-paradigm transactions threadable | +1–2 months |
| WAL + crash recovery + single-table B+ tree | ACID on relational only, one writer | +3–6 months |
| MVCC + concurrent transactions | Multi-writer relational | +3–6 months |
| Document engine (LSM) | Relational + document, transactional | +4–8 months |
| Graph engine + cross-paradigm txns | All three paradigms ACID | +6–12 months |
| Schema-layer compiler (type DSL → physical schema) | Single source of truth for codegen and full-stack blocks | +2–4 months |
| Wire protocol + RBAC + observability | Deployable to production | +3–6 months |
| Replication + backup | Survive a node loss | +6–12 months |

Two to four years for a small team to reach something a real e-commerce business would trust with payment data. Adopting Postgres (with JSONB for the document side and the Apache AGE extension or a separate graph store for the graph side) gets you there in a week.

### Honest Decision Framing

Build `.wo` for an e-commerce platform **only if** at least one of the following is true:

1. The cross-paradigm query atomicity is a business-critical feature the founders want to sell ("our DB does what Postgres + Neo4j glued together cannot"). This is the SurrealDB and EdgeDB thesis.
2. Building the database **is** the product — the e-commerce platform is the test harness, not the goal.
3. You have a team comfortable with the papers listed above and the patience to ship a toy for 18 months before it's useful.

Otherwise, the pragmatic stack for a multi-paradigm ACID e-commerce platform is:

- **Postgres** for relational + document (JSONB) + row-level security. Handles 99% of the workload. Apache AGE extension adds Cypher-compatible graph queries in the same transaction.
- **Redis** for cart/session/rate-limit (expiring, non-durable).
- **Search** (OpenSearch/Meilisearch/Typesense) for product search — specialized workload.
- **Event log** (Kafka/Redpanda) for order events, downstream analytics, fulfillment.

That stack is boring and it works. `.wo` as described is interesting and would take years. Pick based on whether the goal is to ship e-commerce or to ship a database.
