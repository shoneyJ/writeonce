# porch 1 — store-backed middleware: rate limiting and idempotency

Design settled 2026-08-29. Implements
[porch 1](../../stories/porch/01-store-backed-middleware.md).

## Why this is a redesign, not a first draft

Phases A, B and C already have code on `dev` — `519d411`, `5b1e82a`, `aee7926`.
Phase A survives. **Phases B and C are superseded**, because both are built
around a middleware that stores its result *after* the handler returns, and
three of the story's seven acceptance criteria cannot be satisfied by that
shape:

- **In-flight collision is undetectable.** The `before` hook finds nothing and
  passes; the row is written by `after`, once the handler has already run. Two
  concurrent duplicates both miss and both execute. There is no reservation for
  them to collide on.
- **The in-flight heuristic is inverted.** It treats a row younger than ten
  seconds as "in flight", but the timestamp is stamped when the response is
  *stored*, not when the request *starts*. So it fires on a legitimate fast
  replay — the common case — answering 409 where the stored response was owed,
  and it can never fire on a genuinely concurrent request.
- **"Reused key, different body is refused" is unreachable by construction.**
  The body digest is folded into the lookup key, so a different body is a
  different key and simply misses. That is safe — the wrong response is never
  served — but nothing ever looks the bare key up, so nothing can refuse.

Two further defects, independent of the redesign: the limiter emits a monotonic
tick value into `X-RateLimit-Reset`, where clients expect a Unix timestamp; and
its key selection branches on a request-context flag that nothing anywhere
sets, so the branch is dead.

## The shape

One idea, inherited by porch 2 (sessions) and 3 (CSRF):

> **Serialize through an actor. Persist in a `@table`. Never read-modify-write
> from a handler fiber.**

A read-modify-write from a handler fiber is the defect both features share. The
limiter reads a count, adds one, and writes it back; two fibers interleaved lose
an increment, and a limiter that under-counts fails at precisely the moment it
exists for. Idempotency has the same race with worse consequences. An actor
processes one message at a time, so routing both features through one gives
per-key serialization with no locks and no polling.

Actors own **serialization and volatile state**. Tables own **durability**.
No middleware touches those tables directly.

### The pool

A fixed pool of `N` identical actors, selected by a hash of the key modulo `N`.
The same key therefore always reaches the same actor, which is the whole
mechanism — ordering is only needed *per key*, never globally, and a pool buys
concurrency across keys while a single actor would serialize the entire
program's traffic through one mailbox.

`N` is a declared capacity knob, documented and set at construction. It is the
one number in this design that a deployment may need to change.

### Why the primitives are already there

The story states that nothing here needs a new runtime primitive, and then that
`time.after` is "still a reserved builtin id". The second claim is stale and the
first is still true. All of it landed with the actor-lifecycle work: `spawn`
(68), `send` (69), `call` (88 — a send that waits, whose park/reply protocol
lives in the VM), `monitor` (89), and `time.after` (90). `call` parking the
caller until a reply arrives is exactly the blocking primitive this design
needs, and it already exists.

## Phase A — the store convention

Two purpose-shaped tables, as already built and as the story's first fork
leaned. A counter row is an integer; a stored response is a status, a header
set and a body. A single generic table would force both through text, which is
how framework caches end up storing JSON strings for want of a column.

The stored-response table gains **one new column: the request digest**, stored
beside the response rather than folded into the key. That single change is what
makes refusal possible.

Expiry stays lazy — pruned when a key is next touched, never by a sweeper. The
story is right that porch has no scheduler, and `time.after` is a one-shot timer
aimed at an actor, not a recurring sweep.

## Phase B — the rate limiter

Every request calls its pool actor and waits for a verdict. The actor reads the
counter, decides, writes the new count, and replies with the decision plus the
numbers the response headers need.

Three corrections to what exists:

- **Write through, do not delete and re-insert.** Assigning to a field of a
  table row compiles to an engine update that maintains the row's indexes at the
  choke point. Delete-then-insert writes two WAL records where one will do, and
  leaves a window in which the delete has landed and the insert has not — which,
  with the failure swallowed as it currently is, silently loses the counter
  entirely and hands the client a free window. That is the exact inverse of the
  durability this iteration exists to demonstrate.
- **Key on the peer address by default.** The limiter takes a trust-proxy flag
  defaulting to off. Off, it keys on the connection's real peer, which cannot be
  forged. On, it keys on the left-most forwarded-for entry, and the application
  author is asserting that a proxy they control overwrites that header. Only the
  deployer knows the topology, so the declaration belongs in their code — not in
  a context flag nothing sets. When story 35 lands a peer-address seam that can
  *verify* the proxy, it can downgrade a trust-proxy claim that is untrue.
- **Two clocks, deliberately.** Window arithmetic uses the monotonic tick source
  so that a wall-clock jump cannot grant an extra window. The reset header must
  carry wall-clock time, because it is for a client that has no access to this
  process's boot time.

Persisting every increment is one WAL record per request. Group commit batches a
drain into a single barrier, so the cost is amortised rather than a sync per
request — and paying it is what makes the restart criterion true.

## Phase C — idempotency

A keyed request calls its pool actor and receives one of three outcomes.

**The actor runs the handler.** This is the correction of 2026-08-29 — see
History. A keyed request does not run its own handler; it calls its pool actor,
passing the request and the route's `Handler`, and the actor invokes the
handler inside its own `receive`.

| Actor state for that key | What `receive` returns |
| --- | --- |
| A stored response exists, digest matches | the stored response; the handler never runs |
| A stored response exists, digest differs | a refusal verdict, answered as 422 |
| No record | run the handler here, store the response, return it |

There is no fourth row, and that is the point. **A duplicate arriving while the
owner's handler runs waits in the mailbox**, because an actor processes one
message at a time. It is dequeued after the owner's `receive` returns, finds
the stored response, and is answered with it. The queue that blocking needs
already exists and is the mailbox; nothing has to hold a reply.

Why it must be this way: `call`'s reply **is** the return value of `receive`.
There is no handle to stash and answer later. An actor that tried to hold a
waiter would have to not return from `receive`, and while it has not returned
it processes nothing else — including the owner's completion message. That
deadlocks. Verified before committing to it: an actor can receive a message
carrying an interface-typed value and invoke it, so passing the route's
`Handler` through the mailbox works.

The cost is real and must be stated: the actor is occupied for the whole
duration of the handler it runs, so a slow keyed handler blocks other keys that
hash to the same actor. Pool size is what bounds that, and it is the same knob
that bounds saturation.

The lookup key is the **bare** idempotency key. The digest of method, path and
body is a column, compared on a hit. Equal means a genuine retry and earns the
stored response; different means the key was reused for a different request, and
that is refused rather than answered with another request's response.

Replay carries `content-type` only. The story's third fork is settled as built,
and settled correctly: replaying a stored `Set-Cookie` or a stale `Date` is
wrong, and porch has no cookies yet, so this is cheap now and expensive after
iteration 2.

## Failure semantics

**Saturation fails closed.** Mailboxes are bounded, so a burst can trap. Both
features answer 503 with a retry hint rather than proceeding. A rate limiter
that stops limiting under load is worse than absent, because saturating the pool
*becomes* the bypass — and a burst is the thing it exists to stop. The cost is
honest and must be documented: a genuine traffic spike degrades to 503 once the
pool is full, which makes the pool size a real capacity decision rather than a
default nobody reads.

**A crash loses only volatile state.** Counters and stored responses are in the
WAL and replay. Ownership and waiter lists are in-memory and do not. A crash
mid-handler therefore leaves no owner, and the next duplicate re-runs the
handler — the behaviour the world had before the feature existed, rather than a
key wedged permanently pending. This is the decisive advantage over persisting
reservations: a durable pending row outlives the process that owned it and must
then be rescued by a lease, whose expiry is another number to get wrong.

## Phase D — the gate

The story is right that a durability claim no gate exercises is not a claim, and
the restart leg is the one most likely to be quietly skipped. The acceptance
script gains: a burst that crosses the threshold exactly; a restart between two
bursts proving counters replayed rather than reset; a replayed key proving the
handler's side effect count is unchanged, counted from a row count rather than a
log line; a reused key with a different body earning a refusal; and two
simultaneous duplicates proving exactly one execution.

The concurrency legs need genuine parallelism, not two sequential requests. A
test that cannot fail before the fix is not a test.

## Acceptance criteria

Carried from the story, with the three unreachable ones restated to match the
design that replaces them.

- **Given** a limiter of N per window, **when** a client sends N+1, **then** the
  first N succeed and the last is refused with a retry hint.
- **Given** counters at their limit, **when** the process is terminated and
  restarted, **then** the client is still limited — replayed from the WAL, not
  reset.
- **Given** a fully elapsed window, **when** the client returns, **then** it is
  served and the expired row is pruned on that access.
- **Given** the system clock jumping backwards, **when** a window is evaluated,
  **then** no extra allowance is granted.
- **Given** N concurrent requests for one key, **when** they are counted,
  **then** the total is exactly N — no increment is lost. *(Newly reachable;
  the pool is what makes it true.)*
- **Given** a seen idempotency key, **when** it is replayed, **then** the stored
  response is returned byte-identically and the side-effect count is unchanged,
  proven by a row count.
- **Given** a reused key with a different body, **when** it arrives, **then** it
  is refused. *(Newly reachable; the digest column is what makes it true.)*
- **Given** two identical keyed requests in flight at once, **when** both are
  dispatched, **then** exactly one executes and the other receives that one's
  stored response — never a refusal, never a partial write. *(Restated: the
  story allowed 409 here; blocking supersedes it.)*
- **Given** a saturated pool, **when** a request arrives, **then** it is refused
  with 503 rather than served uncounted.

## Out of scope

Unchanged from the story: no pluggable storage interface, no sliding-window or
token-bucket algorithm, no cross-process limiting, no background sweeper, and no
TTL cache — language iteration 18 owns that and its spec is already approved.

Added: **verifying** that a peer really is the trusted proxy. This design lets an
author declare it; story 35 owns proving it.

## History — one correction, made before any code

**`call` cannot defer a reply, and the first version of this spec assumed it
could.** The design said the actor would hold a duplicate's reply and answer it
once the owner reported. That is not expressible: `call`'s reply is the return
value of `receive` (`tests/corpus/run/call-echo/fixture.wo`; `msg_caller` in
`runtime/src/vm.c` is answered on handler completion), so holding a waiter
means never returning, and an actor that never returns processes nothing else —
including the completion it is waiting for.

The error behind it is worth keeping: the brainstorm established that `spawn`,
`send`, `call`, `monitor` and `time.after` had all landed, and concluded from
that that blocking needed no new runtime surface. **The primitives existing is
not the same as one of them supporting deferred reply.** The conclusion was
right by luck — blocking is achievable — but the reasoning did not support it,
and the shape it produced was unimplementable.

Inverting so the actor runs the handler gets the same semantics from the
mailbox itself, and was verified with a throwaway fixture before adoption
rather than after.

## Risks

- **The pool is now on every request path.** Exact counting was chosen over a
  free hot path deliberately, but it makes pool sizing a first-class operational
  concern, and the fail-closed rule converts undersizing into 503s rather than
  into silent overshoot. That trade is the point, and it needs to be measured
  before it is defended.
- **A duplicate waits as long as its owner's handler runs**, and so does every
  other key that hashes to the same actor, because the actor is occupied while
  it runs a handler. This is the sharpest cost of the inversion. No deadline is
  specified here; if one proves necessary it belongs with the measurement, not
  ahead of it.
