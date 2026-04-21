# Document & Graph Database — Design Series

A seven-phase design series that starts with "should writeonce use a document or graph database?" and arrives at a full-stack declarative application platform — then plans the migration from writeonce's current flat-file store to that platform.

> **Start here if you're new:** [wo-language.md](./wo-language.md) — the user-facing overview of what writeonce *is* (a programming language with DB + HTTP in its runtime, Go-style toolchain). This series is the engineering plan that gets you there.

Each phase is self-contained and shippable on its own. Every phase after Phase 1 builds on the previous ones.

## Phases

| Phase | Document | Summary |
| --- | --- | --- |
| **1** | [Database Evaluation](./database/01-evaluation.md) | Evaluate CouchDB, Postgres, Neo4j, petgraph against writeonce's needs. Decision: no external DB; add petgraph-backed `mappings.idx`. |
| **2** | [The `.wo` Language & ACID Engine](./database/02-wo-language.md) | Design a two-layer `.wo` language (unified `type` schema layer + hybrid SQL/Cypher query layer with fixed glue) for an e-commerce platform with ACID transactions across relational, document, and graph storage. |
| **3** | [In-Memory Engine](./database/03-inmemory-engine.md) | RAM-primary, SSD-durable storage engine using `io_uring`, `mlockall`, group commit. 64 GB Linux machine. |
| **4** | [Client API: Wire Protocol & Subscriptions](./database/04-client-api.md) | Native binary protocol + GraphQL over WebSocket. Subscription engine inside the transaction coordinator — no polling anywhere. |
| **5** | [Go Client SDK](./database/05-go-sdk.md) | Typed Go client with subscription-first design. `gen` codegen from `.wo` schema. Subscribe to a live query in 5 lines. |
| **6** | [Low-Code Full-Stack](./database/06-lowcode-fullstack.md) | Expand `.wo` into an application language (like SAP CDS): `##ui`, `##logic`, `##policy`, `##service` blocks compiled into a single binary. |
| **7** | [Replacing `wo-seg`](./database/07-wo-seg-migration.md) | Phased coexistence plan: abstract the article store behind a trait, stand up the `.wo` engine as a second impl, dual-run, cut over, decommission `wo-seg`. |

## Build Order

```
Phase 1: Evaluation          ← writeonce today (blog, flat files)
    │
    ▼
Phase 2: .wo Language         ← query language + ACID engine design
    │
    ▼
Phase 3: In-Memory Engine     ← RAM-primary storage, io_uring, WAL
    │
    ▼
Phase 4: Client API           ← wire protocol, subscriptions, GraphQL
    │
    ▼
Phase 5: Go SDK               ← typed client, codegen, subscription channels
    │
    ▼
Phase 6: Low-Code Full-Stack  ← .wo as application DSL, UI generation, CLI
    │
    ▼
Phase 7: Replace wo-seg       ← trait abstraction, dual-run, cutover, decommission
```

## Cumulative Scope

| After Phase | What exists | Rough cumulative effort |
| --- | --- | --- |
| 1 | petgraph `mappings.idx` in writeonce | ~200 lines |
| 2 | `.wo` parser + planner + ACID engine prototype | 5–10 months |
| 3 | In-memory engine with io_uring durability | 8–16 months |
| 4 | Wire protocol + subscription engine | 14–28 months |
| 5 | Go SDK with typed subscriptions | 16–32 months |
| 6 | Full-stack low-code platform | 28–56 months |
| 7 | `wo-seg` replaced by the `.wo` engine in writeonce | +2–4 months on top of Phase 2 arrival |

## Related Documents

- [wo-language.md](./wo-language.md) — writeonce as a programming language: toolchain, hello-world, stdlib, client model
- [surreal-case-study.md](./surreal-case-study.md) — SurrealDB runtime analysis; why writeonce does not use a multi-model DB for live queries
- [async.md](./async.md) — custom async runtime using Linux kernel primitives
- [05-datalayer.md](../05-datalayer.md) — current `.seg` + `.idx` implementation (8 crates, 44 tests)
- [03-data.md](../03-data.md) — data layer design with subscription model
- [06-markdown-render.md](../06-markdown-render.md) — markdown-first content model
- [ai-agents-content-management.md](../future-scope/ai-agents-content-management.md) — the `mappings` feature that started this series
