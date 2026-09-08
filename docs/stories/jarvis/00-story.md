# Story — `jarvis`, the writeonce AI assistant

The sixth track, and the second whose product is an end-user *program* rather
than a language capability — after [`wmux`](../wmux/00-story.md), the terminal
multiplexer. Where wmux proves writeonce can build the tool a developer lives
in, jarvis proves it can build the tool of the moment: an AI assistant, written
end to end in `.wo`, durable and single-binary by construction. It serves the
same north star — adoption by Linux developers — by meeting them where the
attention is.

Numbering restarts at 1 and is local to this track; frontmatter carries
`track: jarvis`. Status rules are the repo's, unchanged: `status:` in
frontmatter is the only place state lives, no directory encodes it.

## The problem, stated once

An assistant's whole job is to reach a model, and the runtime cannot reach
anything outbound. It learned to *listen* — sockets in 8/11/35, unix sockets and
peer address in 35 and runtime-v2 — but it has never learned to *dial*: there is
no `net.connect` (outbound TCP), confirmed against `runtime/src/wob.h` (the net
builtins stop at listen/accept/read/write plus the unix-socket client), and no
outbound TLS client anywhere. An LLM API is HTTPS on a remote host. jarvis
therefore does not begin until two things exist.

The design chosen is **direct outbound HTTPS** — jarvis dials the LLM API
itself, keeping the pure single-binary story. That gates the whole track on
runtime work, now **partly built**:

- **`net.connect`** — outbound TCP — ✅ **landed 2026-09-07** (`wob.h` id 110,
  `getaddrinfo` DNS + blocking connect; the outbound half language 38 named).
- **An outbound TLS client** — HTTPS over that socket — owned by
  runtime-v2 [9](../runtime-v2/09-in-process-tls.md) (in-process TLS), which
  **retires the standing "TLS is the proxy's job" doctrine**. In progress: its
  crypto foundations are landed and vector-gated — **A AEAD** (ChaCha20-Poly1305
  + AES-GCM, runtime-v2 [8](../runtime-v2/08-symmetric-cipher.md) A–C),
  **B HKDF**, **C X25519**, **D signatures** (RSA PKCS1/PSS + ECDSA-P256) — and
  the remaining rungs (**E** ASN.1/X.509 chain, **F** record layer + handshake
  FSM, **G** server) are what jarvis still waits on.

A **local-gateway alternative was considered and set aside**: jarvis could speak
to a small companion process over a unix socket (`net.connect_unix`, id 107) or
spawn one (`proc.spawn`, id 97) — both exist today — and let that companion do
the HTTPS, exactly as inbound TLS terminates at a proxy. It is buildable now.
It was rejected in favour of the single-binary story, in which the assistant
owns its own connection rather than shipping a second executable.

## Architecture

    browser  ⇄  jarvis (a porch app)  ⇄  net.connect ✅ + TLS (rv2 9, in progress)  ⇄  LLM API

Requests arrive at a porch web app; the answer streams the other way, token by
token, LLM → jarvis → browser, over porch's SSE. Conversation state is durable
in a `@table`, so history survives a restart with no external store — the
writeonce differentiator wmux already showed for session state, applied to chat.

## The iterations

Ordered by dependency; the first rung is the whole end-to-end seam, and nothing
past it is worth building until that seam is proven.

| # | Iteration | Delivers | Needs |
| --- | --- | --- | --- |
| 1 | [the chat loop](01-chat-loop.md) — ✅ `ready` (forks auto-approved, `review_pending`) | a prompt sent to one LLM, tokens streamed back to the browser, the conversation persisted durably | the outbound seam (TLS phase F); porch 2/3/6/7; wo-html |
| 2 | [tool use / the agent loop](02-tool-use.md) — `refine` | function-calling and multi-step orchestration through actors — where "assistant" becomes "agent" | 1 |
| 3 | [retrieval (RAG)](03-retrieval.md) — `refine` | embeddings + vector search over a document set; carries its own sub-gap — an embeddings call over the same outbound path, plus a vector store (pure-`.wo` or a new primitive, decided in that story) | 1, and the embeddings/vector decision |

Sketched, not committed — named so the shape is visible, not to schedule them:
**model routing / multi-model** (choose a backend per request) and an **MCP
client** (jarvis as an MCP host, calling tools over the protocol) — both
on-brand, both later.

Only iteration 1's scope is settled by this overview; every iteration file is
written and refined to `ready` before its code lands, per the repo's story
discipline.

## Dependencies

Consumed, and already `ready` or shipped:

| Needs | From |
| --- | --- |
| signed cookies, session id | porch [2](../porch/02-randomness-and-cookies.md) |
| durable conversation history, revocable sessions | porch [3](../porch/03-sessions.md) + `@table` |
| incremental response writes | porch [6](../porch/06-streaming-core.md) |
| token streaming to the browser | porch [7](../porch/07-sse-and-compression.md) (SSE) |
| the chat UI | `wo-html` / `writeonce-view` |

Blockers, which must land before iteration 1 starts:

| Blocker | Owner | State |
| --- | --- | --- |
| outbound TCP (`net.connect`) | language [38](../language-runtime-database/38-content-platform-capabilities.md) | ✅ **landed 2026-09-07** (`wob.h` id 110) |
| outbound TLS client | runtime-v2 [9](../runtime-v2/09-in-process-tls.md) — in-process TLS; **retires the proxy-termination doctrine** | 🔄 in progress — A AEAD ✅, B HKDF ✅, C X25519 ✅, D signatures ✅ (RSA + ECDSA-P256); **E–G remain** |

## What this track does NOT own

| Not jarvis's | Why |
| --- | --- |
| local, in-process model inference | needs an ML runtime and heavy FFI — against the no-external-dependency doctrine |
| the local-gateway companion process | considered and rejected (above) in favour of the single-binary story |
| voice / audio in or out | a separate surface with its own capture and codec story; no rung asks for it |
| starting before the blockers land | like [porch 9](../porch/09-idempotent-replay.md) waiting on language 41, jarvis waits on the outbound seam — documented, not worked around |

## Review protocol

Same as every track: the developer reads one iteration, approves or amends, and
the next starts only after approval. Each iteration is an unsplittable value
slice with phases, Given/When/Then acceptance criteria, and an out-of-scope
list, proven by a gate before it is called done.
