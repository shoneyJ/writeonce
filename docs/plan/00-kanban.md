# Plan kanban — backend: runtime, database, API

Status board for every phase doc under `docs/plan/`. **Focus: the backend** — the runtime, the database engine, and the REST/`.wo`-language API. Frontend phases are parked, not deleted. Each phase file carries a matching status banner; this board is the index.

Statuses: ✅ **done** · 🔄 **in progress** · ⬜ **not started** · ⏸ **parked (out of backend focus)**

## Board

### Track 1 — Runtime foundations (dependency removal, kernel primitives)

| Status | Phase | Doc | Notes |
| --- | --- | --- | --- |
| ✅ | 01 crate scaffolding | [done/01](done/01-scafolding-crates.md) | 15 crates |
| ✅ | 02 epoll event loop | [done/02](done/02-event-loop-epoll.md) | `runtime/netpoll_epoll.rs` |
| ✅ | 03 hand-rolled HTTP | [done/03](done/03-hand-rolled-http.md) | + keep-alive & pipelining (follow-up under plan 09) |
| ✅ | 04 tokio/axum cutover | [done/04](done/04-cutover-remove-tokio-axum.md) | deps now: anyhow, serde, serde_json, libc |
| ⬜ | 05 hand-rolled JSON | [05](05-hand-rolled-json.md) | removes serde/serde_json — **next in this track** |
| ⬜ | 06 bespoke error type | [06](06-bespoke-error.md) | removes anyhow |
| ⬜ | 07 inotify content watcher | [07](07-inotify-content-watcher.md) | `wo dev` hot reload |
| ⬜ | 08 sendfile static assets | [08](08-sendfile-static-assets.md) | needed by parked UI track too |

### Track 2 — Concurrency & scale-out (plan 09) — 🔄 in progress

| Status | Sub-phase | Notes |
| --- | --- | --- |
| ✅ | 09a thread-per-core | `runtime/scheduler.rs`, `SO_REUSEPORT`, pinned `wo-shard-<t>` workers |
| ✅ | 09b sharded engine | `shard.rs` bus; `Arc<Mutex<Engine>>` deleted; interleaved ids; fan-out lists |
| ✅ | 09c per-shard WAL | `wal.rs`; ack-after-fsync; boot replay; `meta` shard guard |
| ✅ | — keep-alive (follow-up) | reads ×3.4 → 770k/s; C phase-C sequence |
| ✅ | — io_uring group commit (follow-up) | `netpoll_io_uring.rs` raw ring; 4.7× durable writes on real disk |
| ⬜ | 09d cross-shard subscriptions | LIVE fan-out, one message per shard — pairs with Stage 3 |
| ⬜ | 09e cross-shard transactions (2PC) | needed by `fn checkout` spanning shards |
| ⬜ | 09f observability & reshard | per-shard metrics, `WO_RESHARD` |

All numbers + find-and-fix stories: [09-concurrency-scaleout.md](09-concurrency-scaleout.md) shipped notes and the [benchmark table](../../prototypes/wo-rt-c/README.md).

### Track 3 — Storage & durability (plans 10–12)

| Status | Phase | Doc | Notes |
| --- | --- | --- | --- |
| ⬜ | 10 storage foundations | [10](10-storage-foundations.md) | scope reduced: WAL framing/fallocate landed via 09c |
| ⬜ | 11 WAL & recovery | [11](11-wal-and-recovery.md) | remaining: snapshots (`.data`), compaction, WAL rotation — replay core shipped in 09c |
| ⬜ | 12 engine disk cutover | [12](12-engine-disk-cutover.md) | mmap arena engine (C phase B is the proving ground) |

### Track 4 — Language & API (`.wo` on the wire) — 🔄 in progress

| Status | Phase | Doc | Notes |
| --- | --- | --- | --- |
| ✅ | 13a class surface | [13](13-class-model-live-pricing.md) | `class` parses, CRUD serves, spec amended |
| ✅ | 13b method execution | [13](13-class-model-live-pricing.md) | `POST /api/<t>/:id/<method>`; row-scoped txn; one `WalRec::Txn` frame per call; abort → 409 rollback |
| ⬜ | 13c LIVE pricing push | [13](13-class-model-live-pricing.md) | Stage 3 scoped: subscription registry, WS at `/api/<t>/live`, replaces the 501 stub |
| ⬜ | Stage 3 wire layer | [../runtime/database/04-client-api.md](../runtime/database/04-client-api.md) | full subscription engine + wire protocol; 13c is its beachhead |
| ⬜ | 13e pricing at scale | [13](13-class-model-live-pricing.md) | wires demo to 09; hot-row reads |
| ⬜ | 15a MCP core + tools | [15](15-mcp-streamable-http.md) | JSON-RPC 2.0 on `POST /mcp`; catalog-generated tools; durable-ack gated; stateless |
| ⬜ | 15b MCP resources | [15](15-mcp-streamable-http.md) | `wo://` URIs, templates, cursors |
| ⬜ | 15c MCP SSE + sessions | [15](15-mcp-streamable-http.md) | first streaming response in `rt`; `Mcp-Session-Id`; GET stream |
| ⬜ | 15d `service mcp` surface | [15](15-mcp-streamable-http.md) | `ServiceKind::Mcp`; 13b methods become tools |
| ⬜ | 15e MCP live subscriptions | [15](15-mcp-streamable-http.md) | `resources/subscribe` + `Last-Event-ID` replay — needs 13c + 09d |

Ecommerce sample status (verified 2026-06-13, `api.rest` **17/17 expected statuses pass**): route wiring, empty lists, 405 for un-exposed ops, 404 for unregistered routes, 501 Stage-3 stubs — all exactly as documented. `fn checkout`, `on startup` seeding, and status-lifecycle triggers await 13b-style execution + 09e.

### Track 5 — C proving ground — ✅ done (A–F)

| Status | Phase | Doc |
| --- | --- | --- |
| ✅ | A threads · B arena · C io_uring · D WAL · E recovery · F bench+ACID | [exploration/c-runtime/00-plan.md](exploration/c-runtime/00-plan.md) |

859k reads/s, 618k durable commits/s; found the ack-ordering + fd-ABA bugs the Rust port then avoided. Optional phase G (splice `wo-db` C++ engine) remains an idea.

### Track 6 — Frontend — ⏸ parked (out of backend focus)

| Status | Phase | Doc | Notes |
| --- | --- | --- | --- |
| ⏸ | 13d pricing UI | [13](13-class-model-live-pricing.md) | MVC triplet exists as design artifact |
| ⏸ | 14 MVC UI implementation (14a–f) | [14](14-mvc-ui-implementation.md) | htmlx engine, SCSS subset, controllers, SSR, actions, live patching |
| ⏸ | UI exploration track | [exploration/ui/00-overview.md](exploration/ui/00-overview.md) | 00–08 design docs stay current |

## Suggested order of play (backend)

1. ~~**13b — method execution**~~ ✅ shipped — methods run as row-scoped transactions over RPC
2. **13c / Stage 3 LIVE** with **09d** fan-out (turns every 501 stub real; the ecommerce order-ops board's backend)
3. **09e — 2PC** (cross-shard `fn checkout` — the canonical ACID demo end-to-end; 13b's single-shard txn is its building block)
4. **15a/15b — MCP core, tools, resources** (needs nothing unshipped; makes every app agent-callable — 13b methods become MCP tools in 15d; 15e waits on 13c + 09d)
5. **05/06** dependency removal (mechanical, any time)
6. **10–12** storage completion (snapshots/compaction; arena engine)
