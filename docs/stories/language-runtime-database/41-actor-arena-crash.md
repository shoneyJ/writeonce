---
track: language-runtime-database
iteration: "41"
status: in-progress
readiness: refine
---

# 41 — the actor arena crash: a SIGSEGV under concurrent parked callers

> Found 2026-08-30 while implementing [porch 1](../porch/01-store-backed-middleware.md).
> It blocks [porch 9](../porch/09-idempotent-replay.md) outright and it is not a
> porch bug — it is in the C runtime, and it threatens **any** actor code that
> allocates heavily inside `receive`.

## Status 2026-08-30 — the SIGSEGV is FIXED. The hang is not.

**They were two defects, not one.** This file used to say "two shapes, almost
certainly one cause". That guess was wrong, and it was disproven by fixing one
and watching the other survive.

### Fixed: the SIGSEGV — an unadopted shard impersonating shard 0

Root cause, reproduced minimally and pinned:

A worker shard's runtime is initialised **lazily**, when it adopts its first
fiber, and `rt.shard_id` is stamped only there (`vm.c`, worker late-init). But
`INBOX_READY[i]` is set at **thread creation**, long before. So a shard that
never adopts a fiber still gets settled at shutdown by `eng_settle_inboxes`,
which — unlike its sibling loop over `vm_drop_actor_world` — was guarded only on
`INBOX_READY`, not on the runtime actually being initialised.

Such a shard carried `rt.shard_id == 0` from the `memset`, so it
**impersonated shard 0**. `wo_drop_obj` compares `o->shard_id` against
`rt->shard_id`, saw `0 == 0` for anything the primary had allocated, took the
"we are home" branch instead of routing, and called `class_free` against
`rt->classes` — which lazy init had never filled. `&rt->classes[class_id]` off a
NULL base is the faulting read; ASan reported `0x148`, the offset of the class
id it was carrying.

The fix stamps the runtime's real identity at thread creation. An
uninitialised shard has allocated nothing and therefore owns nothing, so its
true id makes every payload correctly foreign and routes it to the owner that
can free it.

**Why ASan never caught this in the existing suite:** the arena is one
hand-managed `malloc` block, so intra-arena reuse is invisible to the
sanitizer. The failure surfaces as a bare SEGV, never as a use-after-free
report — which is also why the original investigation could only localise it
rather than name it.

Pinned by `tests/regress/lang-41/shard-settle-crash.wo`, driven from
`scripts/db-actor-accept.sh`. It needs **multiple shards** (the corpus runner
pins `WO_SHARDS=1`, which is why it does not live there) and the ASan build. It
SEGVs twice per run against the unfixed runtime and is clean with the fix.

### Still open: the hang

With the SIGSEGV fixed, the reproduction harness stopped losing whole sections
to crashes — the runs that used to fail six checks with `000` status codes are
gone. What remains is a single check, `idempotent-stop-2`: after SIGTERM the
process is still alive. Measured at roughly **1 run in 6** with the fix in
place, against ~1 in 4 before.

So the hang is its own defect and needs its own investigation. It was not
caught in this pass: a `gdb` attach needs the hung process held open, and eight
scripted attempts to catch one in the act did not land inside the time budget.

**Where to look first.** `wo_engine_stop` sets `eng_shutdown`, wakes each
worker's eventfd, then `pthread_join`s. Two candidates worth eliminating before
anything else: a worker blocked in `wo_io_wait` with a fiber parked on a `call`
whose reply will never arrive, and the primary spinning in
`while (eng_settle_inboxes() > 0) {}` if two shards can route the same payload
to each other indefinitely. The second is cheap to rule out with a counter.

## The original symptom

Two shapes, believed at the time to share one cause:

- **A hang.** Under concurrent `call()`-parked callers doing real per-request
  table I/O, `main()` returns cleanly and then the OS process fails to exit.
  Roughly one run in five, non-deterministic.
- **A SIGSEGV.** The aggressive variant — same workload with forced
  `Connection: close` — crashes outright.

A `gdb` backtrace puts it inside **`wo_arena_alloc` / `wo_str_new` / `vm_run`**.

## What is already known, and how it was measured

This is the part worth keeping: the evidence was gathered by accident and would
be expensive to reproduce from scratch.

- **It tracks allocation inside `receive`, not `call` itself.** Two middlewares
  drive the same actor pool through the same `call`/park machinery. The one
  whose actor arm has ~5 allocation sites and moves a whole `Req` plus a
  `Handler` through the mailbox crashes; the one with ~2 sites moving four
  scalars does not. Over ten consecutive gate runs, **every** failure belonged
  to the allocation-heavy path and **none** to the light one.
- **It scales with sequential insert+delete volume against one key.** N=4 and
  N=5 crashed 1 time in 3 and 3 times in 3; N=1–3 stayed clean across 12+
  trials. That points at slot recycling or arena reuse rather than at anything
  request-shaped.
- **A freshly restarted server makes it much rarer**, which is consistent with
  state accumulated in the arena rather than a single bad allocation.

## The reproduction harness

`archive/porch-idempotency` is a working reproduction, not a description.
Sections 18a–18h and 19 of `scripts/web-app-accept.sh` on that tag drive it.
Recover with `git checkout -b <name> archive/porch-idempotency`.

The two most reliable triggers there are the concurrent-duplicate leg (two
parallel clients through one actor against a slow handler) and the ephemeral-row
leg (N sequential insert+delete cycles on one key).

## Two smaller runtime defects found alongside it

Independent of the crash, both worked around rather than fixed, both worth
fixing while someone is in this code:

1. **`try EXPR catch (e) nil` cannot distinguish a literal `Int 0` reply from a
   trap.** Any code whose valid reply includes 0 silently treats success as
   failure. Worked around in the archived code by never packing a zero outcome.
2. **A `Text`/map value read off `json.decode(...) as T` is corrupted once
   embedded in a struct that crosses a function-return boundary.** Worked around
   by forcing fresh text with `.. ""` on every field copied out of a decoded
   record. This one is a data-corruption class defect and deserves its own
   minimal fixture.

## Why this outranks the porch work behind it

A crash in `wo_arena_alloc` under concurrent actors is not a niche failure. The
actor model is the concurrency story for this runtime, and allocation inside
`receive` is the normal thing for an actor to do — porch merely happened to do
enough of it to find this. Anything built on actors is exposed until it is
fixed.

## Out of scope

Fixing the porch feature that found it. That is [porch 9](../porch/09-idempotent-replay.md),
and it is already written; it only needs this to land first.
