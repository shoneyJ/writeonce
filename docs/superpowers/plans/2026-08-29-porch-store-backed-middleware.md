# porch 1 — store-backed middleware: implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: use
> `superpowers:subagent-driven-development` or `superpowers:executing-plans` to
> work this task-by-task. Steps are checkboxes.
>
> **House convention: this plan carries concept, reason and actions in words —
> no implementation or test code blocks.** The engineer writes the code; the
> plan says what must be true and why, and names every file and symbol
> involved. Exact strings that must match (error text, header names) are quoted
> inline.

**Goal:** rate limiting and idempotent replay, both durable across a restart,
both serialized per key through an actor pool so neither loses a write to a
concurrent duplicate.

**Architecture:** a fixed pool of identical actors selected by hash of the key.
Actors own serialization and volatile state; `@table` rows own durability. For
idempotency the actor **runs the handler itself**, so a duplicate waits in the
mailbox rather than needing a reply to be held — `call`'s reply is the return
value of `receive`, and there is no deferred-reply primitive.

**Tech stack:** writeonce `.wo` under `docs/examples/porch/`, the embedded
`@table` store, and the actor builtins `spawn` (68), `send` (69), `call` (88),
`monitor` (89), `time.after` (90) — all landed.

**Spec:**
[`2026-08-29-porch-store-backed-middleware-design.md`](../specs/2026-08-29-porch-store-backed-middleware-design.md)

## Global constraints

- Window arithmetic uses `time.ticks()` — µs, monotonic. A wall-clock jump must
  never grant an extra window.
- Any header carrying a timestamp to a client uses `time.now()` — wall clock.
  Monotonic ticks are seconds-since-boot and meaningless to a client.
- Mutating a stored row is a field assignment on the row, which writes through
  and maintains indexes. Never delete-then-insert **as a way to update**: it
  doubles WAL traffic and leaves a window where a failed insert after a
  successful delete loses the row. This does not forbid deleting a row you
  genuinely mean to remove — pruning an expired row is a plain delete and is
  required.
- A saturated mailbox (`call` trapping `WO_T_ACTOR`) answers **503** with
  `Retry-After`, for both features. Saturation must never become the limiter's
  bypass.
- Never swallow a store failure with an empty catch. The current limiter's
  `catch (e) nil` is how a lost counter becomes silent.
- `woc docs/examples/porch/` must exit 0 after every task. It is a library with
  no entry point; a compile error there breaks every downstream example.
- Gates: `just web-app`, `just site`, `just linkcheck` all green at the end.

## File structure

| File | Responsibility |
| --- | --- |
| `docs/examples/porch/middleware/store.wo` | the two `@table` classes and nothing else. Gains a digest column on the idempotency row. |
| `docs/examples/porch/middleware/keypool.wo` | **new.** The actor, its message classes, and the hash-to-shard selection. The only file that knows a pool exists. |
| `docs/examples/porch/middleware/limiter.wo` | the `Limiter` middleware: key selection, and a `call` into the pool. Holds no counting logic. |
| `docs/examples/porch/middleware/idempotent.wo` | the `Idempotent` middleware: digest computation, and a `call` into the pool carrying the route's `Handler`. |
| `scripts/web-app-accept.sh` | the gate legs, including the restart and concurrency legs. |
| `docs/examples/porch/README.md` | ledger rows moved from 🔶 to ✅ only when their gate leg exists. |

Counting and replay logic lives in `keypool.wo` alone. The two middlewares
become thin: they decide a key and delegate. That is what stops sessions and
CSRF from each re-implementing serialization in iterations 2 and 3.

---

### Task 1 — the store gains a digest column

**Files:** modify `docs/examples/porch/middleware/store.wo`.

**Produces:** an `IdempotencyKey` row carrying `digest: Text` alongside `key`,
`response` and `created_at`.

- [ ] **Step 1 — add the column.** Add `digest: Text` to `IdempotencyKey`. The
      lookup key becomes the **bare** idempotency key; the digest of
      method+path+body is data, not part of the identity. Fold the digest into
      the key and "reused key, different body" becomes undetectable — nothing
      ever looks the bare key up, so nothing can refuse.
- [ ] **Step 2 — check it compiles.** Run `woc docs/examples/porch/`. Expect
      exit 0. A `.wo` library typechecks entry-less.
- [ ] **Step 3 — confirm the annotation is unchanged.** Both tables stay
      `durable: true` and fully resident. `resident: keys` is refused at load
      today and is not wanted here anyway: these tables are small and hot.
- [ ] **Step 4 — commit.** Prefix `feat(porch-store)`.

---

### Task 2 — the key pool: the actor and its protocol

**Files:** create `docs/examples/porch/middleware/keypool.wo`.

**Consumes:** the tables from Task 1.

**Produces:** the message classes and the pool accessor every later task uses.
Name them once here and do not rename them later:

- **ONE message class** carrying a `kind: Int` discriminator and the union of
  what both operations need: the key, the limit and the window size for a
  count; the digest, the request and the route's `Handler` for a begin. An
  actor handle is typed to a single message class, so a second `receive`
  compiles but is unreachable through that handle. `docs/examples/chat/main.wo`
  is the repo's precedent — its registry takes `kind: 1` for lookup and
  `kind: 2` for shutdown. Use `kind: 1` for count and `kind: 2` for begin;
- a pool type holding a list of actor addresses, and a selector that maps a key
  to one of them by hash;
- a verdict class the limiter reads: whether the request is allowed, the count,
  the limit, and the wall-clock reset instant.

- [ ] **Step 1 — write the failing test as a gate leg stub.** Add a leg to
      `scripts/web-app-accept.sh` that compiles a tiny program using the pool
      and asserts two sequential counts return 1 then 2. It must fail now,
      because `keypool.wo` does not exist. A test that cannot fail before the
      code exists is not a test.
- [ ] **Step 2 — run it and watch it fail.** `just web-app`. Expect the new leg
      to report a compile failure naming the missing class.
- [ ] **Step 3 — define the message classes and the verdict class.** Fields
      only; no behaviour yet.
- [ ] **Step 4 — define the actor class with ONE `receive`,** switching on
      `kind`. Implement the count arm (`kind: 1`) now and leave the begin arm
      (`kind: 2`) for Task 4. Counting reads the row for the key, decides, and
      returns a verdict.
      Counting reads the row for the key, decides, and writes the new count by
      **assigning to the row's field** so it writes through.
- [ ] **Step 5 — window arithmetic.** If the elapsed monotonic time since the
      stored window start exceeds the configured window, reset the count to
      zero and restart the window. Use `time.ticks()`. Compute the reset
      instant for the header from `time.now()` — the two clocks are not
      interchangeable and mixing them is the defect being fixed.
- [ ] **Step 6 — prune, do not merely reset.** When a window has fully elapsed
      and the client returns, delete the stale row rather than resetting its
      count in place. Resetting keeps a row forever for every key ever seen,
      which for IP-keyed limiting is an unbounded leak — one row per address
      that ever touched the service. Deleting on access is the lazy expiry the
      story specifies, and there is no sweeper to do it later.
- [ ] **Step 7 — the pool and its selector.** A construction function that
      spawns N actors and returns the pool; a selector that hashes a key to an
      index. Same key must always select the same actor — that is the entire
      serialization mechanism.
- [ ] **Step 8 — run the leg.** Expect 1 then 2.
- [ ] **Step 9 — commit.** Prefix `feat(porch-store)`.

---

### Task 3 — the limiter delegates

**Files:** modify `docs/examples/porch/middleware/limiter.wo`.

**Consumes:** the count message, the pool selector and the verdict from Task 2.

**Produces:** a `Limiter` middleware with fields for the limit, the window, and
a `trust_proxy` flag defaulting to false.

- [ ] **Step 1 — delete the counting logic.** All of it: the query, the
      increment, the delete-then-insert, and the `catch (e) nil`. The limiter
      must not touch the table at all. If any store call remains in this file
      the serialization guarantee is void.
- [ ] **Step 2 — key selection.** With `trust_proxy` false, key on
      `net.peer(req.conn)`, which cannot be forged. With it true, key on
      `client_ip(req)` — the left-most `X-Forwarded-For` entry — and the app
      author is asserting a proxy they control overwrites that header. Prefer
      `req.principal` when it is non-empty. **Delete the
      `req.ctx["verified_proxy"]` branch**: nothing anywhere sets that key, so
      it is dead code that reads as a security control.
- [ ] **Step 3 — call the pool.** Send the count message and act on the
      verdict. Set `X-RateLimit-Limit`, `X-RateLimit-Remaining` and
      `X-RateLimit-Reset` on both the allowed and the refused path.
- [ ] **Step 4 — the refusal.** On a spent window return 429 with
      `Retry-After` in seconds.
- [ ] **Step 5 — saturation.** Catch the actor trap from `call` and return 503
      with `Retry-After`. Do not allow the request through. A limiter that
      stops limiting under load is worse than absent, because saturating the
      pool is then the bypass.
- [ ] **Step 6 — gate leg: the threshold.** Add a leg asserting that of N+1
      requests the first N pass and the last is 429 carrying `Retry-After`.
- [ ] **Step 7 — gate leg: the restart.** Drive the limiter to its limit,
      `SIGTERM` the process, restart it against the same `WO_DATA`, and assert
      the client is **still** limited. This is the leg that proves the
      differentiator over an in-memory limiter, and it is the one most likely
      to be skipped. A durability claim no gate exercises is not a claim.
- [ ] **Step 8 — gate leg: exact counting under concurrency.** Fire N requests
      for one key **genuinely in parallel** and assert the recorded count is
      exactly N. This is the criterion the whole actor pool exists for: the old
      read-modify-write lost increments when two fibers interleaved, so a
      limiter under load stopped limiting at precisely the moment it mattered.
      A sequential version of this leg passes against the broken code and
      proves nothing.
- [ ] **Step 9 — run all three legs.** Expect green.
- [ ] **Step 10 — commit.** Prefix `feat(porch-store)`.

---

### Task 4 — idempotency, with the actor running the handler

**Files:** modify `docs/examples/porch/middleware/idempotent.wo` and
`docs/examples/porch/middleware/keypool.wo` (step 4 adds the actor's begin
arm, which lives in the pool file).

**Consumes:** the begin message and the pool from Task 2; the digest column
from Task 1.

**Produces:** an `Idempotent` middleware taking the key header name, an
`include_body` flag and a TTL.

- [ ] **Step 1 — read the spec's reason before writing code.** The middleware
      does **not** run the handler and store afterwards. It hands the request
      and the route's `Handler` to the actor, and the actor invokes the handler
      inside its own `receive`. A duplicate then waits in the **mailbox** and is
      served after the owner returns. This is forced: `call`'s reply is the
      return value of `receive`, so a reply cannot be held for later.
- [ ] **Step 2 — delete the old flow.** Remove the `before`/`after` pair, the
      `idem_miss` and `idem_key` context keys, and the ten-second in-flight
      heuristic. That heuristic was inverted — its timestamp is stamped when
      the response is stored, not when the request starts, so it fired on
      legitimate fast replays and never on a real collision.
- [ ] **Step 3 — the digest.** Compute a digest of method, path and body when
      `include_body` is set, and pass it alongside the bare key. Keep `use
      json` — the file did not typecheck without it.
- [ ] **Step 4 — the actor arm for the begin message.** `call`'s reply must be
      a copyable scalar (WO-E226 — a response object cannot cross the mailbox),
      so the arm returns an **outcome code** and the response travels through
      the table. Look the bare key up. A hit whose digest matches returns
      "replayed" without running the handler. A hit whose digest differs
      returns "mismatch". A miss runs the handler, stores status, body and
      `content-type` with the digest and the current tick, and returns
      "executed". The middleware then reads the stored row and builds the
      response from it — so owner and duplicate return the same durable row,
      and byte-identical replay is structural rather than careful.
      **Keep the return type identical to the one Task 2's count arm uses**;
      WO-E226 also requires every `receive` program-wide to agree.
- [ ] **Step 5 — the replay allowlist.** Store and replay `content-type` only.
      Never `Set-Cookie`, never `Date`. Cookies arrive in porch 2; this is
      cheap now and expensive to retrofit after.
- [ ] **Step 6 — lazy expiry.** On access, if the stored row is older than the
      TTL, delete it and treat the request as a miss. There is no sweeper and
      no scheduler; `time.after` is a one-shot timer aimed at an actor, not a
      recurring sweep.
- [ ] **Step 7 — saturation.** Same rule as the limiter: a trapped `call`
      answers 503, not a silent second execution.
- [ ] **Step 8 — gate leg: replay is exact.** Assert the replayed response is
      byte-identical **and** that the handler's side effect happened once —
      counted from a row count in the store, never from a log line.
- [ ] **Step 9 — gate leg: digest mismatch.** Reuse a key with a different body
      and assert 422, not the other request's response.
- [ ] **Step 10 — gate leg: concurrent duplicates.** Dispatch two identical
      keyed requests **genuinely in parallel** — backgrounded clients, not two
      sequential calls — against a handler slow enough to overlap. Assert
      exactly one execution and that both clients receive the same response
      body. Sequential requests cannot fail this leg, so a sequential version
      of it proves nothing.
- [ ] **Step 11 — run the legs.** Expect green.
- [ ] **Step 12 — commit.** Prefix `feat(porch-store)`.

---

### Task 5 — pool saturation, and closing out

**Files:** modify `scripts/web-app-accept.sh`,
`docs/examples/porch/README.md`, `docs/stories/porch/01-store-backed-middleware.md`,
`docs/stories/00-status.md`.

- [ ] **Step 1 — gate leg: saturation fails closed.** Configure a pool of one
      actor and a handler slow enough to fill its mailbox, then assert the
      overflow answers 503 and that no request slips through uncounted. This
      is the leg that proves saturation is not a bypass.
- [ ] **Step 2 — document the pool size knob.** Record in the README what the
      pool size bounds and what happens when it is too small: 503s, not silent
      overshoot. It is a capacity decision, not a default to ignore.
- [ ] **Step 3 — the ledger.** Move the two rows in the README from 🔶 to ✅,
      and only now — a ✅ whose gate leg does not exist is the thing this
      project keeps catching.
- [ ] **Step 4 — the story.** Fill the Progress table with real commit hashes,
      move the criteria from Outstanding to Met **recording how each was
      verified**, and set `status: done` in the frontmatter.
- [ ] **Step 5 — the board.** Add the standup entry: what landed, what did not,
      dependencies unblocked, next steps.
- [ ] **Step 6 — run everything.** `just web-app`, `just site`,
      `just linkcheck`. All green.
- [ ] **Step 7 — commit.** Prefix `docs(porch-store)`.

---

## What this plan deliberately does not do

- **No pluggable storage interface.** One store exists; an abstraction with one
  implementor is decoration.
- **No sliding window or token bucket.** Fixed window is what the sample needs;
  a better algorithm is a later slice with a measurement behind it.
- **No deferred-reply runtime primitive.** It would make the original design
  implementable and is useful beyond this feature, but porch 1 was chosen as
  the slice needing no runtime work. It belongs in its own iteration.
- **No blocking deadline.** A duplicate waits as long as its owner's handler
  runs. If a deadline proves necessary it should arrive with the measurement
  that justifies its value, not ahead of it.
