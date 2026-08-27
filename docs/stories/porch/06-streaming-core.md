---
track: porch
iteration: "6"
status: pending
readiness: refine
---

# porch 6 — streaming core: the seam three iterations wait on

> Part of [Story — `porch`, the writeonce web framework](00-story.md).
> Source: [the Fiber parity study](../../plan/exploration/fiber/00-fiber-parity.md) §3.
>
> The largest and riskiest slice in this track, and the one with the most
> downstream value: iterations [7](07-sse-and-compression.md) and
> [8](08-static-and-lifecycle.md) are both blocked on it, and porch's README has
> carried "lazy body streaming · streaming responses · explicit commit point"
> as parked since framework v1.

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
- **Chunked request bodies on the way in — carefully.**
  `internal/parse.wo` **deliberately refuses** them today, with a correct note
  that silently treating a chunked request as body-less is request smuggling.
  That refusal is good engineering. It may only be lifted by an implementation
  that handles the smuggling cases explicitly, and the refusal must remain the
  behaviour for anything the parser is not certain about.
- **An explicit commit point.** Once the first byte is written, the status and
  headers are gone and no `after` middleware can change them. That is a real
  semantic change to the middleware contract and it has to be stated, not
  discovered — the `after` chain currently runs on *every* response and
  security headers depend on it.

## Phases

### Phase A — the writer seam

- Decide the shape (fork 1) and introduce a way for a handler to emit body
  bytes progressively instead of returning a complete `Resp`. Handlers are
  classes, so this is a second interface beside `Handler`, not a callback.
- Keep the existing whole-response path as the default and unchanged: the
  overwhelming majority of responses are small and should not pay for this.
- Verify: the two paths coexist; every existing response is byte-identical.

### Phase B — chunked responses

- Chunk framing, the terminating zero-length chunk, and the interaction with
  keep-alive — a connection whose chunked response was truncated must be closed,
  not reused.
- `Content-Length` and chunked are mutually exclusive; `serialize()` must pick
  one and never emit both.
- `HEAD` on a streaming route: headers only, and decide what `Content-Length`
  claims when the length is unknown.
- Verify: a chunked response reassembles byte-exactly; a mid-stream trap closes
  the connection rather than leaving a half-frame; `HEAD` is coherent.

### Phase C — the commit point and the middleware contract

- Define and enforce when headers are locked. An `after` middleware that tries
  to mutate a committed response must fail loudly in development rather than
  silently doing nothing.
- Decide what happens to `SecurityHeaders` and `Cors` — both are `after`
  middleware and both must still apply to streamed responses, which means they
  have to run *before* the commit for those routes.
- Verify: security headers and CORS are present on a streamed response; a
  post-commit mutation attempt is reported.

### Phase D — chunked request bodies

- Only if phase C is clean. Parse chunked request bodies with the smuggling
  cases enumerated and tested: both `Content-Length` and `Transfer-Encoding`
  present, duplicated `Transfer-Encoding`, unknown transfer codings, and
  oversized or malformed chunk sizes.
- Every ambiguous case stays a 400-and-close, matching the existing duplicate
  `Content-Length` discipline.
- Verify: a well-formed chunked upload arrives intact; every enumerated
  smuggling shape is refused.

### Phase E — the gate and the ledger

- A streaming route in a sample, gated on both consumers, plus a large-body leg
  proving memory does not scale with response size.
- Retire the README's parked streaming rows; record the commit-point semantics
  where a handler author will find them.
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
- **Given** a streamed response, **when** it leaves, **then** the security and
  CORS headers the `after` chain contributes are still present.
- **Given** an `after` middleware attempting to change a committed response,
  **when** it runs, **then** the attempt is reported rather than silently
  dropped.
- **Given** a `HEAD` request to a streaming route, **when** it is answered,
  **then** the headers are coherent and no body is sent.
- **Given** a request with both `Content-Length` and `Transfer-Encoding`,
  **when** it is parsed, **then** it is refused with 400 and the connection is
  closed — smuggling is refused, never guessed at.
- **Given** every pre-existing non-streaming response, **when** both serving
  gates run, **then** output is byte-identical to before this iteration.

## Out Of Scope

- **SSE** — iteration [7](07-sse-and-compression.md), the first consumer.
- **Compression** — also [7](07-sse-and-compression.md); it composes with
  chunking and should not be entangled with building it.
- **Byte ranges and `SendFile`** — iteration
  [8](08-static-and-lifecycle.md).
- **Request-body backpressure as a general mechanism.** Reading a body slowly to
  push back on a producer wants cancellation, which porch does not have and
  which language iteration 31's actor lifecycle owns. This iteration streams
  *out* and parses chunked *in*; it does not add flow control.
- **WebSockets.** Already shipped (`ws_accept`, `wsframe`) and deliberately a
  hijack that bypasses `serialize()` — that path must keep working untouched,
  which is worth an explicit regression check.
- **HTTP/2.** Proxy-terminated by doctrine, and parked behind language
  iteration 23 regardless.

## Info

Forks the spec must settle:

1. **What is the writer?** Candidates: a second interface whose method is
   called repeatedly until it signals done; a `Resp` variant carrying a producer
   object instead of a `Text` body; or a handler that receives the connection and
   writes directly (which is what `ws_accept` already does via the 101 hijack
   sentinel). The third is the least new machinery and the most footgun. The
   first fits the no-closures doctrine best, since a producer is just another
   class with fields.
2. **Does the `after` chain still run for streamed responses?** It must, or
   security headers regress. But it cannot run *after* the body. So either
   `after` runs at commit time for streaming routes, or streaming routes declare
   they opt out and the framework refuses to combine them with header-mutating
   middleware. Silent partial application is the one unacceptable answer.
3. **Is chunked request parsing in this iteration at all?** It is separable and
   it is the riskiest security surface in the framework. Splitting phase D into
   its own iteration is a legitimate outcome of the brainstorm — the study is
   explicit that the current refusal is *correct*, so there is no pressure to
   rush it.
