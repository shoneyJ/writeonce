---
iteration: "39"
status: hold
---

# Iteration 39 — web framework parity *(superseded by the porch track)*

> **⏸ SUPERSEDED 2026-08-26, the same day it was written.** Framework work now
> lives in its own track: [`docs/stories/porch/`](../porch/00-story.md), numbered
> from 1. This iteration's content was split across **porch 1–5** and is not
> planned from here — the sequencing below survives, but as that track's
> dependency order.
>
> | This iteration's goal | Now |
> | --- | --- |
> | limiter + idempotency (the cheap first slice) | [porch 1](../porch/01-store-backed-middleware.md) |
> | random-bytes builtin, cookies, `Resp` repeated headers | [porch 2](../porch/02-randomness-and-cookies.md) |
> | sessions | [porch 3](../porch/03-sessions.md) |
> | CSRF | [porch 4](../porch/04-csrf.md) |
> | method helpers, named routes, body limit, request id, response helpers | [porch 5](../porch/05-routing-response-ergonomics.md) |
> | *(deferred here, now scheduled)* streaming, SSE, compression, byte ranges | [porch 6](../porch/06-streaming-core.md)–[8](../porch/08-static-and-lifecycle.md) |
>
> Kept rather than deleted because the [Fiber study](../../plan/exploration/fiber/00-fiber-parity.md)
> cites it and because the reasoning below — especially why the randomness
> blocker comes first — is what the porch track is built on. Original text
> follows.

## Original scope

> Format: fiberloom `product/story-iteration-template`. Part of
> [Story — one language, one runtime, one database, one binary](00-story.md).
>
> **Inserted 2026-08-26** (developer ask: add gofiber/fiber as a reference and
> find the basic features the web framework lacks). Derived from
> [the Fiber v3.5.0 parity study](../../plan/exploration/fiber/00-fiber-parity.md),
> which read Fiber's routing surface, `Req`/`Res` API, binder and all 32 of its
> `middleware/` packages against `docs/examples/porch`. This iteration
> takes that study's §0–§2 plus the cheap half of §5; the study names an owner
> for everything it leaves out.

## Goals

- **A random-bytes builtin, first, because it gates the rest.** The framework
  README's crypto row claims signed cookies, CSRF and session integrity are
  "UNBLOCKED — the primitives exist since iteration 34". For CSRF and sessions
  **that is wrong**: SHA-256 and HMAC let a program *authenticate* a token, not
  *mint* one, and writeonce has no source of randomness anywhere — no
  `getrandom(2)`, no CSPRNG builtin, nothing. An HMAC over a guessable session
  id is a signed guess. Close this before writing a line of session code, and
  correct the ledger row that says otherwise.
- **Cookies, in both directions.** Absent entirely today: nothing parses a
  `Cookie:` request header and there is no `Set-Cookie` builder. This is the
  foundation the next goal stands on, and it forces a design decision the
  framework has so far avoided — `Resp.headers` is a `map<Text, Text>`, so it
  **structurally cannot** carry the two `Set-Cookie` lines a login-plus-flash
  response needs. Deciding what replaces or supplements that map is the real
  work of this goal; the parsing is the easy half.
- **The store-backed middleware chain, in dependency order**: rate limiting and
  idempotency first (they need only a store and `time.ticks`, both of which
  exist — the cheapest real wins available), then sessions, then CSRF. Each is
  an ordinary `.wo` middleware class, and each gets a **durable** store for
  free from `@table` — where Fiber ships an in-memory default and expects you
  to bolt on Redis. That is a genuine writeonce advantage and the samples should
  show it.
- **Close the routing and response sugar that is merely missing.** `PATCH` /
  `OPTIONS` / `HEAD` / `ALL` registration helpers (a `Route { method: "PATCH" }`
  literal already works, so this is registration ergonomics), a per-route body
  limit instead of one compile-time `BODY_MAX = 1048576` for the whole server, a
  request-id middleware, and the response helpers every framework has and this
  one writes by hand: `Location`, `Vary`, `Attachment`/`Download`.
- **Say what is still missing, with an owner.** The study's §3–§6 stay out of
  scope; this iteration's closing act is updating the framework README's ledger
  so each row points at whoever owns it rather than reading as an oversight.

## Acceptance Criteria

- **Given** the random-bytes builtin, **when** the acceptance script draws many
  values across separate processes, **then** no value repeats and none is
  derivable from the clock — and the builtin is refused, loudly, if the kernel
  source is unavailable rather than silently falling back to something weaker.
  A CSPRNG that degrades quietly is worse than no CSPRNG.
- **Given** a login handler that sets a session cookie and a flash cookie on
  one response, **when** the response is serialized, **then** **two** distinct
  `Set-Cookie` headers reach the wire — the criterion the current
  `map<Text, Text>` cannot satisfy, and the reason this iteration touches
  `Resp`.
- **Given** a request carrying a `Cookie:` header with several pairs, quoted
  values and stray whitespace, **when** it is parsed, **then** each value is
  recovered exactly, and a malformed header is a 400 rather than a silent
  partial parse.
- **Given** a session cookie whose HMAC is altered by one bit, **when** the
  session middleware reads it, **then** the session is rejected in constant
  time (`ct_eq`, which already exists) and the request proceeds unauthenticated
  — never as a *different* user.
- **Given** the limiter configured to N requests per window, **when** a client
  exceeds it, **then** it receives 429 with the rate-limit headers set, the
  window expires on `time.ticks`, and the counters **survive a restart** — the
  durability the `@table` store buys, proven by a restart in the gate.
- **Given** a CSRF-protected form flow, **when** a request arrives with a
  missing, stale or foreign-origin token, **then** each is refused distinctly;
  **when** the token is valid, the request proceeds. Single-use tokens are not
  double-spendable.
- **Given** the same POST replayed with an identical idempotency key, **when**
  it is handled, **then** the stored response is returned and the handler does
  not run twice — proven by a side effect that would be visible if it had.
- **Given** a route registered with each new method helper and a per-route body
  limit, **when** the matrix runs, **then** methods dispatch correctly, an
  oversized body is refused per-route rather than per-server, and every standing
  gate (`just web-app`, `just site`, `oop-accept`) is unchanged.

## Out Of Scope

Every item below is a real Fiber feature and a real writeonce gap. Each is
excluded because someone else owns it — the study's §7 is the full map.

- **Streaming, SSE, compression, byte-range requests** — all four sit on one
  missing seam: `internal/serve.wo` builds a whole response as one `Text` and
  `serialize()` always emits `Content-Length`. The framework README already
  parks "lazy body streaming + backpressure · streaming responses · explicit
  commit point"; these belong to that slice. Note `internal/parse.wo:153-157`
  **deliberately refuses** chunked request bodies with a request-smuggling note
  — that refusal is correct and must survive whoever implements chunked.
- **Typed binding of query / params / form / headers into a class** — Fiber's
  `Bind` reflects over struct tags; principle 13 forbids reflection, so the
  answer is compile-time generation:
  [iteration 29's `@derive`](29-compile-time-metaprogramming.md). Recorded, not
  attempted. Same for XML/CBOR/MsgPack codecs — JSON only is the small stdlib
  working as intended.
- **TTL cache middleware** — [iteration 18](18-memory-db-features.md) owns it,
  spec already approved.
- **`proxy` middleware** — needs an outbound socket, which does not exist:
  [iteration 38](38-content-platform-capabilities.md).
- **`pprof` / `expvar` / metrics / stack traces on trap** — iteration 30
  (observability, CI, fuzz — still no story file).
- **Everything the study's §6 lists as a deliberate divergence**: a runtime
  template engine (markup is a compile-time literal or it does not exist), TLS
  and HTTP/2 (proxy-terminated by doctrine), `net/http` interop (no FFI),
  prefork and buffer-size knobs (the shard runtime owns placement), closures as
  handlers (a handler is a class; its fields are the closure substitute). These
  are settled — the study lists them so nobody re-opens them as "missing".
- **A radix-tree router.** Path matching is a linear scan, already 🔶 in the
  ledger pending a *measurement*. Iteration 22 built the harness but benched the
  database, not the router. Still waiting on a number, not on this iteration.

## Info

Fiber v3.5.0, `.dev/reference/fiber` (gitignored; the study carries the clone
command). Of its 32 middleware packages, **nine already have a working
`porch` counterpart** — CORS, basic auth, key/bearer auth,
helmet-style security headers, ETag, static files, logger, host authorization,
and recover-as-500. The framework is further along than its size suggests; the
gaps are breadth, not foundations, with the single exception of randomness.

Dependency order, which is the whole point of the iteration:

```
random bytes  ──▶  cookies  ──▶  sessions  ──▶  CSRF
     │                 │
     │                 └──▶  (signed cookies, JWT HS256 issuing)
     └──▶  request-id

limiter, idempotency  ──▶  need only @table + time.ticks (start here)
```

Forks the spec must settle:

1. **What replaces `Resp.headers: map<Text, Text>`?** Options: a
   `multi Text` of raw header lines beside the map; a dedicated
   `cookies: multi Cookie` field on `Resp` that `serialize()` renders; or a
   general repeated-header list. The first is smallest, the second is the most
   typed, the third is the most honest about HTTP. This fork decides how much
   of the framework's public surface moves, so it comes first.
2. **What shape is the random builtin?** `random_bytes(n) -> Bytes` is the
   obvious one and composes with everything iteration 19 and 34 added. Decide
   whether a convenience `random_hex`/`random_id` rides along or whether
   `base64_encode` is enough, and decide the failure mode when the kernel source
   is unavailable — the criterion above says refuse, not degrade.
3. **Is there a `Store` interface, or does each middleware own its `@table`?**
   Fiber abstracts `Storage` so the same middleware runs on memory or Redis.
   writeonce has one store, and interfaces are structural — an abstraction with
   exactly one implementor is decoration (iteration 37 learned this the hard way
   about `Component`). Leaning: concrete `@table` per middleware until a second
   backend actually exists.
4. **Where does session state live — cookie or table?** A signed cookie
   carrying the whole payload needs no store and cannot be revoked; a table row
   keyed by a random id can be revoked and costs a lookup. Leaning: table, since
   `@table` is the language's whole thesis and revocation is not optional for a
   real login.
5. **Does the request-id middleware trust an inbound header?** Fiber's
   `requestid` accepts one by default. Behind a trusted proxy that is what you
   want; on an open port it lets a client forge correlation ids. `client_ip` and
   `net.peer` already exist for the trust decision — reuse that judgement rather
   than inventing a second one.

## Proposed Solution

Wants a spec: fork 1 changes public framework types, and fork 2 adds a runtime
builtin. Order follows the dependency chain above, and the first slice is
deliberately the *cheapest* rather than the most foundational — limiter and
idempotency need nothing new, so they prove the store pattern and the gate shape
before the risky work starts.

Then: the random builtin (runtime, with its own corpus fixtures), cookies (the
`Resp` decision plus parse and serialize), sessions, CSRF, and the routing and
response sugar last since it is independent of everything else. Each slice ends
green on `just web-app` and `just site`, and the closing act corrects the
framework README's crypto row and re-points every ledger row this study touched
at its actual owner.

Off-chain and independent of the concurrency chain, with one caveat worth
stating: the SSE work this study defers is a natural companion to
[iteration 24's](24-chat-websocket-workload.md) chat sample, since a room actor
already has the fan-out shape. If 24 lands first, SSE gets cheaper.
