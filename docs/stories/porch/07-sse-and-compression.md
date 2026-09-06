---
track: porch
iteration: "7"
status: pending
readiness: ready
---

# porch 7 — server-sent events and compression

> Part of [Story — `porch`, the writeonce web framework](00-story.md).
> Source: [the Fiber parity study](../../plan/exploration/fiber/00-fiber-parity.md) §3,
> re-checked 2026-09-06 against `.dev/reference/fiber` (v3, `3ca9a9d`).
> Both halves need [iteration 6](06-streaming-core.md); the compression half also
> needs [iteration 5](05-routing-response-ergonomics.md)'s q-value ranking and
> comma-join `Vary`, and **two new language-track builtins** (decision 2).
> Not dependent on iteration 2, and not exposed to the lang-41 hang.

## Goals

- **SSE, which fits this runtime unusually well.** A room actor already has the
  fan-out shape a live feed needs, and a parked fiber per subscriber costs almost
  nothing on the shard model — so the awkward part of SSE in most frameworks
  (holding thousands of idle connections) is the part writeonce already solved
  with iteration 35's idle deadlines and fiber-per-connection. Fiber ships
  `Retry`, `HeartbeatInterval` and `OnClose`; all three matter, because a proxy
  will silently drop an idle event stream.
- **gzip/deflate, decided honestly.** Iteration 36 landed the bitwise operators,
  so a pure-`.wo` DEFLATE is now expressible — but the brainstorm chose C
  builtins for the hot path (decision 2). Negotiate, never assume: compress only
  when the client accepts the coding, only above a size threshold, and never for
  content that is already compressed.

## Decisions locked (brainstorm 2026-09-06)

1. **An incoherent heartbeat/idle pair is refused at construction.** A heartbeat
   interval at or above the connection's `idle_ms` evicts the very client the
   heartbeat exists to keep — so the SSE endpoint refuses that pair when it is
   built, with a message naming both values. A misconfiguration is a startup
   error, never a flaky-network mystery — the same refuse-the-unconstructible
   discipline story 6 used for streaming plus header-mutating middleware.
2. **The codec is two C builtins: `deflate` (raw) and `crc32`; gzip framing is
   assembled in `.wo`.** This is the iteration's language-track work, called out
   like iteration 2's phase A. The brainstorm chose performance over the
   pure-`.wo` option: LZ77 match-finding and a per-byte CRC table are inner-loop
   heavy and belong in C. They live in the crypto-family builtin table beside
   `sha256`, hand-rolled with **no zlib dependency** (the `sha256` precedent).
   `deflate` returns the raw stream; `crc32` returns the 32-bit checksum; the
   gzip container (10-byte header, deflate body, little-endian CRC32 and ISIZE
   trailer) is built in `.wo` with the iteration-36 bit operators. Decompression
   (`inflate`) is not built — request-body decompression is out of scope.
3. **The ETag is computed over the uncompressed bytes, before compression, and
   paired with `Vary: Accept-Encoding`.** `etag_for`/`with_etag` run on the
   handler's uncompressed body as today; the middleware compresses afterward. The
   ETag identifies the resource and is stable across encodings; `Vary` keys
   caches on the encoding so a gzipped body never reaches an identity-only client;
   a 304 carries no body, so the encoding is moot on a conditional hit. The
   per-representation/weak-ETag alternative is more RFC-strict but makes the
   middleware rewrite the handler's tag and is easy to get wrong — not chosen.
4. **`Last-Event-ID` resumption is not supported this iteration, and says so.**
   Real resumption needs a per-subscriber replay buffer, which no workload asks
   for yet. The client's `Last-Event-ID` is not silently ignored — that would let
   a client believe it resumed when it did not; the endpoint states plainly that
   it does not resume.
5. **`Vary` accumulation uses iteration 5's comma-join, not iteration 2.** This
   corrects the draft: story 5 decided `Vary` accumulates by comma-joining in the
   existing header map, so the compression middleware composes with other `Vary`
   contributors without iteration 2's repeated-header work.

## Phases

### Phase A — SSE framing and lifecycle

- The event framing (`data:`, `event:`, `id:`, `retry:`), the double-newline
  terminator, and the `text/event-stream` content type with caching disabled,
  emitted through iteration 6's streaming writer.
- Heartbeats, with the construction-time refusal from decision 1 guarding the
  interaction with iteration 35's `idle_ms`.
- Disconnect detection and cleanup: a write to a gone client must free the fiber,
  the actor subscription and the fd, on every path.
- Verify: a client receives ordered events; a heartbeat keeps an otherwise idle
  stream alive past `idle_ms`; a disconnect releases everything (fd count flat).

### Phase B — an SSE workload worth gating

- A live feed in a sample fed by an actor, so the fan-out path is real rather
  than a loop in one handler.
- `Last-Event-ID` handling per decision 4: not supported, stated explicitly.
- Verify: N concurrent subscribers all receive an event published once; fds and
  memory flat across a churn of connect/disconnect.

### Phase C — the compression codec (language-track work)

- Add the `deflate` and `crc32` builtins (decision 2), hand-rolled in C with no
  external dependency, documented in the builtin-surface contract and error
  catalog in the same change. CRC32 is the consumer the crypto row has listed as
  waiting.
- Assemble the gzip container in `.wo`: header, deflate stream, CRC32 and ISIZE
  trailer.
- Correctness against a reference decompressor is the acceptance bar, on awkward
  input: empty, highly repetitive, incompressible, and larger than any internal
  buffer.
- Verify: `oop-e2e` green for the builtins; `gzip -d` reproduces the input
  byte-exactly for every case.

### Phase D — the compression middleware

- `Accept-Encoding` negotiation reusing iteration 5's q-value ranking, a
  minimum-size threshold, and a content-type skip list.
- `Vary: Accept-Encoding` on anything compressed, accumulated by comma-join
  (decision 5) so it coexists with other `Vary` contributions.
- Composition with chunked streaming: compress then chunk; the ETag rule from
  decision 3.
- Verify: a compressed response decompresses to the original; `Vary` is present
  and coexists; no double-compression; an already-compressed content type is
  skipped.

### Phase E — the gate and the ledger

- Both serving gates plus an ASan leg for the codec, the part most likely to leak
  or over-read.
- Retire the ledger's SSE and compression rows; record the CRC32 consumer.
- Verify: `just web-app`, `just site`, `just linkcheck` green; ASan clean.

## Acceptance Criteria

- **Given** an SSE endpoint and a subscribed client, **when** events are
  published, **then** the client receives them in order with correct framing.
- **Given** an idle SSE stream and a heartbeat interval shorter than `idle_ms`,
  **when** it idles past the deadline, **then** it stays open.
- **Given** a heartbeat interval at or above `idle_ms`, **when** the endpoint is
  constructed, **then** it is refused with a message naming both values.
- **Given** a client disconnecting mid-stream, **when** the next publish occurs,
  **then** the write failure frees the fiber, the subscription and the fd; fd
  count returns to baseline.
- **Given** N concurrent subscribers, **when** one event is published, **then**
  all N receive it and memory does not grow per event.
- **Given** any input including empty, repetitive and incompressible, **when** it
  is compressed, **then** a reference `gzip -d` reproduces it byte-exactly.
- **Given** a client that did not send `Accept-Encoding`, **when** it requests a
  compressible resource, **then** the response is uncompressed.
- **Given** a compressed response, **when** it leaves, **then** `Vary:
  Accept-Encoding` is set and coexists with any other `Vary` contribution.
- **Given** an already-compressed content type, **when** it is served, **then**
  it is not compressed again.
- **Given** a conditional request for a compressible resource, **when** its
  `If-None-Match` matches the uncompressed-body ETag, **then** a 304 is returned
  regardless of the client's `Accept-Encoding`.

## Out Of Scope

- **Brotli and zstd.** One codec, proven, before a second. gzip is what every
  client accepts.
- **Request-body decompression (`inflate`).** A compressed *upload* is a separate
  surface with its own decompression-bomb risk, and no workload asks yet — so the
  codec builtins are compress-only.
- **`Last-Event-ID` resumption** — decision 4; needs a replay buffer no workload
  wants yet.
- **WebSockets as an SSE alternative.** Already shipped and a different tool; SSE
  is the one-way, proxy-friendly, reconnect-by-default option.
- **Compressing static files at rest.** Fiber's static has `Compress`;
  precompressed-file serving belongs with iteration
  [8](08-static-and-lifecycle.md).
- **A general-purpose zlib library surface.** The builtins are what the middleware
  needs (`deflate`, `crc32`); if a second consumer appears it can argue for
  `inflate` and a library.

## Info

This is the second porch iteration with a language dependency (after iteration
2): two builtins, `deflate` and `crc32`, chosen over the pure-`.wo` path for
hot-path performance. Everything else is pure `.wo` on iteration 6's streaming
writer and iteration 5's negotiation. The codec is pure compute — no actors, no
per-key pool — so, like the rest of the track since iteration 1, it is not
exposed to the lang-41 hang. The risk concentrates in the C codec (ASan leg in
phase E) and in the SSE lifecycle's fd/subscription cleanup.
