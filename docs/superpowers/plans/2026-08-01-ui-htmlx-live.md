# UI — .htmlx SSR + LIVE Subscriptions Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
>
> **Style rule (user convention):** concept, reason, and required behavior in words only; the executor writes the code.

**Goal:** Sub-project 5 of the OOP spec — the single binary serves a user interface: `##ui` screens compile to `.htmlx` templates rendered server-side, LIVE subscriptions push delta frames over WebSocket on commit, and the pricing demo's driving workload runs end to end — a price update on the server patches subscribed browsers' cells in place.

**Architecture:** Plan 7 of 7. Depends on plans 1–6. Direction is already locked by the UI track (`docs/plan/exploration/ui/00-overview.md`): **`.htmlx` is the template format; `##ui` is the DSL that emits it** — the v1 engine at `.dev/reference/crates/wo-htmlx/` (bindings, each-blocks, partials, data-bind attributes) is the semantic reference the C renderer ports. The subscription machinery is the Stage-3 story (`docs/runtime/database/04-client-api.md`, plan 13c): a per-shard registry hooked into the commit path emits delta frames to WebSocket subscribers — replacing the honest 501 that plan 6 preserved. Static assets (the client runtime JS) serve per the sendfile doctrine (`docs/plan/08-sendfile-static-assets.md`). The UI track's larger workspace story (apps/, per-app binaries, shared DB daemon — ui docs 04–06) is explicitly OUT of this plan: one binary, its own screens, first.

**Tech Stack:** C11 + libc (WebSocket framing hand-rolled; SHA-1 + base64 for the upgrade handshake hand-rolled — the only crypto in the runtime, ~150 lines, documented). OCaml (##ui parsing, .htmlx emission). Vanilla JS client runtime (~20 KB target per the UI track), no build tooling.

## Global Constraints

- All prior plans' constraints carry over (libc only, no commits — drafts to `.dev/commit.md`, gates, docs under `docs/`).
- **`.htmlx` semantics follow the v1 engine** where features overlap (`{{path}}` bindings, `{{#each}}`, `{{> partial}}`, `data-bind`); the live-subtree extension (`<wo:live source="...">`) is this plan's addition, specified in the format doc it writes.
- **Deltas ride commits:** subscription taps sit AFTER the WAL accepts, never in the read or ack path — a slow subscriber can never delay a commit (the Postgres-mirror tap discipline, applied to WebSockets: overflow drops the subscriber loudly, never blocks the shard).
- **Subscriptions are shard-local:** a socket subscribes on the shard that owns its connection; queries against rows on that shard push directly. Cross-shard live queries are out of scope, stated in the doc.
- **No JS frameworks, no bundlers** — the client runtime is one hand-written file served as a static asset.

---

## File Structure

```
runtime/src/
  ws.c ws.h              WebSocket upgrade (SHA-1/base64), frame codec, ping/pong (Task 1)
  sub.c sub.h            per-shard subscription registry + commit tap + delta encode (Task 2)
  htmlx.c htmlx.h        template parse + SSR render (Task 4)
  assets.c assets.h      static asset serving, sendfile path (Task 5)
compiler/src/            ##ui parsing + .htmlx emission (Task 3)
client/wo-live.js        the DOM-patching client runtime (Task 5)
tests/corpus/ui/         render goldens + live end-to-end scripts (Task 6)
docs/plan/oop-vm/06-ui-live.md   .htmlx subset, wo:live semantics, delta frame format (Task 1)
```

---

### Task 1: WebSocket transport

**Concept & reason:** the push channel, hand-rolled to the zero-dep bar: HTTP upgrade handshake (the fixed-GUID SHA-1/base64 accept key — the two primitives implemented locally with test vectors from their RFCs), frame codec (text frames, masking rules, fragmentation tolerated on receive, close handshake, ping/pong), mounted on plan 6's connection machine so a socket upgrades in place and joins the shard's loop. The doc this task writes pins everything downstream: the delta frame JSON shape (kind: insert/update/delete, class, id, changed fields), the subscribe message a client sends, and the `wo:live` template semantics Task 3–5 implement.

- [ ] Failing tests: RFC test vectors for the accept key; frame codec round-trips incl. masked payloads and close; a socket-level upgrade-then-echo harness on the real loop.
- [ ] Implement; green.
- [ ] Record commit draft: `feat(runtime): hand-rolled WebSocket — upgrade handshake (local SHA-1/base64 with RFC vectors), frame codec, ping/pong/close, mounted on the shard loop; docs/plan/oop-vm/06-ui-live.md pins delta/subscribe/wo:live formats.`

### Task 2: Subscription registry + commit taps

**Concept & reason:** Stage 3's engine, per shard. A registry maps subscription keys (class + optional indexed-field filter, the plan-5 WHERE subset) to subscriber lists (socket + subscription id). The commit path gains a tap after the WAL accept: each committed mutation consults the registry, encodes one delta frame, and enqueues it on matching subscribers' write queues with try-semantics — a full queue drops that subscriber with a loud close frame and a log line (the mirror discipline: RAM-side progress never waits on a consumer). The `/api/<type>/live` route flips from 501 to the upgrade + subscribe flow; unsubscribe and disconnect clean the registry.

- [ ] Failing tests: registry match/miss across filters; commit-to-frame flow on a rigged shard (insert/update/delete each produce the right frame); overflow drops the subscriber and only the subscriber; disconnect cleanup; the 501 fixture from plan 6 flips to expecting an upgrade.
- [ ] Implement; green under ASan/TSan (sockets and registry are shard-local — the tests prove no cross-thread traffic exists).
- [ ] Record commit draft: `feat(runtime): LIVE subscriptions — per-shard registry with filter matching, post-WAL commit taps with try-enqueue drop-loudly discipline, /live flips from 501 to upgrade+subscribe; delta frames per the pinned format.`

### Task 3: `##ui` compiles to `.htmlx`

**Concept & reason:** the compiler's side of the locked UI decision. The parser accepts the `##ui` block form the samples use (screen name, source class, projection/columns, filters) and the typechecker validates it against the class (fields exist, filter fields indexed where the live path requires it). Emission writes an `.htmlx` file per screen into the build output beside the `.wob`: bindings for projected fields, an each-block over the source rows, and the screen's live subtree wrapped in `<wo:live source="...">` carrying the subscription key the runtime will register. Hand-written `.htmlx` files in the project pass through untouched (first-class authoring alternative, per the locked decision) — the compiler only validates their `wo:live` sources against the schema.

- [ ] Failing tests: golden `.htmlx` output for a pricing screen fixture; validation diagnostics (unknown field, unfilterable live source); pass-through of a hand-written template with source validation.
- [ ] Implement; green.
- [ ] Record commit draft: `feat(compiler): ##ui emits .htmlx — screen DSL parse/typecheck, binding+each+wo:live template generation with subscription keys, hand-written .htmlx pass-through with source validation.`

### Task 4: `.htmlx` SSR renderer

**Concept & reason:** the C port of the v1 engine's render semantics, scoped to the compiled subset: parse the template once at boot into a node tree (static chunks, bindings, each-blocks, partials, live-subtree markers); render a screen by walking the tree against query results from the plan-5 select path, HTML-escaping bound values, expanding each-blocks per row, and stamping each live subtree with the ids the client runtime needs to patch later (stable per-row element ids derived from class + row id — the contract the delta patcher relies on). Rendered pages route like any handler; the screen's route comes from the service/UI declarations.

- [ ] Failing tests: render goldens (template + fixture rows → exact HTML) covering escaping, each over rows, partials, and live-subtree id stamping; a malformed-template diagnostic at boot, not at request time.
- [ ] Implement; green.
- [ ] Record commit draft: `feat(runtime): .htmlx SSR renderer — boot-time parse to node tree, escaped binding render over select results, stable per-row ids in live subtrees; render goldens.`

### Task 5: Client runtime + static assets

**Concept & reason:** the last mile. `wo-live.js` (hand-written, one file, ~20 KB budget): on load, find `wo:live` subtrees, open the WebSocket, send subscribe messages from the stamped keys, and patch on frames — update replaces bound cell contents by stable id, insert appends a row rendered from a client-side row template the SSR emitted, delete removes the row's element; reconnect with backoff; a visible stale indicator when the socket is down (honesty over silence). Static serving: the asset module serves the JS (and any project assets) with correct content types and the sendfile-doctrine path for regular files.

- [ ] Failing tests: asset serving (content type, byte-exact body, 404 miss); client runtime exercised by the Task-6 end-to-end (no separate JS test harness — stated tradeoff: the e2e is the test).
- [ ] Implement; green.
- [ ] Record commit draft: `feat: wo-live.js client runtime (subscribe from stamped keys, patch update/insert/delete by stable ids, reconnect+stale indicator) + static asset serving on the sendfile path.`

### Task 6: Pricing live demo end to end + acceptance

**Concept & reason:** the spec's driving workload, closed: the pricing project compiles to one binary; a scripted browser-less client (a test WebSocket client speaking the pinned protocol) loads the SSR page, subscribes, then a method RPC (`set_price` over plan 6's route) commits an insert — the script asserts the delta frame arrives with the new amount, and that a second subscriber sees it too (fan-out). A manual demo recipe (`just pricing-live-demo`) serves it for human eyes. Render goldens + the scripted live scenario + the asset tests join `just oop-accept`. Docs closeout: kanban (13c-equivalent milestone on the C stack), CLAUDE.md stage table amendment, the UI track doc gains a status note that the format/live layer shipped and the workspace/per-app-binary layers (ui docs 04–06) remain open.

- [ ] Wire the scenario + recipe + gate; green; docs synced.
- [ ] Record commit draft: `test: pricing live demo e2e — SSR load, subscribe, set_price RPC commit, delta fan-out asserted by scripted WS clients; just pricing-live-demo; oop-accept gains the UI gate; UI-track status synced.`

---

## Plan self-review notes

- **Spec coverage (sub-project 5 + Stage 3):** `##ui` SSR, live subscriptions with delta frames on commit, the pricing live workload, single binary serving UI + API + DB — the spec's "single binary that is the database, the web API, and the UI" sentence is fully mechanized after this plan. Non-scope, stated: cross-shard live queries, the workspace/per-app-binaries/shared-daemon layers (ui docs 04–06), auth/`me` sessions, TLS.
- **Order rationale:** transport before registry before templates before renderer before client — each is the next one's substrate; the e2e needs all five.
- **Consistency check:** delta frame shape, subscribe message, stable-id contract, and `wo:live` semantics are pinned once (Task 1 doc) and consumed by Tasks 2–5; subscription keys originate in the compiler (Task 3) and terminate in the registry (Task 2) — same key format, one doc section.
