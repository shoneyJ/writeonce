# Chat + actor lifecycle — iteration 24 (absorbing 31) design

> **Status: APPROVED 2026-08-23; plan ready**
> ([`../plans/2026-08-23-chat-ws-lifecycle.md`](../plans/2026-08-23-chat-ws-lifecycle.md)).
> Iterations 24 (chat: WebSocket pub/sub
> workload) and 31 (actor lifecycle) ship as ONE iteration by developer
> directive 2026-08-23 — chat builds request/response, backpressure,
> death notices, and timers as it needs them; the recorded chain
> "31 → 24" collapses into "24". Story 34's fork is resolved here too
> (C builtins, full set). Stories:
> [24](../../stories/language-runtime-database/24-chat-websocket-workload.md) ·
> [31](../../stories/language-runtime-database/31-actor-lifecycle.md) ·
> [34](../../stories/language-runtime-database/34-crypto-builtins.md).
> Substrate: the landed 8+11 arc
> ([spec](2026-08-20-shard-fiber-arc-design.md)); every decision below
> reuses its machinery rather than growing parallel machinery.

## The decisions (developer, 2026-08-23)

1. **24 absorbs 31.** One iteration, one plan. The four lifecycle
   mechanisms are built to chat's need, not speculatively.
2. **Crypto (34): C builtins now, full set** — SHA-1, SHA-256,
   HMAC-SHA256. Hand-rolled, libc-only, whole-value.
3. **Request/response: `call(addr, msg)` parks, reply is typed** by the
   receive method's return type.
4. **Backpressure: fail the send** — a full mailbox traps the sender,
   catchably. One policy, not configurable.
5. **Death: monitor only** — one-way notice, delivered as an ordinary
   typed message the observer chose.
6. **Timers: `time.after` one-shot, no cancel** — stale-timer handling
   is the generation-counter idiom in `.wo`, documented in the sample.
7. **Serving model: WS-only actors.** The blocking HTTP serve loop is
   untouched; only upgraded connections leave it. Keep-alive retirement
   for plain HTTP stays a future slice.

## Part A — runtime: actor lifecycle

### call / reply

- `call(addr, msg)` is `send` that waits: the message moves to the
  callee exactly as `send` moves it, the caller's fiber parks on
  `WO_PARK_INBOX` (the DB-RPC park, unchanged), and the callee's reply
  value moves back and becomes `call`'s result.
- Two new envelope kinds (5 = call request, 6 = call reply) generalize
  the DB pair (3/4). The request envelope carries the caller's shard +
  fiber so the reply routes home; delivery, adoption, and the
  once-per-slice inbox drain are the arc's existing code paths.
- **Typing:** the reply type is the receive method's declared return
  type. `receive(msg: M) -> R` makes `call` on that actor produce `R`;
  a `receive` with no return type makes `call` a compile error
  (WO-E226). `send` to a returning receive stays legal and discards the
  result. Ownership: the request moves (existing transfer machinery),
  the reply moves back — a reply that is a traced-containing type is
  rejected at compile time exactly as WO-E222 rejects such sends.
- **Death integration (no hangs, ever):** `call` to an already-dead
  actor traps immediately; a callee dying mid-call (trap while the
  caller is parked) unparks the caller into the same trap. Both are
  WO_T_ACTOR (below), catchable.

### Bounded mailboxes

- Every actor mailbox has one fixed cap: 1024 messages, `WO_MAILBOX`
  env override (soak tests shrink it to force the policy). The arc's
  stage-2 deviation 4 (mutex-guarded unbounded list) gains a length
  check — no ring rewrite in this iteration.
- A `send` or `call` arriving at a full mailbox traps the SENDER with
  **WO_T_ACTOR (trap kind 13)**, catchable via the existing try/catch.
  The message is not enqueued; the sender's value is not consumed (the
  trap unwinds before the move completes, same as any trapping
  builtin). Nothing is silently dropped: the sender always learns.

### Monitor

- `monitor(addr, msg)` — the observer supplies an M-typed message (its
  OWN mailbox type); when the watched actor dies, the runtime delivers
  that message to the observer like any send. No new message types, no
  untyped mailbox hole.
- Death v1 = trap-death only (a fiber trap unwinding out of an actor's
  receive). Normal program teardown reaps actors without firing
  monitors — shutdown is not death. Multiple monitors on one actor all
  fire; monitoring an already-dead actor fires immediately; the
  monitor message is subject to the mailbox cap like any send — but
  the "sender" here is the runtime mid-unwind, so a full observer's
  notice is DROPPED with a stderr diagnostic naming both actors
  (disclosed, not hidden; there is no sensible fiber to trap).
- Supervision (respawn) is `.wo` code the sample demonstrates only if
  chat needs it; no runtime restart policy.

### Timers

- `time.after(ms, addr, msg)` — one-shot: after `ms` milliseconds the
  runtime delivers `msg` to `addr` as an ordinary send, riding the
  shard I/O plane's existing timeout arm (arc T4). No cancel builtin;
  the documented idiom is a generation counter in the actor's state —
  a stale timer message that arrives after its purpose passed is
  recognized and ignored by the receive code.
- The armed timer holds the moved message; if the target dies before
  expiry the delivery is a send-to-dead (existing silent-drop rule).

### Lowering + format

- All four surfaces lower to BUILTIN ids (like spawn/send): no new
  opcodes. New ids from 85 up: call, monitor, time.after (and Part B's
  three digests). `WO_B_MAX` moves; `.wob` version stays (the ticks-84
  precedent: pure id additions do not bump — an old runtime rejects a
  new image on the id range check, which is the honest failure).

## Part B — runtime: crypto builtins (story 34 resolved)

- Three builtins over Bytes: `crypto.sha1(bytes)`,
  `crypto.sha256(bytes)`, `crypto.hmac_sha256(key, msg)`, each
  returning fresh Bytes. Hand-rolled C in the runtime (libc-only
  doctrine permits it; the code is bounded and well-specified),
  whole-value like every existing builtin — no streaming interface.
- Unit gate carries the RFC test vectors (SHA-1: RFC 3174; SHA-256:
  FIPS 180-4 vectors; HMAC: RFC 4231) plus empty-input and
  block-boundary lengths.
- SHA-1 exists for the WebSocket handshake (its hard requirement);
  SHA-256/HMAC land in the same slice because the implementation
  shares its skeleton and the ledger's ETag/signed-token rows are
  waiting consumers. No other primitives (no MD5, no SHA-512, no AES)
  — YAGNI until a consumer names them.

## Part C — framework: WebSocket in pure `.wo`

### Upgrade (handler-owned — no generic spawn seam)

- `spawn` takes a class literal, so the framework cannot spawn an
  app-defined connection actor. Inversion: the APP's route handler owns
  the upgrade. The framework provides, in `http/ws.wo`:
  - an upgrade validator (Upgrade/Connection/Sec-WebSocket-Key/version
    headers, RFC 6455 §4.2.1),
  - the accept-key computation — base64 (builtin 80) of SHA-1 (Part B)
    of key + the RFC GUID,
  - a `ws_accept`-shaped function that writes the 101 response on the
    connection and returns the connection fd (Int) for the handler to
    move into ITS actors,
  - and a hijack sentinel: the serve loop, seeing it, neither
    serializes a response nor closes the fd — it forgets the
    connection and returns to accept. Non-upgrade requests are
    byte-identical to today (web-app gates prove it).
- The Req type grows the internal connection handle to make this
  possible; it is not part of the public surface beyond `ws_accept`.

### Frame codec (pure `.wo`, Bytes, iteration-36 bitwise)

- Parse and serialize: text, close, ping, pong; binary accepted and
  echoed only. Client-to-server masking (XOR over the payload with the
  32-bit key) uses the landed bitwise operators. Fragmentation: v1
  rejects fragmented messages with a close frame (documented limit);
  control frames interleaved between data frames are handled per RFC.
  Payload lengths: 7-bit and 16-bit accepted; 64-bit lengths answered
  with close (BODY_MAX-scale bound, same doctrine as HTTP).
- The codec is a pure function library over Bytes — no fd, no actor —
  so its tests are plain corpus-style probes.

### Two actors per connection

- One actor cannot both block in `net.read` and hear room broadcasts
  (one message at a time is the actor contract). So: a READER actor —
  the fd's only reader; parses frames, forwards inbound text to the
  room, answers ping with pong via the writer, sees close/EOF and
  tells room (leave) and writer (close) — and a WRITER actor — the
  fd's only writer; receives broadcast/system/close messages and
  serializes frames. Single-writer discipline means no interleaved
  partial frames; single-reader means no torn parses. Both are
  app-side classes (the chat sample's), placed round-robin across
  shards by the existing spawn — connection load spreads without any
  placement surface.

## Part D — the chat sample (docs/examples/chat)

- Actors: a REGISTRY (name → room address; conn actors `call` it — the
  first real `call` consumer) and a ROOM per name (member list =
  writer-actor addresses — plain scalars; join/leave/broadcast;
  presence lines on join/leave). Message history: none (out of scope).
- HTTP surface: `/` answers JSON usage (JSON-first doctrine, no
  templates); `/ws` upgrades; everything served by the framework
  through `[deps]` exactly as web-app.
- Shutdown: SIGTERM → serve loop stops accepting; rooms broadcast a
  close; writers send close frames; readers see EOF; every fiber
  unwinds (ASan zero leaks) and the process exits 0.
- The gate (`just chat`, scripts/chat-accept.sh + a python client
  speaking raw RFC 6455 over stdlib sockets — no dependencies):
  1. functional: two clients, different shards (TID-asserted), one
     room — a send reaches the other; a third client in another room
     receives nothing.
  2. handshake vectors: the RFC 6455 example key round-trips.
  3. soak: 1k concurrent clients, one hot room, every room progresses
     (reduction budget proof), RSS bounded (mailbox cap engaged, the
     WO_MAILBOX override shrinks it to force the trap path).
  4. drain: SIGTERM with clients connected — close frames observed,
     exit 0, ASan-clean run repeated under both WO_IO backends.
  5. the standing battery stays green (HTTP and WS share serve.wo
     honestly).

## Diagnostics (new)

- **WO-E226** — `call` on an actor whose receive declares no return
  type (or `call`'s result used as the wrong type — existing param
  machinery).
- **WO_T_ACTOR (trap 13)** — mailbox full (sender-side), call-to-dead,
  callee-died-mid-call. One trap kind, message names which.
- Monitor/after argument shapes ride existing arity/type checking.

## Out of scope (restated from the stories)

Message persistence/history; auth beyond bearer; permessage-deflate;
fragmented-message assembly; wss (TLS at the proxy — pass-through
documented in the sample README); supervision trees / restart policy;
priorities; timer cancel; cross-process anything; retiring the HTTP
close-when-idle policy (its own slice); mailbox ring rewrite (22's
mutex number stands until 23-scale work).

## Success criteria

1. The chat gate's five checks green at default cores AND `WO_SHARDS=1`.
2. Crypto vectors green; handshake interoperable with a stock client
   (the python client IS one — it computes the accept key
   independently).
3. Lifecycle proofs: a `call` round-trip parks (TID same-shard check),
   a full mailbox traps the sender catchably, a monitored actor's
   death delivers the chosen message, a timer fires as a message —
   each pinned by a corpus fixture or the sample gate.
4. The full standing battery green; the language grew NOTHING —
   `call`/`monitor`/`time.after` are builtins, not keywords.
