---
track: porch
iteration: "6"
status: pending
readiness: ready
---

# porch 6 — streaming core: the seam two iterations wait on

> Part of [Story — `porch`, the writeonce web framework](00-story.md).
> Source: [the Fiber parity study](../../plan/exploration/fiber/00-fiber-parity.md) §3,
> re-checked 2026-09-06 against `.dev/reference/fiber` (v3, `3ca9a9d`) and the
> existing `internal/serve.wo`/`app.wo` dispatch pipeline.
>
> The largest and riskiest slice in this track, and the one with the most
> downstream value: iterations [7](07-sse-and-compression.md) and
> [8](08-static-and-lifecycle.md) are both blocked on it, and porch's README has
> carried "lazy body streaming · streaming responses · explicit commit point"
> as parked since framework v1.
>
> Re-scoped by the brainstorm to **outbound streaming only** — chunked *request*
> bodies split into their own future iteration (decision 3), so this slice ships
> and gates without entangling the smuggling surface.

## Goals

- **A response that can be written incrementally.** Today `internal/serve.wo`
  builds the whole response as one `Text` and hands it to a single `net.write`,
  and `serialize()` always emits `Content-Length`. Nothing can produce output it
  cannot first hold entirely in memory — which rules out large downloads,
  server-sent events, and any response whose length is unknown when the first
  byte is ready.
- **Chunked transfer-encoding on the way out**, correctly framed and correctly
  terminated, because a truncated chunked response is indistinguishable from a
  network failure to the client and corrupts keep-alive for the connection.
- **An explicit commit point.** Once the first byte is written, the status and
  headers are gone and no `after` middleware can change them. That is a real
  semantic change to the middleware contract and it has to be stated, not
  discovered — the `after` chain currently runs on *every* response and security
  headers depend on it.

## Decisions locked (brainstorm 2026-09-06)

1. **A separate `StreamHandler` interface and a parallel dispatch path — the
   `Resp` path is untouched.** A streaming route registers a `StreamHandler`
   (`app.stream(pattern, …)`), distinct from `Handler`. Its output is a stream
   outcome carrying status, a header set, and a `BodyProducer` — an interface
   (a class with a `next() -> ?Bytes` method, `nil` = done), so the no-closures
   doctrine holds. Dispatch resolves a streaming route on its own branch;
   `serve_conn` gains one branch: a stream outcome writes its headers then pumps
   the producer as chunks, everything else serializes exactly as today. The two
   paths coexist, which is what keeps every existing response byte-identical.
2. **Streaming routes opt out of the after-chain, and the framework refuses at
   registration to combine one with header-mutating middleware.** The after-chain
   is *not* re-plumbed onto the streaming branch. Instead, at startup the
   framework raises a loud error naming the conflict if a streaming route is in
   the scope of a header-mutating `after` middleware (`SecurityHeaders`, `Cors`)
   — never a silent partial application, which the study calls the one
   unacceptable answer. A stream handler that wants those headers stamps them
   itself via a `security_headers()` helper before committing, so opting out of
   the chain does not mean losing them — it means setting them explicitly. Once
   `serve_conn` writes the header set, it is frozen; a later mutation attempt
   hits a guard that traps or logs in development.
3. **Chunked *request*-body parsing is split into its own future iteration; the
   deliberate refusal stays until then.** Inbound parsing is orthogonal to
   outbound streaming (a different file, `parse.wo`), it is the single riskiest
   security surface in the framework (smuggling), and this slice is already the
   largest — so it ships outbound-only. `internal/parse.wo`'s refusal of chunked
   request bodies remains the behaviour, correct as it stands.

## Phases

### Phase A — the writer seam

- Introduce the `StreamHandler` and `BodyProducer` interfaces and the
  `app.stream` registration (decision 1). A streaming handler emits body bytes
  progressively through the producer instead of returning a complete `Resp`.
- Keep the existing whole-response path as the default and unchanged: the
  overwhelming majority of responses are small and must not pay for this. The
  `Resp` path, `serialize()`, `route_req` and the after-chain are untouched.
- Verify: the two paths coexist; every existing response is byte-identical, both
  serving gates unchanged.

### Phase B — chunked responses

- Chunk framing (hex size, CRLF, bytes, CRLF), the terminating zero-length
  chunk, and `Transfer-Encoding: chunked` in the header set; `Content-Length`
  and chunked are mutually exclusive and the stream path emits chunked and never
  a length — mutual exclusion holds by construction because it is a separate path
  from `serialize()`.
- Keep-alive interaction: a chunked response that completes (zero chunk sent)
  may keep the connection; one truncated by a mid-stream producer trap must close
  it, never reuse it, and leave no half-frame.
- `HEAD` on a streaming route: the header set (including `Transfer-Encoding:
  chunked`) with no body and no `Content-Length`, since the length is unknown.
- Verify: a chunked response reassembles byte-exactly; a mid-stream trap closes
  the connection rather than leaving a half-frame; `HEAD` is coherent.

### Phase C — the commit point and the middleware contract

- Implement the opt-out and the registration refusal (decision 2): a streaming
  route under a header-mutating `after` middleware is refused at startup with a
  message naming the conflict.
- Provide the `security_headers()` helper so a stream handler stamps the standard
  security headers into its own header set before committing.
- Enforce the commit point: after `serve_conn` writes the header set, it is
  frozen; a mutation attempt traps or logs in development rather than silently
  doing nothing.
- Verify: security headers and CORS are present on a streamed response that asks
  for them via the helper; a streaming route wrongly combined with
  header-mutating middleware is refused at startup; a post-commit mutation
  attempt is reported.

### Phase D — the gate and the ledger

- A streaming route in a sample, gated on both consumers, plus a large-body leg
  proving peak memory does not scale with response size, and a regression check
  that the WebSocket 101 hijack path still works untouched.
- Retire the README's parked streaming rows; record the commit-point semantics
  and the streaming opt-out where a handler author will find them.
- Verify: `just web-app`, `just site`, `just linkcheck` green; ASan clean.

## Acceptance Criteria

- **Given** a streaming handler emitting N chunks, **when** a client reads the
  response, **then** the reassembled body is byte-exact and the framing is
  well-formed.
- **Given** a response far larger than the arena, **when** it is streamed,
  **then** it completes and peak memory does not grow with the body — the
  criterion that distinguishes streaming from buffering.
- **Given** a handler that traps mid-stream, **when** the failure occurs,
  **then** the connection is closed rather than reused, no half-frame is left
  behind, and the server survives.
- **Given** a streamed response whose handler calls `security_headers()`,
  **when** it leaves, **then** those headers are present.
- **Given** a streaming route registered in the scope of a header-mutating
  `after` middleware, **when** the app starts, **then** it is refused with a
  message naming the conflict — never a silently unheadered stream.
- **Given** an `after` or handler attempting to change a committed response,
  **when** it runs, **then** the attempt is reported rather than silently
  dropped.
- **Given** a `HEAD` request to a streaming route, **when** it is answered,
  **then** the headers are coherent (chunked, no `Content-Length`) and no body is
  sent.
- **Given** the WebSocket 101 hijack path, **when** an upgrade is handled after
  this iteration, **then** it still bypasses `serialize()` and works untouched.
- **Given** every pre-existing non-streaming response, **when** both serving
  gates run, **then** output is byte-identical to before this iteration.

## Out Of Scope

- **Chunked *request* bodies** — split into their own future iteration
  (decision 3). The `internal/parse.wo` refusal stays until then; when it is
  picked up it must enumerate the smuggling cases (both `Content-Length` and
  `Transfer-Encoding` present, duplicated `Transfer-Encoding`, unknown transfer
  codings, oversized or malformed chunk sizes), each a 400-and-close, matching
  the existing duplicate `Content-Length` discipline.
- **SSE** — iteration [7](07-sse-and-compression.md), the first consumer.
- **Compression** — also [7](07-sse-and-compression.md); it composes with
  chunking and should not be entangled with building it.
- **Byte ranges and `SendFile`** — iteration [8](08-static-and-lifecycle.md).
- **Request-body backpressure as a general mechanism.** Reading a body slowly to
  push back on a producer wants cancellation, which porch does not have and which
  language iteration 31's actor lifecycle owns. This iteration streams *out*; it
  does not add flow control. A slow client simply parks the writing fiber.
- **WebSockets.** Already shipped (`ws_accept`, `wsframe`) and deliberately a
  hijack that bypasses `serialize()` — that path must keep working untouched,
  which is why phase D checks it.
- **HTTP/2.** Proxy-terminated by doctrine, and parked behind language
  iteration 23 regardless.

## Info

No language enhancement is needed: the writer seam is `net.write` (id 54) called
repeatedly for framing, a `BodyProducer` sourcing bytes from `fs.read_at`
(id 44) or an actor `receive`, and interfaces/classes for the producer — all
present. The risk in this slice is design risk (the commit contract, framing
correctness, keep-alive on truncation), not a runtime gap, and the streaming
writes ride the existing fiber-per-connection loop, not the per-key actor pool,
so this iteration is not exposed to the lang-41 hang.

Forks are settled above. The one the brainstorm moved most was fork 1: rather
than folding a producer into `Resp` (which would have made the after-chain apply
for free), the decision is a genuinely separate `StreamHandler` path — which is
why fork 2 had to be answered explicitly, and why streaming routes opt out of
the after-chain with a loud registration refusal rather than inheriting it.
