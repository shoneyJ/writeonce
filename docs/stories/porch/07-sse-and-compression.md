---
track: porch
iteration: "7"
status: pending
readiness: refine
---

# porch 7 — server-sent events and compression

> Part of [Story — `porch`, the writeonce web framework](00-story.md).
> Source: [the Fiber parity study](../../plan/exploration/fiber/00-fiber-parity.md) §3.
> Both halves need [iteration 6](06-streaming-core.md); neither is possible
> before it.

## Goals

- **SSE, which fits this runtime unusually well.** A room actor already has the
  fan-out shape a live feed needs, and a parked fiber per subscriber costs
  almost nothing on the shard model — so the awkward part of SSE in most
  frameworks (holding thousands of idle connections) is the part writeonce
  already solved with iteration 35's idle deadlines and fiber-per-connection.
  Fiber ships `Retry`, `HeartbeatInterval` and `OnClose`; all three matter,
  because a proxy will silently drop an idle event stream.
- **gzip/deflate, decided honestly.** Iteration 36 landed the bitwise operators,
  so a pure-`.wo` DEFLATE is now *expressible* — the question is whether it
  should be. A C builtin is faster and smaller to write; a `.wo` implementation
  keeps the runtime doctrine intact and proves the language can do real
  bit-level work. Fork 2 decides, and the answer should turn on whether anything
  else will ever want zlib.
- **Negotiate, never assume.** Compress only when the client said it accepts the
  coding, only above a size threshold, and never for content that is already
  compressed — a gzipped JPEG is bigger than the JPEG.

## Phases

### Phase A — SSE framing and lifecycle

- The event framing (`data:`, `event:`, `id:`, `retry:`), the double-newline
  terminator, and the `text/event-stream` content type with caching disabled.
- Heartbeats, because an idle stream through a proxy dies quietly. Interacts
  directly with iteration 35's `idle_ms` — a heartbeat interval longer than the
  idle deadline evicts the client the heartbeat exists to keep.
- Disconnect detection and cleanup: a write to a gone client must free the
  fiber, the actor subscription and the fd, on every path.
- Verify: a client receives ordered events; a heartbeat keeps an otherwise idle
  stream alive past `idle_ms`; a disconnect releases everything (fd count flat).

### Phase B — an SSE workload worth gating

- A live feed in a sample fed by an actor, so the fan-out path is real rather
  than a loop in one handler.
- `Last-Event-ID` resumption, or an explicit statement that it is not supported
  — silently ignoring it means clients think they resumed when they did not.
- Verify: N concurrent subscribers all receive an event published once; fds and
  memory flat across a churn of connect/disconnect.

### Phase C — the compression codec

- Implement or bind the codec per fork 2, with the framing gzip requires
  (header, deflate stream, CRC32 and length trailer). Note CRC32 does not exist
  yet — the crypto row lists it as waiting for a consumer, and this is that
  consumer.
- Correctness against a reference decompressor is the acceptance bar, on
  awkward input: empty, highly repetitive, incompressible, and larger than any
  internal buffer.
- Verify: `gzip -d` reproduces the input byte-exactly for every case.

### Phase D — the compression middleware

- `Accept-Encoding` negotiation reusing the q-value ranking iteration
  [5](05-routing-response-ergonomics.md) added, a minimum-size threshold, and a
  content-type skip list.
- `Vary: Accept-Encoding` on anything compressed, or every cache in front will
  serve gzipped bytes to a client that cannot read them. This needs iteration
  2's repeated-header work to accumulate correctly with other `Vary`
  contributions.
- Composition with chunked streaming: compress then chunk, and the ETag question
  — an ETag computed over compressed bytes is a different entity than the same
  resource uncompressed.
- Verify: a compressed response decompresses to the original; `Vary` is present;
  no double-compression; an already-compressed content type is skipped.

### Phase E — the gate and the ledger

- Both serving gates plus an ASan leg for the codec, which is the part most
  likely to leak or over-read.
- Retire the ledger's SSE and compression rows.
- Verify: `just web-app`, `just site`, `just linkcheck` green; ASan clean.

## Acceptance Criteria

- **Given** an SSE endpoint and a subscribed client, **when** events are
  published, **then** the client receives them in order with correct framing.
- **Given** an idle SSE stream and a heartbeat interval shorter than `idle_ms`,
  **when** it idles past the deadline, **then** it stays open.
- **Given** a heartbeat interval *longer* than `idle_ms`, **when** the stream
  idles, **then** the misconfiguration is evident rather than mysterious — the
  interaction is documented and, ideally, refused at construction.
- **Given** a client disconnecting mid-stream, **when** the next publish
  occurs, **then** the write failure frees the fiber, the subscription and the
  fd; fd count returns to baseline.
- **Given** N concurrent subscribers, **when** one event is published, **then**
  all N receive it and memory does not grow per event.
- **Given** any input including empty, repetitive and incompressible, **when**
  it is compressed, **then** a reference `gzip -d` reproduces it byte-exactly.
- **Given** a client that did not send `Accept-Encoding`, **when** it requests a
  compressible resource, **then** the response is uncompressed.
- **Given** a compressed response, **when** it leaves, **then** `Vary:
  Accept-Encoding` is set and coexists with any other `Vary` contribution.
- **Given** an already-compressed content type, **when** it is served, **then**
  it is not compressed again.

## Out Of Scope

- **Brotli and zstd.** One codec, proven, before a second. gzip is what every
  client accepts.
- **Request-body decompression.** A compressed *upload* is a separate surface
  with its own decompression-bomb risk, and no workload asks yet.
- **WebSockets as an SSE alternative.** Already shipped and a different tool;
  SSE is the one-way, proxy-friendly, reconnect-by-default option.
- **Compressing static files at rest.** Fiber's static has `Compress`;
  precompressed-file serving belongs with iteration
  [8](08-static-and-lifecycle.md).
- **A general-purpose zlib library surface.** Whatever lands is what the
  middleware needs. If a second consumer appears, it can argue for a library.

## Info

Forks the spec must settle:

1. **Heartbeat versus idle deadline.** These two mechanisms can silently fight,
   and the failure looks like a flaky network. Decide whether the framework
   refuses an incoherent pair at construction — leaning yes, because a
   configuration that cannot work should not be constructible.
2. **`.wo` DEFLATE or a C builtin?** The honest tiebreaker is whether anything
   else ever wants zlib. If compression is the only consumer forever, a `.wo`
   implementation keeps the runtime small and is a genuine demonstration that
   iteration 36's bit operators earned their place. If a second consumer is
   plausible (precompressed assets, a WAL codec, an archive format), the builtin
   wins. CRC32 comes along either way.
3. **ETag over compressed or uncompressed bytes?** `etag_for` exists and is used
   with `with_etag` for 304s. Compressing after the ETag is computed keeps the
   entity identity stable across encodings, which is almost certainly right —
   but it must be decided, because getting it wrong serves the wrong body for a
   conditional request.
