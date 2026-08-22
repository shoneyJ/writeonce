# Chat + actor lifecycle — implementation plan (iteration 24, absorbing 31 + 34)

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans task-by-task. Steps use checkbox syntax.
>
> **Style rule (user convention):** concept, reason, required behavior in
> words plus verification commands only — the executor writes the code.

**Goal:** the chat workload proves the concurrency arc end to end —
`call`/monitor/timers/bounded mailboxes in the runtime, crypto builtins,
a pure-`.wo` WebSocket layer in the framework, and a rooms+presence chat
sample gated at 1k clients.

**Architecture:** everything reuses the arc's machinery — `call` rides
the DB-RPC envelope+park pair, timers ride the T4 timeout plumbing,
death rides the existing trap-unwind path. The framework never spawns
app classes: the app's handler owns the upgrade and moves the fd (Int)
into its own reader/writer actor pair. See the spec (normative):
[`../specs/2026-08-23-chat-websocket-actor-lifecycle-design.md`](../specs/2026-08-23-chat-websocket-actor-lifecycle-design.md).

**Tech Stack:** C11 libc-only (`wovm`), OCaml stdlib-only (`woc`),
pure-`.wo` framework code, bash + python3-stdlib gates.

## Global Constraints

- Branch `chat-ws-lifecycle` (this one); commits local only, never push.
- ALWAYS `just woc-build && just wovm-build` before any gate run — gate
  scripts require built binaries and never rebuild (stale-binary
  incident, 2026-08-22).
- A stage does not start until the previous stage's full battery is
  green: `just woc-test wovm-test oop-e2e deps-accept web-app
  log-watcher employee fibers db-actor db-bench-quick` (run as separate
  recipes).
- New builtin ids: 85 `crypto.sha1`, 86 `crypto.sha256`,
  87 `crypto.hmac_sha256`, 88 `call`, 89 `monitor`, 90 `time.after`.
  `WO_B_MAX` follows. NO `.wob` version bump (ticks-84 precedent: pure
  id additions; old runtimes reject on the id-range check).
- New trap kind: `WO_T_ACTOR = 13`. New diagnostic: WO-E226.
- No new opcodes, no new keywords — `call`/`monitor`/`time.after` are
  builtins resolved like `spawn`/`send`/`time.sleep`.
- Plain-HTTP serving stays byte-identical throughout — `just web-app`
  is the canary in every stage.

## Spec refinements (disclosed, decided here)

1. **Reply-type erasure rule.** `actor M` does not name the class, so
   `call`'s static type comes from a program-wide agreement check:
   every `receive(msg: M) -> R` for a given M must declare the same R;
   two classes disagreeing is WO-E226 naming both. A `receive` with no
   return type makes `call` on that M a WO-E226 at the call site.
2. **Cross-shard cap check.** The mailbox cap is enforced through a
   per-actor ATOMIC queue-length counter readable from any shard;
   send/call check it before enqueue and trap `WO_T_ACTOR` when at cap.
   Racing senders can overshoot by at most the number of in-flight
   sends — bounded, disclosed; RSS stays flat under the soak.
3. **Runtime-sourced deliveries** (monitor notices, timer messages)
   have no fiber to trap: delivery to a full mailbox is dropped with a
   stderr diagnostic naming both actors (spec's monitor wording,
   applied to timers too).

---

## Stage 1 — crypto builtins (Part B; independent, smallest risk)

### Task 1 — sha1 / sha256 / hmac_sha256

**Files:**
- Create: `runtime/src/crypto.c` (the three digests, hand-rolled,
  libc-only — one file, shares the block-schedule skeleton)
- Modify: `runtime/src/wob.h` (ids 85–87, `WO_B_MAX 87`, doc comments
  in the builtin roster), `runtime/src/builtin.c` (forward the id range
  to the crypto entry point), `runtime/Makefile` (new object)
- Modify: `compiler/src/types.ml` (the `crypto.sha1|sha256|hmac_sha256`
  names → ids, arity/typing: Bytes→Bytes and Bytes,Bytes→Bytes —
  follow exactly how `base64.encode`-family names map)
- Create: `runtime/test/test_crypto.c` (RFC vectors),
  `tests/corpus/run/crypto-digests/` fixture pair
- Modify: `docs/plan/oop-vm/08-builtin-surface.md` (three rows)

**Interfaces:**
- Produces: builtins callable from `.wo` as `crypto.sha1(b)`,
  `crypto.sha256(b)`, `crypto.hmac_sha256(key, msg)`, each returning
  fresh Bytes; C entry `wo_builtin_crypto(vm, R, ins, msg)` consumed by
  `builtin.c`'s dispatch. Task 6 consumes `crypto.sha1` from `.wo`.

- [ ] Create the board marker `docs/in-progress/2026-08-23-chat-ws-lifecycle.md`
  (slice active, links to spec+plan) and flip the board's In-progress
  Runtime row to this slice. Commit with the first code commit.
- [ ] Digest cores in `crypto.c`: SHA-1 and SHA-256 over one buffer
  (init/update-once/final collapsed — whole-value contract), HMAC as
  the RFC 2104 two-pass over SHA-256. Wrong-class-id argument traps
  WO_T_BOUNDS with the same message shape the Bytes builtins use.
- [ ] `test_crypto.c`: RFC 3174 SHA-1 vectors ("abc", the 56-byte
  chaining case, empty input), FIPS 180-4 SHA-256 vectors (same
  three), RFC 4231 HMAC cases 1–4, plus a 63/64/65-byte block-boundary
  sweep asserting against python3 hashlib-precomputed constants (put
  the generator one-liner in a comment). Wire into `make -C runtime test`.
- [ ] Verify: `just wovm-build && just wovm-test` green (ASan+UBSan
  stage included).
- [ ] Compiler surface + corpus fixture: a `.wo` program hashing "abc"
  through all three and printing base64 of each (exercises 19's
  encode); expected output = precomputed. Verify: `just woc-build &&
  just woc-test && just oop-e2e`.
- [ ] Full battery. Commit (bullets: ids 85–87, vectors, surface doc).

## Stage 2 — actor lifecycle (Part A)

### Task 2 — bounded mailboxes + WO_T_ACTOR

**Files:**
- Modify: `runtime/src/wob.h` (trap kind 13 + roster comment),
  `runtime/src/vm.h` (per-actor atomic queue length, the cap constant,
  `WO_MAILBOX` plumbing), `runtime/src/vm.c` (check in the same-shard
  enqueue AND in `inbox_push_to`'s caller path before the envelope is
  built; trap message names the actor and the cap)
- Create: `runtime/test/test_mailbox.c`,
  `tests/corpus/run/mailbox-full-trap/` (WO_MAILBOX=4 in its runner
  env, sender catches the trap and prints proof)

**Interfaces:**
- Produces: `WO_T_ACTOR` trap reachable from `.wo` via try/catch on
  send; the atomic length counter Task 3's call path reuses.
- Consumes: arc stage-2 mailbox/inbox structures as they are (mutex
  list stays — no ring rewrite, spec's out-of-scope).

- [ ] Cap default 1024; `WO_MAILBOX` env override parsed once at engine
  start (same pattern as `WO_SHARDS`). Counter increments at enqueue,
  decrements when receive DEQUEUES (not when it finishes).
- [ ] `test_mailbox.c`: fill to cap, next send returns the trap;
  dequeue one, send succeeds; two threads racing the last slot never
  lose a message and never exceed cap + in-flight (assert the bound,
  not exactness).
- [ ] Corpus fixture: catchable trap proven from `.wo`; deterministic
  under `WO_SHARDS=1`.
- [ ] Verify: `just wovm-build && just wovm-test && just oop-e2e`,
  then full battery (unchanged cap = no behavior change anywhere
  else — `just fibers` and `just db-actor` are the canaries).
- [ ] Commit.

### Task 3 — call / reply

**Files:**
- Modify: `runtime/src/wob.h` (id 88), `runtime/src/vm.c` (envelope
  kinds 5 request / 6 reply generalizing the DB pair: kind-5 carries
  caller shard+fiber and the moved message; adoption runs the actor's
  receive for it and ships kind-6 with the moved return value; the
  caller parks `WO_PARK_INBOX` and re-executes the builtin to consume
  the reply — mirror `wo_db_rpc`'s shape), `runtime/src/builtin.c`
  (the call case), `runtime/src/vm.h` (pending-call bookkeeping on the
  fiber)
- Modify: `compiler/src/types.ml` (WO-E226: the program-wide
  `receive(M) -> R` agreement table, call-site typing `call(actor M,
  M) -> R`, call-on-void-receive error), `compiler/src/emit.ml`
  (lower to id 88 — same shape as send), `compiler/src/diag.ml`
  (E226 text)
- Create: corpus `run/call-echo` (same-shard round trip, TID-printed
  park proof), `run/call-cross-shard` (output-set assertion),
  `run/call-dead-trap` (call after callee trap-died → caught
  WO_T_ACTOR; callee dies mid-call → caught), `compile-fail/call-void-receive`,
  `compile-fail/call-reply-disagree` (two classes, same M, different R)

**Interfaces:**
- Consumes: Task 2's atomic length check (call is a send first).
- Produces: `call(addr, msg) -> R` callable from `.wo`; the
  kind-5/6 envelope pair; death-unparks-caller hook that Task 4's
  death machinery triggers. Task 8's registry lookups consume `call`.

- [ ] Runtime first (unit-provable without the compiler): kind-5/6
  paths + park/resume + dead-target immediate trap + die-mid-call
  unpark-to-trap. Extend `runtime/test/test_fiber.c` with a
  hand-built call round trip and a die-mid-call case (ASan).
- [ ] Compiler: the agreement table is built in the same pass that
  already collects `receive` signatures for spawn/WO-E221; E226 fires
  on disagreement (both class names in the message) and on
  call-of-void. Reply values face the same WO-E222
  traced-containment check as send arguments — extend that check to
  receive RETURN types reachable via call, at the receive site.
- [ ] Corpus fixtures above; ASan AND TSan on the actor corpus
  (`just fibers` carries the TSan lane).
- [ ] Verify: `just woc-test && just oop-e2e && just fibers &&
  just db-actor`, then full battery. Commit.

### Task 4 — monitor

**Files:**
- Modify: `runtime/src/wob.h` (id 89), `runtime/src/vm.c` (per-actor
  monitor list: observer address + the moved notice message; the
  fiber-trap unwind path that already isolates actor death walks the
  list and delivers each notice as an ordinary send from runtime
  context — full observer = drop + stderr line naming both actors;
  monitor-of-dead delivers immediately; also unpark any caller parked
  in a kind-5 call on the dying actor into WO_T_ACTOR — closing
  Task 3's hook), `runtime/src/builtin.c` (the monitor case)
- Modify: `compiler/src/types.ml` (arity/typing: `monitor(actor M2,
  msg: M1)` where M1 is the OBSERVER's mailbox type — the msg argument
  is typed against the observer address's M... the observer is the
  CALLER: v1 rule, monitor's msg must be the type some actor the
  caller names receives; concretely `monitor(watched, observer, msg)`
  three-argument form so the target mailbox is explicit and typed),
  `compiler/src/emit.ml`
- Create: corpus `run/monitor-death` (watched actor traps; observer
  prints the notice; deterministic at WO_SHARDS=1),
  `run/monitor-already-dead`

**Interfaces:**
- Consumes: Task 3's pending-call bookkeeping (die-mid-call unpark).
- Produces: `monitor(watched, observer, msg)` from `.wo`; the death
  walk Task 8's rooms use to drop dead members.

- [ ] Note the spec deviation and disclose it in the commit: the spec
  wrote two-argument monitor with the caller as implicit observer, but
  the caller of `monitor` may be plain `main` (no mailbox) — the
  three-argument form names the observer address explicitly and stays
  fully typed. Story banner records it at closeout.
- [ ] Runtime + compiler + fixtures as above.
- [ ] Verify: `just oop-e2e && just fibers`, full battery. Commit.

### Task 5 — time.after

**Files:**
- Modify: `runtime/src/wob.h` (id 90), `runtime/src/sysio.c` or
  `runtime/src/park.c` (whichever owns the T4 deadline scan — arm a
  timer entry: deadline + target address + moved message; expiry
  delivers as an ordinary runtime send: full mailbox = drop +
  diagnostic, dead target = existing silent-drop),
  `runtime/src/vm.h` (the shard's timer list), `compiler/src/types.ml`
  + `emit.ml` (`time.after(ms, addr, msg)`)
- Create: corpus `run/timer-delivery` (arm 30ms, actor prints on
  receipt, main outlives it — loose ordering like the fibers demo
  part 2), `run/timer-generation` (the documented cancel idiom: arm
  two, bump the generation, prove the stale one is ignored)

**Interfaces:**
- Produces: `time.after(ms, addr, msg)`; Task 8's presence/ping logic
  consumes it.

- [ ] Timer list lives on the ARMING fiber's shard and rides that
  shard's existing io_uring/epoll timeout arm — no new wait machinery;
  delivery crosses shards through the normal envelope path when the
  target lives elsewhere.
- [ ] Verify: `just oop-e2e && just fibers` on BOTH `WO_IO` backends
  (the fibers gate already forces both), full battery. Commit.
  **Stage 2 complete: board note.**

## Stage 3 — framework WebSocket (Part C)

### Task 6 — upgrade seam (ws_accept + hijack)

**Files:**
- Modify: `docs/examples/writeonce-framework/http/types.wo` (Req grows
  the internal conn handle; internal-only — document it as not public
  surface), `docs/examples/writeonce-framework/internal/serve.wo`
  (pass the conn into Req; recognize the hijack sentinel — the
  Resp status 101 — and neither serialize nor close, just return to
  accept), `docs/examples/writeonce-framework/internal/parse.wo` (no
  behavior change — only whatever plumbing Req's new field needs)
- Create: `docs/examples/writeonce-framework/http/ws.wo` — upgrade
  validation (RFC 6455 §4.2.1: method GET, `Upgrade: websocket`,
  `Connection` contains upgrade, `Sec-WebSocket-Version: 13`, the key
  header present), accept-key = `base64.encode(crypto.sha1(key ++
  GUID))` with the RFC GUID constant, `ws_accept(req)` writes the 101
  with the computed key and returns the fd; malformed upgrade returns
  nil (handler answers a plain 400 — nothing traps)
- Modify: framework probe scripts (whichever pattern auth.wo's 26-case
  matrix uses — add handshake cases: the RFC 6455 worked example key
  `dGhlIHNhbXBsZSBub25jZQ==` must produce
  `s3pPLMBiTxaQ9kYGzzhZRbK+xOo=`, plus each missing-header rejection)

**Interfaces:**
- Consumes: Task 1's `crypto.sha1`, builtin 80 base64.
- Produces: `ws_accept(req) -> ?Int` (nil = not a valid upgrade) and
  the 101-sentinel contract with serve.wo; Task 8's `/ws` handler
  consumes both.

- [ ] Verify handshake probe vector; then `just web-app` (the
  byte-identical canary — no HTTP behavior may move) and `just
  deps-accept`. Full battery. Commit.

### Task 7 — frame codec (pure `.wo`)

**Files:**
- Create: `docs/examples/writeonce-framework/http/wsframe.wo` — parse
  one frame from Bytes (fin/opcode/mask/len; 7-bit and 16-bit lengths;
  64-bit length → a close verdict; unmasking via bitwise XOR;
  fragmented data frames → close verdict; control frames legal between
  data frames), serialize text/close/ping/pong (server frames
  unmasked, per RFC), and an incremental feeder shape: a carry buffer
  so a reader can accumulate `net.read` chunks and pull complete
  frames — mirrors parse.wo's carry convention
- Create: codec probes (fixture-style like the multipart probes):
  masked "Hello" round trip (the RFC 6455 example bytes), 16-bit
  length boundary at 126, oversize close verdict, ping between
  fragments of nothing (control-frame interleave), torn-buffer
  reassembly across three feeds

**Interfaces:**
- Produces: frame parse/serialize functions over Bytes + the carry
  convention; Task 8's reader/writer consume them. No fd, no actor —
  pure functions.

- [ ] Verify probes + full battery (framework compiles = deps gates).
  Commit.

## Stage 4 — the chat sample (Part D)

### Task 8 — docs/examples/chat

**Files:**
- Create: `docs/examples/chat/wo.toml` ([deps] on the framework — copy
  web-app's shape), `docs/examples/chat/main.wo` (or a small module
  split if main crowds 200 lines: `actors.wo` for
  registry/room/reader/writer classes)
- Registry actor: map name → room address; a Lookup request answered
  through `call` (the first honest consumer — reply is the room
  address, a scalar). Rooms spawned on demand.
- Room actor: members = multi of writer addresses; Join/Leave add and
  remove + broadcast presence lines; a text message broadcasts to all
  writers; a full writer mailbox trap at broadcast = that member is
  dropped (catch, remove, close) — the backpressure policy earning its
  keep; `monitor` on writers so a died writer leaves the room.
- Reader actor: owns the fd read loop with wsframe's carry; text →
  room; ping → writer sends pong; close/EOF → Leave to room, Close to
  writer.
- Writer actor: sole fd writer; Text/Presence/Pong/Close messages →
  serialized frames; Close also closes the fd.
- main: spawn registry, register the `/ws` handler (query params name
  the room and user; `ws_accept`; spawn reader+writer with fd +
  addresses; Join via the room), `/` answers JSON usage; serve; on
  `env.stopping()` the serve loop returns — main sends Shutdown
  through registry → rooms broadcast close → writers flush close
  frames — then main returns (the reap).

**Interfaces:**
- Consumes: everything Tasks 1–7 produced, by exact name.
- Produces: the running sample Task 9 gates.

- [ ] Manual smoke: build, connect with the Task 9 python client
  prototype, two clients two shards, exchange lines. Verify
  `WO_SHARDS=1` byte-order determinism for the single-client script.
- [ ] Commit (sample alone — the gate lands next so a reviewer can
  run the sample by hand first).

### Task 9 — the chat gate

**Files:**
- Create: `scripts/chat-accept.sh` + `scripts/ws_client.py`
  (python3 stdlib only: socket, base64, hashlib, os, threading —
  speaks the handshake with an independently computed accept-key
  check, masks client frames, reads server frames)
- Modify: `justfile` (`chat` recipe), `scripts/` battery docs if the
  repo lists gates anywhere beside the justfile

**Checks (the spec's five, exactly):**
1. functional: two clients, one room, cross-shard TID assert, third
   client in another room silent;
2. handshake: the RFC worked-example key verified by the client
   itself;
3. soak: 1k clients, one hot room, every room progresses; RSS bound
   asserted; a `WO_MAILBOX=8` sub-run proving the drop-slow-member
   path fires and the room survives;
4. drain: SIGTERM with connected clients → close frames observed →
   exit 0; repeated under `WO_IO=uring` and `WO_IO=epoll`, once under
   the ASan build (zero leaks);
5. `just web-app` byte-identical plus the full battery green.

- [ ] Verify: `just chat` 5/5 at default cores AND `WO_SHARDS=1`.
- [ ] Full battery. Commit. **Stage 4 complete.**

## Stage 5 — closeout

### Task 10 — docs, stories, board, graph

- [ ] Stories: 24 → `done/` with the landing banner (what landed, gate
  numbers, the monitor three-argument deviation, the reply-agreement
  rule); 31 → `done/` with a banner saying it landed INSIDE 24 (the
  four forks and their decisions, link to the spec); 34 → `done/`
  (C-builtin resolution, ids, vectors). Frontmatter status + folder
  move together (house rule).
- [ ] Board: In-progress row cleared (marker doc deleted), Landed
  entries standup-shaped (the six questions), chain note: next is 23
  (io_uring group-commit) with 22's numbers in hand.
- [ ] Graph: PUBSUB2/KEEPAL-adjacent nodes — PUBSUB2 done; CRYPTO gate
  done (SHA/ETag row unblocked, not built); framework README ledger:
  WebSocket/pub-sub rows ✅, ETag row's gate cleared, cancellation row
  unblocked-not-built; `media_type`/streaming rows untouched.
- [ ] CODE-LOGIC files: `runtime/src/CODE-LOGIC.md` (lifecycle
  section: call envelopes, cap counter, monitor walk, timer list;
  crypto section: one paragraph, vectors pointer),
  `docs/examples/chat/CODE-LOGIC.md` (actor topology, the
  two-actors-per-connection reason, shutdown choreography).
- [ ] Full battery once more after doc edits. Commit.

## Success criteria

The spec's four, verbatim: chat gate 5/5 both shard counts; crypto
vectors + independent-client handshake; the four lifecycle proofs
pinned; full battery green with zero language growth (builtins only).

## Self-review notes

- Spec coverage: Part A → T2–T5, Part B → T1, Part C → T6–T7,
  Part D → T8–T9, diagnostics WO-E226 (T3) / WO_T_ACTOR (T2–T4),
  closeout obligations → T10.
- Two spec deviations pre-disclosed: monitor's three-argument form
  (T4) and the reply-agreement rule + atomic cap counter (header).
- Names used consistently: `crypto.sha1/sha256/hmac_sha256`, `call`,
  `monitor(watched, observer, msg)`, `time.after(ms, addr, msg)`,
  `WO_T_ACTOR`, `WO-E226`, `WO_MAILBOX`, `ws_accept`, ids 85–90.
- Riskiest surgery is T3 (typing through erasure) — it sits behind two
  green stages and its compile-fail fixtures are written with it.
