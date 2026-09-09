---
track: jarvis
iteration: "1"
status: pending
readiness: ready
review_pending: "forks auto-approved 2026-09-08 for autonomous execution — developer second review before code lands"
---

# jarvis 1 — the chat loop: a prompt in, streamed tokens out, durable history

> Part of [Story — jarvis, the writeonce AI assistant](00-story.md).
> The whole end-to-end seam: a browser sends a prompt, jarvis relays it to an
> LLM over HTTPS, streams the reply back token by token, and persists the
> conversation durably. Nothing past this rung is worth building until it runs.
>
> **Forks auto-approved (2026-09-08) for autonomous execution and flagged in
> `review_pending` for the developer's second review** — the defaults below are
> reasonable but were not individually confirmed.

## The outbound seam has landed

The blockers are cleared: `net.connect` (✅) and runtime-v2
[9](../runtime-v2/09-in-process-tls.md)'s **`net.connect_tls` / `net.read_tls` /
`net.write_tls`** (✅ landed 2026-09-09, live-gated `just tls`) expose outbound
HTTPS from `.wo`, with the full hand-rolled TLS 1.3 handshake + chain/hostname
validation. Everything below is now buildable `.wo` on top of that seam plus
porch 2/3/6/7. (Note: the TLS builtins return the connection as an `Int` fd, per
the F3c-net object-model decision — the chat loop dials with them directly.)

## Decisions locked (auto-approved, review pending)

1. **Backend: the Anthropic Messages API over HTTPS**, streaming (SSE). One
   backend for v1; a thin adapter boundary so an OpenAI-compatible backend can
   slot later without touching the chat loop. Endpoint, model id and version
   header come from config.
2. **Auth: an API key from the environment/config**, sent as the `x-api-key`
   header — never in a URL, never logged. Missing key is a startup refusal, not
   a runtime surprise.
3. **The relay is an actor per conversation.** A conversation actor owns the
   upstream `net.connect_tls` connection, parses the LLM's SSE
   (`content_block_delta` text deltas), and forwards each delta to the browser
   through porch [7](../porch/07-sse-and-compression.md)'s SSE. Backpressure and
   disconnect cleanup follow porch 7's contract (a write to a gone client frees
   the fiber, the upstream fd and the actor).
4. **Durable history in two `@table` classes**: `Conversation { id @unique,
   principal, created_at }` and `Message { conv_id (indexed), seq, role,
   content, created_at }`. Keyed to the session principal (porch
   [3](../porch/03-sessions.md)); history replays after a restart with no
   external store — the writeonce differentiator.
5. **Route surface, session-gated**: `GET /` (chat UI via wo-html/writeonce-view),
   `POST /message` (accept a prompt, append it, start the stream), `GET /stream`
   (SSE of the reply). The POST is CSRF-protected once porch
   [4](../porch/04-csrf.md) is built; until then it is bearer/session-gated and
   the gap is stated.
6. **One turn at a time, no tools.** No function-calling, no retrieval — those
   are iterations 2 and 3. The reply is a single streamed assistant message.

## Phases

- **A — the backend client.** Over `net.connect_tls`, issue the Messages
  request and parse the streamed SSE deltas into text. Adapter boundary isolated
  so the wire format is one file.
- **B — the conversation store.** The two `@table` classes, append-message,
  load-history, list-conversations; durable-replay proven.
- **C — the relay + web surface.** The conversation actor, the porch routes, the
  wo-html chat page, browser SSE of the reply; session-gated.
- **D — the gate and the ledger.** `just` a jarvis sample end to end against a
  **local stub** LLM server (no network in the gate): prompt → streamed reply →
  durable history → restart-replay. Both the happy path and a mid-stream
  disconnect. Record what landed.

## Acceptance Criteria

- **Given** a signed-in user and a prompt, **when** it is sent, **then** the
  reply streams back token by token and both prompt and reply are persisted.
- **Given** a restart, **when** the user returns, **then** their conversation
  history replays from the `@table`, no external store.
- **Given** the client disconnecting mid-stream, **when** the next delta
  arrives, **then** the upstream fd, the fiber and the actor are freed.
- **Given** a missing API key, **when** the app starts, **then** it refuses
  loudly rather than failing at first request.
- **Given** the gate, **when** it runs, **then** it exercises the whole path
  against a local stub with no live network call.

## Out Of Scope

- **Tool use / function calling** — iteration [2](00-story.md).
- **Retrieval / embeddings** — iteration [3](00-story.md).
- **Multiple backends at once / model routing** — sketched in the overview,
  later.
- **The live network in the gate** — the gate uses a local stub server; a real
  API smoke test is a manual, keyed, out-of-gate step.

## Info

Pure `.wo` on porch 2/3/6/7 + the TLS seam; no new runtime work of its own. The
one hard dependency is runtime-v2 9 reaching phase F. The auto-approved forks
(backend choice, auth transport, store schema, route surface) are the developer
second-review items flagged in `review_pending`.
