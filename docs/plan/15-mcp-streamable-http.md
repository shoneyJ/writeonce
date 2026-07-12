# 15 — MCP over Streamable HTTP: every writeonce app is an MCP server

> **Kanban: ⬜ not started (Track 4 — Language & API)** — board: [00-kanban.md](00-kanban.md)

**Context sources:** [MCP specification 2025-06-18 — Transports](https://modelcontextprotocol.io/specification/2025-06-18/basic/transports) (the normative Streamable HTTP contract this plan implements, verified 2026-07-12), [`reference/mcp-python-sdk/`](../../reference/README.md) (symlink to the official MCP Python SDK — grep `src/mcp/server/streamable_http.py` + `streamable_http_manager.py` for the reference server behaviour, `src/mcp/client/streamable_http.py` for what a conforming client expects; behaviour is ported, code is not), [`../runtime/database/04-client-api.md`](../runtime/database/04-client-api.md) (the wire-protocol design; its "REST + SSE gateway" row is what this plan makes concrete for agents), [`./13-class-model-live-pricing.md`](./13-class-model-live-pricing.md) (13b methods become MCP tools; 13c's subscription registry carries 15e), [`./09-concurrency-scaleout.md`](./09-concurrency-scaleout.md) (thread-per-core + shard bus the endpoint rides; 09d fan-out gates 15e), `crates/rt/src/server.rs` + `crates/rt/src/http/` (the keep-alive HTTP layer and router this lands in).

## Context

**MCP** (Model Context Protocol) is the open JSON-RPC 2.0 protocol LLM agents use to discover and call external capabilities: **tools** (typed functions), **resources** (readable content addressed by URI), and change **notifications**. **Streamable HTTP** is its HTTP transport (spec 2025-06-18, replacing the 2024-11-05 HTTP+SSE pair): one endpoint path serving POST and GET, where every client→server JSON-RPC message is a POST, the server answers each *request* with either a single `application/json` body **or** a `text/event-stream` (SSE) that may carry server messages before the final response, a GET opens a server→client SSE stream for unsolicited notifications, and an `Mcp-Session-Id` header carries optional stateful sessions.

The fit with writeonce is unusually direct. The runtime already is the database, the schema authority, and the HTTP server in one binary; REST routes are *generated* from `type` declarations and their `service … expose` lists. MCP is the same generation problem with a different wire shape: the catalog becomes `tools/list` and `resources/templates/list`, the engine's CRUD paths become `tools/call`, and — once Stage 3/13c lands — committed deltas become `notifications/resources/updated`. A `.wo` app then serves browsers (REST/htmlx), programs (REST), and agents (MCP) from one catalog, one engine, one port.

What exists today that this plan builds on: the hand-rolled epoll HTTP layer with keep-alive and pipelining (plan 03 + the 09 follow-up), thread-local routers per `SO_REUSEPORT` worker, sharded engine with owner-hop/fan-out on the shard bus (09b), and durable ack gating on the io_uring group commit (`Response.gate` — a mutation's response is parked until its fsync CQE). What does **not** exist yet: any non-buffered response (every `Response` is a full `Vec<u8>` with `Content-Length`), sessions, and the subscription registry (13c).

## Transport contract (normative summary)

The rules the sub-phases implement, condensed from the spec — each MUST below is the spec's, not ours:

| # | Rule |
| --- | --- |
| T1 | One endpoint path (`/mcp`) MUST support POST and GET. Body of a POST is a **single** JSON-RPC message (batching is gone in 2025-06-18). |
| T2 | POSTed *request* → server returns `Content-Type: application/json` (one object) **or** `text/event-stream` (SSE stream that eventually carries the response, then SHOULD close). The server chooses; clients MUST support both. |
| T3 | POSTed *notification*/*response* → `202 Accepted`, no body (or 4xx if rejected). |
| T4 | GET → SSE stream for server-initiated messages, **or** `405 Method Not Allowed`. No JSON-RPC *responses* on a GET stream except when resuming. |
| T5 | Sessions: server MAY return `Mcp-Session-Id` on the `InitializeResult` response; clients MUST echo it on all subsequent requests; missing → `400`; terminated/unknown → `404` (client then re-initializes); client DELETE terminates a session (server MAY answer `405`). |
| T6 | `MCP-Protocol-Version` header required on post-initialize requests; absent → assume `2025-03-26`; invalid/unsupported → `400`. |
| T7 | Resumability: SSE events MAY carry `id:` (unique per stream, acting as a per-stream cursor); client reconnects with `Last-Event-ID`; server MAY replay messages from that stream only. |
| T8 | Security: server MUST validate `Origin` (DNS-rebinding defence), SHOULD bind localhost when local, SHOULD authenticate. |

## Goal

`cargo run --bin wo -- run docs/examples/blog` serves `POST /mcp` alongside `/api/*`: an MCP client (MCP Inspector, Claude Code, or a curl script) performs `initialize` → `tools/list` → `tools/call article_create` → `tools/call article_list` and sees its write — with the ack held for the fsync CQE exactly as REST does. After 15e (with 13c + 09d): `resources/subscribe` on `wo://product/1`, a `set_price` commit in another terminal, and `notifications/resources/updated` arrives on the open SSE stream — the agent-shaped twin of the 13d browser demo.

## Design decisions (locked)

1. **One endpoint, same workers.** `/mcp` is a route in the existing thread-local `Router` — no second listener, no port, no dedicated thread. It scales the way `/api/*` does: `SO_REUSEPORT` spreads connections, shard bus routes data ownership.
2. **Catalog-driven and class-blind.** Tools and resources are generated from the catalog + expose lists, never hand-registered — the 13a doctrine (storage/REST class-blind) extends to MCP. Until 15d, the existing `service rest … expose` list governs what MCP exposes; 15d adds `service mcp` for independent control.
3. **JSON first, streaming second.** 15a–15b answer every POSTed request in `application/json` mode — spec-legal per T2 — so the MCP surface is useful before any streaming machinery exists. SSE (T2's other arm, T4, T7) is additive in 15c/15e.
4. **The durable-ack rule is transport-independent.** A `tools/call` that mutates parks its JSON-RPC response on the group-commit gate exactly like a REST POST (`Response.gate` / `Parked` machinery from 09c). An MCP client never observes a result for a non-durable write.
5. **Sessions are worker-owned.** The worker that serves `initialize` mints `Mcp-Session-Id = w<t>-<128-bit hex>`; the embedded worker index lets any other worker forward session-scoped work over the shard bus (`run_on`, the existing point-op machinery). Stateless until 15c — no session header is issued, which the spec permits.
6. **Protocol version `2025-06-18`.** Negotiated at `initialize`; absent header → assume `2025-03-26` (T6 — identical for the surface served here); anything else → `400`.
7. **`serde_json` for now.** Same dependency posture as the rest of `rt`; migrates when phase [05](05-hand-rolled-json.md) lands. **No MCP SDK crates** — the protocol layer is hand-rolled like the HTTP layer, per the zero-deps north star.
8. **Origin validated on every `/mcp` request** (T8): allow absent-Origin (non-browser clients) and a `WO_MCP_ORIGINS` allowlist defaulting to localhost origins; anything else → `403`. The localhost-bind guidance is already satisfied — `WO_LISTEN` defaults to `127.0.0.1:8080`. Authentication is deferred (non-scope; ties to the policy phase).

## Dependency graph

```
15a JSON-RPC core + tools ──→ 15b resources ──→ 15c SSE + sessions ──→ 15e LIVE subscriptions
        │                                        (first streaming         (needs 13c + 09d)
        │                                         response in rt)
        └──→ 15d `service mcp` surface  (parser-only; any time after 15a)
15a needs nothing that isn't shipped: router, sharded engine, group commit.
```

## Sub-phase sequence

### `15a-jsonrpc-core-and-tools.md` — the endpoint speaks MCP, tools work

- **Endpoint + envelope**: `POST /mcp` in `server.rs`; parse a single JSON-RPC 2.0 message (T1); protocol errors as JSON-RPC errors (`-32700` parse, `-32600` invalid request, `-32601` method not found, `-32602` invalid params). Notifications/responses → `202` empty (T3). `GET /mcp` and `DELETE /mcp` → `405` (T4/T5 — legal until 15c).
- **Header plumbing**: surface `Accept`, `Origin`, `MCP-Protocol-Version` (and later `Mcp-Session-Id`, `Last-Event-ID`) on `http::Request`; enforce decisions 6 and 8.
- **Lifecycle**: `initialize` (version negotiation; capabilities `{tools: {listChanged: false}}`; `serverInfo` from the app directory name + crate version), `notifications/initialized`, `ping`.
- **Tool generation**: per exposed type×op → `<type>_list`, `<type>_get`, `<type>_create`, `<type>_update`, `<type>_delete`, with `inputSchema` (JSON Schema) derived from catalog field types (unions → `enum`, embedded structs → nested `object`) — same source of truth as `describe_routes`.
- **`tools/call` dispatch** through the *same* handler paths REST uses: creates local, point ops `run_on(owner_of(id))`, lists fan out — no second data path. Engine/validation failures return `isError: true` inside the tool *result* (the MCP rule: execution errors are results, protocol errors are JSON-RPC errors). Mutations park on the WAL gate (decision 4).

**Exit:** scripted flow (checked in beside [`reference/rest/`](../../reference/rest/README.md)) against the blog sample passes: `initialize` → `202` for `initialized` → `tools/list` enumerates exactly the exposed ops → `article_create` → `article_list` shows the row; runs green with `WO_GROUP_COMMIT` on and off; `GET`→405, `DELETE`→405, bad version→400, disallowed Origin→403; unit tests in the `server.rs` style cover envelope errors and gate parking.

### `15b-resources.md` — the schema and rows become addressable

- URI scheme: `wo://schema/<type>` (field/shape listing as JSON) and `wo://<type>/<id>` (one row). `resources/list` returns the schema resources (bounded); `resources/templates/list` returns `wo://<type>/{id}` per exposed type; `resources/read` resolves both forms (row reads owner-hop like REST GET). Opaque id-based `nextCursor` pagination on list endpoints.
- Capabilities gain `resources: {subscribe: false, listChanged: false}` (flips in 15e).

**Exit:** `resources/read wo://articles/1` body-equals `GET /api/articles/1`; templates enumerate every exposed type; unknown URI → resource-not-found error (`-32002`); cursor walks a 3-page listing without duplication or loss.

### `15c-sse-and-sessions.md` — the "streamable" half

- **First streaming response in the runtime**: a streaming variant beside the buffered `Response` (`ConnState::Streaming`) that writes SSE frames (`event: message\ndata: <json>\n\n`) incrementally under epoll writability, honours backpressure (a slow reader parks on `EPOLLOUT`, never blocks the worker), and holds the connection out of keep-alive reuse until the stream closes. This is the piece 15e and Stage 3 inherit.
- **POST answering mode**: requests that will emit interim server messages answer in `text/event-stream` mode (response as the final SSE event, then close — T2); plain requests stay JSON. In 15c itself only long `tools/call`s use it; the machinery is the deliverable.
- **Sessions** (T5, decision 5): `Mcp-Session-Id` minted at `initialize`; missing on later requests → `400`; unknown → `404`; `DELETE /mcp` terminates → `200`. Per-worker session table; cross-worker requests forward via the worker index in the id.
- **`GET /mcp`** opens the session's server→client SSE stream (heartbeat comments to keep intermediaries happy; never carries responses — T4).

**Exit:** the spec's own sequence diagram replayed end-to-end by script (init+session → 202 → JSON answer → GET stream stays open across ≥2 heartbeats); 400/404/DELETE conformance matrix green; a deliberately unread client stalls only its own connection (other connections' p99 unaffected, measured).

### `15d-service-mcp-surface.md` — the language names the capability

- Parser: `ServiceKind::Mcp` + an ident arm for `mcp` in `parse_service` (ident, not keyword — the `expose` gotcha stands); `service mcp "/mcp" expose list, get, set_price` inside a `type`/`class` controls generation independently of REST. Precedence: `service mcp` present → it alone governs MCP exposure; absent → fall back to the `service rest` list (15a behaviour, now documented in the spec doc [`02-wo-language.md`](../runtime/database/02-wo-language.md)).
- **Methods become tools**: a 13b class method in an `expose` list generates `<type>_<method>` with `inputSchema` from the method's parameter list — the agent-facing twin of `POST /api/<t>/:id/<method>`. (Parses and lists from this phase; round-trips once 13b ships.)

**Exit:** parser tests for the new arm and precedence; the pricing sample gains a `service mcp` block; `tools/list` reflects it (method tools listed; callable gated on 13b).

### `15e-live-subscriptions.md` — commits push to agents

- Capabilities flip to `resources: {subscribe: true}`. `resources/subscribe {uri: wo://<type>/<id>}` registers a keyed (O(1)) subscription in the 13c registry, bound to the session's GET stream; commit → `notifications/resources/updated {uri}` pushed as an SSE event; cross-shard commits reach the session's worker via 09d fan-out; `resources/unsubscribe` and session teardown free registry slots (the `EPOLLHUP` → unsubscribe philosophy of the v1 datalayer).
- **Resumability** (T7): per-stream monotonic SSE `id:`s; a bounded per-session ring buffer of undelivered notifications; reconnect `GET` with `Last-Event-ID` replays from the cursor, stream continues.

**Exit:** two-terminal demo — subscribe to `wo://products/1` over the GET stream, `set_price` via REST curl in the other terminal, the notification arrives without polling; kill the client mid-stream, reconnect with `Last-Event-ID`, the missed notification is replayed exactly once. **Requires 13c + 09d.**

## Verification targets (after 15e)

| Check | Target | How |
| --- | --- | --- |
| Spec conformance | T1–T8 matrix green (status codes, headers, content types) | scripted curl flow checked in beside `reference/rest/` |
| Interop | MCP Inspector connects, lists tools/resources, calls a tool | manual check, noted per release |
| Parity | `tools/call <type>_get` ≡ `GET /api/<type>/:id` byte-for-byte on the row payload | unit test |
| Durability | mutation results never precede their fsync CQE (`WO_GROUP_COMMIT` on) | gate test in `server.rs` style |
| Latency | `tools/call` read p99 within 1 ms of the REST equivalent under the plan-09 bench load | bench harness rerun |
| Dep budget | no new crates; `serde_json` only, dropped with phase 05 | `Cargo.toml` review |

## Non-scope

- **No 2024-11-05 HTTP+SSE backwards compatibility.** Only Streamable HTTP; old-transport clients are not served.
- **No stdio transport.** A `wo mcp-stdio` subcommand would be cheap later; out of scope here.
- **No authorization.** The MCP auth spec (OAuth 2.1) waits for the policy phase; until then the endpoint trusts what the Origin check and bind address admit.
- **No prompts capability, no client-feature counterparts** (sampling, elicitation, roots) — server capabilities only.
- **No JSON-RPC batching** — removed from the protocol in 2025-06-18; single message per POST, enforced.
- **No WebSocket.** MCP rides SSE only; the 13c browser WebSocket at `/api/<type>/live` is a separate surface sharing the same registry.

## Cross-references

- [`../runtime/database/04-client-api.md`](../runtime/database/04-client-api.md) — the protocol-tier survey; this plan implements its "REST + SSE gateway" row for agents, on the same subscription registry it specifies.
- [`./13-class-model-live-pricing.md`](./13-class-model-live-pricing.md) — 13b gates method tools (15d); 13c gates 15e; 13d's demo has an agent-shaped twin in 15e's exit.
- [`./09-concurrency-scaleout.md`](./09-concurrency-scaleout.md) — 09d gates cross-shard notification fan-out (15e); the shard-bus ownership rules 15a/15c reuse.
- [`./05-hand-rolled-json.md`](05-hand-rolled-json.md) — removes this plan's `serde_json` use when it lands.
- [`./07-inotify-content-watcher.md`](07-inotify-content-watcher.md) — a future `notifications/tools/list_changed` on hot reload would pair with it (not scheduled).
- [MCP specification 2025-06-18](https://modelcontextprotocol.io/specification/2025-06-18/basic/transports) — the normative transport text summarized in T1–T8.
