---
track: porch
iteration: "9"
status: hold
readiness: ready
---

# porch 9 — idempotent replay of unsafe requests

> Part of [Story — `porch`, the writeonce web framework](00-story.md).
> Spec: [`2026-08-29-porch-store-backed-middleware-design.md`](../../superpowers/specs/2026-08-29-porch-store-backed-middleware-design.md).
> Code: the tag **`archive/porch-idempotency`** — complete, reviewed, and the
> reproduction harness for the defect blocking it.
>
> **Split out of [porch 1](01-store-backed-middleware.md) on 2026-08-30.**
> `readiness: ready` because nothing about the design is unsettled — it was
> implemented and passed review. `status: hold` because it is blocked on a
> C-runtime crash it did not cause but does provoke.

## Why this is on hold, precisely

Not a design failure, and worth being exact about that so nobody re-litigates a
settled design when they pick this up.

The middleware works. A keyed request calls its pool actor, which runs the
route's `Handler` inside its own `receive`; a duplicate for the same key waits
in the **mailbox** and is dequeued once the owner returns, by which time the
response row exists. That is real blocking with no held reply, no polling and no
409 — and it needed no new runtime primitive.

What blocks it is a **C-runtime defect**: a SIGSEGV localised by `gdb` to
`wo_arena_alloc` / `wo_str_new`, under concurrent `call()`-parked callers doing
heavy allocation inside `receive`. It also manifests as the process hanging
after `main()` returns, roughly one run in five.

The evidence that this path provokes it, rather than merely coinciding with it:

- Across ten consecutive gate runs, **every** failure was an idempotency leg.
  The rate limiter's 30-parallel leg — same pool, same `call`, same park
  machinery — never failed once.
- The begin arm has **five times** the allocation sites inside `receive` that
  the count arm has, and it moves a whole `Req` plus a `Handler` through the
  mailbox where the count arm moves four scalars.
- The crash grows more likely with sequential insert+delete volume against one
  key: N=4 and N=5 crashed 1/3 and 3/3 times, N=1–3 stayed clean over 12+ trials.

## What has to happen first

[The runtime crash](../language-runtime-database/41-actor-arena-crash.md) must
be root-caused and fixed. Recover this work with
`git checkout -b <name> archive/porch-idempotency`, re-run
`scripts/web-app-accept.sh` sections 18a–18h and 19, and expect them stable
before resuming.

## What is already settled — do not re-brainstorm

1. **The actor runs the handler.** `call`'s reply is the return value of
   `receive`, so a reply cannot be held for later; an actor that tried would
   deadlock against the completion it waits for. The mailbox IS the queue.
2. **The response travels through the `@table`, not the mailbox.** WO-E226:
   replies must be copyable scalars and every `receive` program-wide must share
   one return type. The actor stores the response and returns an outcome code.
   Owner and duplicate then read the same durable row, which makes
   byte-identical replay structural rather than careful.
3. **The digest is a column, not part of the key.** Fold it into the key and
   "same key, different body" becomes undetectable, because nothing ever looks
   the bare key up.
4. **`Idempotent` is a `Handler` decorator, not a `Middleware`** — the actor
   needs the route's handler and only the handler slot exposes it.
5. **4xx/5xx are never durable replay targets.** A cached transient 500 would be
   replayed for the whole TTL, so a retry could never succeed — the exact
   inverse of why idempotent retry exists.
6. **Ephemeral rows are per-attempt and nonce-keyed**, and the middleware
   deletes its own after one read. Sharing one row raced; leaving them lingering
   leaked and, because the nonce wraps every 1000s, eventually replayed a stale
   failure.
7. **Saturation fails closed with 503.** Saturating the pool must not become the
   bypass.

## Two runtime defects it also has to work around

Both are worked around in the archived code and neither is porch's:

- `try EXPR catch (e) nil` cannot distinguish a literal `Int 0` reply from a
  trap — worked around by never packing a zero outcome code.
- A `Text`/map value read off `json.decode(...) as T` is corrupted once embedded
  in a struct crossing a function-return boundary — worked around by forcing
  fresh text with `.. ""` on every field copied out of a decoded record.

## Acceptance criteria

Carried from porch 1, all of them **met by the archived code** and all of them
to be re-proven once the runtime is fixed: byte-identical replay with the
handler's side effect counted from a row count; a reused key with a different
body refused with 422; two concurrent duplicates yielding exactly one execution;
a transient 5xx never replayed, solo or concurrent; ephemeral rows returning to
baseline rather than accumulating; and a saturated pool answering 503 rather
than executing twice.
