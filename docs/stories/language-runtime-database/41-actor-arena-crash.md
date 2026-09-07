---
track: language-runtime-database
iteration: "41"
status: in-progress
readiness: ready
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

### Status 2026-09-05 — the hang is ROOT-CAUSED. It is a double free.

Neither of the two candidates below was right, and the third guess in this
file — "two shards route the same payload to each other" — is half right: it is
**one** shard routing to *itself*.

`gdb` is still unusable here (`ptrace_scope=1` blocks a sibling tracer), so the
evidence came from `/proc` plus counters compiled into the runtime. Two CPU
samples a second apart during a live hang: `Threads: 1`, state `R`, `wchan 0`,
utime 157 → 212 and stime 148 → 193. One thread spinning at 100%, every worker
already joined — so the hang is past `pthread_join`, and `main()` had returned.

**The livelock, measured.** A counter around `while (eng_settle_inboxes() > 0)`
shows it returning **12, forever**: `moved_total` is exactly 12 × passes at one
million passes. Dumping those envelopes names the mechanism:

```
L41 env: inbox=19 vm_rt_shard=19 kind=2 obj_shard=32019 class_id=-3888 nshards=20
```

`obj_shard=32019` is not a shard. It is the **high 16 bits of a pointer**
(`0x7d13`), and `class_id` is that pointer's low 32 bits. `wo_arena_free` frees
a block by writing the freelist next-pointer over its first 8 bytes — which is
exactly `class_id` (0..3), `shard_id` (4..5), `flags`, `pad`. So the payload
being settled is **an already-freed block**, and the header being read is a
freelist link.

That garbage id is what makes it spin rather than crash or leak.
`wo_route_free` pushes to `INBOX[shard_id % WO_ENG_MAX_SHARDS]`, but
`wo_drop_obj` compares the **unmasked** `shard_id` against `rt->shard_id`.
`32019 % 64 == 19`, so the envelope lands back in the very inbox it came from,
is judged foreign again, and routes again. Any id ≥ 64 congruent to a live
shard mod 64 livelocks the settle loop.

**Where the freed block enters.** A `backtrace()` on any route whose
`shard_id >= nshards` puts the origin in the RUN, not in teardown:

```
main → wo_vm_call → vm_run (vm.c:2163) → wo_vm_adopt (vm.c:174, case 2,
    a home-routed free) → wo_drop_obj → class_free (gc.c:66)
    → wo_drop_kind → wo_drop_obj → wo_route_free
```

The parent object is intact; its **fields** are not:

```
L41 BADFIELD: parent class_id=0 shard=0 | field #0 kind=4 target_shard=29774
L41 BADFIELD: parent class_id=0 shard=0 | field #2 kind=4 target_shard=29774
L41 BADFIELD: parent class_id=0 shard=0 | field #3 kind=4 target_shard=29774
```

Kind 4 is `WO_K_MULTI`. A shard-0 object owns three `multi` containers that
live in **another shard's arena** and have already been freed. Freeing the
parent drops them a second time. This is why the allocation-heavy arm was the
only one that ever failed and the four-scalar arm never did: **containers
crossing the mailbox are the trigger**, not `call` and not allocation volume as
such. Slot recycling under insert+delete churn only decides how fast the freed
block gets reused, which is why N=4/N=5 looked causal.

### Which side drops first — settled 2026-09-06

Measured, not inferred. The instrument is a per-arena history table: every
`wo_arena_alloc` (both the bump and freelist paths) and every `wo_arena_free`
records the class the block carried plus two return addresses, keyed by block
address. A dangling pointer's history then names the free that orphaned it. The
earlier suspicion — the parked-`call` re-execution dropping its moved argument
twice — was **wrong**.

The block that starts the livelock has this history, all of it in **shard 1's**
arena:

```
  -- PARENT 0x73cdc7fff1e0 --
    #0 ALLOC bump      via wo_obj_new+0x53
    #1 FREE  as user(14)  via class_free <- wo_drop_obj+0xcb
```

**First drop: the object's own home shard, legitimately.** Shard 1 allocated it
as class 14 and freed it through the ordinary owned-graph path. Nothing is wrong
up to here.

**Second drop: a worker executing the compiled `DROP` opcode**, walking a
*different* owner graph that still reaches the same block:

```
wo_vm_serve → vm_run (vm.c:2153, CASE(DROP)) → wo_drop_obj
  → class_free (gc.c:67) → wo_drop_kind → wo_drop_obj
    → multi_free (gc.c:42) → wo_drop_kind → wo_drop_obj
      → class_free → wo_drop_kind → wo_drop_obj → wo_route_free   ← stale
```

So the defect is **an owned subtree reachable from two owner graphs**: one shard
1 has already destroyed, one still live on a worker. It is shared where it
should have been transferred. The `multi_free` frame in the middle is why only
the container-carrying arm ever failed.

**Why the second drop lands on shard 0 rather than trapping.** `wo_arena_free`
writes the freelist next-pointer over the block's first 8 bytes. When the block
is the **tail** of its size class that pointer is NULL, so the header reads back
`class_id 0, shard_id 0, flags 0` — and class 0 is a *valid* class index. The
worker sees `shard_id 0 != its own`, routes the free to shard 0; shard 0 adopts
it (`wo_vm_adopt` case 2), matches `0 == 0`, concludes "we are home", and runs
`class_free` with **class 0's** field kinds over a dead class-14 object. Class
0's kinds say fields #0/#2/#3 are `WO_K_MULTI`; the slots actually hold the dead
object's Text pointers, already freed by shard 1. Those carry freelist links as
headers, so their `shard_id` is a pointer's high 16 bits — 29645, 32019 — and
the modulo alias sends them back to the inbox they came from, forever.

Three distinct defects, in fix order:

1. **The aliased subtree** — the root cause. A child owned by two graphs.
   Not yet localised to the code path that creates the alias; that is the next
   question, and the `DROP` site plus the `multi_free` frame are where to look.
2. **A freed block is indistinguishable from a live class-0 object.** A NULL
   freelist link forges a valid header. A poison class id (or a free bit in
   `flags`) would turn every one of these into an immediate, named trap instead
   of a silent misinterpretation — cheap, and it would have caught this on the
   first run.
3. **The modulo alias.** `wo_route_free` pushes to `INBOX[shard_id % 64]` while
   `wo_drop_obj` compares the unmasked id. Mask consistently, or refuse to route
   an id ≥ `nshards`.

**Two defects, and the order matters.** Bounding the settle loop would stop the
hang and leave a double free behind, turning a visible spin into a silent
corruption. Fix the ownership bug first; the modulo alias is a real second
defect worth its own fix (mask consistently, or refuse to route an id ≥
`nshards`), but it is not the root cause.

Reproduction, all of it scripted: worktree at `archive/porch-idempotency`,
cherry-pick `9dca0b4` onto it, then loop `scripts/web-app-accept.sh`. The
`/proc` dump, the settle counter, the envelope trace and the bad-field trace are
each a few lines against `vm.c` and `gc.c`.

**Where to look first (superseded — kept for the record).**
`wo_engine_stop` sets `eng_shutdown`, wakes each
worker's eventfd, then `pthread_join`s. Two candidates worth eliminating before
anything else: a worker blocked in `wo_io_wait` with a fiber parked on a `call`
whose reply will never arrive, and the primary spinning in
`while (eng_settle_inboxes() > 0) {}` if two shards can route the same payload
to each other indefinitely. The second is cheap to rule out with a counter.

## Fix design — settled 2026-09-06 (`readiness: ready`)

The root cause is a **broken invariant**, not a stray double free. `wo_db_rpc`
states the invariant plainly (`vm.c:281`): *the args are ENCODED into engine
slots on THIS thread — VM heaps are never read cross-shard.* The DB path
marshals. But cross-shard actor `send`/`call` (`vm.c:1098-1112`) does
`e->payload = msg_val` — it **pointer-shares** the message into the receiver's
shard. A worker then reads and eventually drops an object that lives in the
sender's arena, and the double free, the class-0 forge and the modulo livelock
are all downstream of that single violation.

### Decision 1 — marshal cross-shard messages (root fix)

Cross-shard `send` and `call` copy the message into the receiver's arena on the
crossing, exactly as `wo_db_rpc` already marshals its args. No pointer crosses
an arena boundary, so the double-free class is eliminated **by construction** —
and, importantly, the exact aliasing *site* need not be localised, because the
fix removes the shared pointer rather than the specific graph that aliased it.
It restores the "heaps are never read cross-shard" invariant the actor path
currently breaks, and it closes the latent hazard beyond the double free: a
worker reading sender-arena fields is unsafe under GC or compaction even when
the ownership happens to be clean. The cost is a copy per cross-shard message —
the same cost the DB RPC already pays, and correctness outranks the zero-copy
the current path was reaching for. Same-shard send is unchanged (the arena is
shared, the pointer move is correct — which is why `WO_SHARDS=1` never failed).

### Decision 2 — fix the modulo alias and bound the shard id (this iteration)

`wo_route_free` pushes to `INBOX[shard_id % nshards]` while `wo_drop_obj`
compares the **unmasked** `shard_id` against `rt->shard_id`; that mismatch is
what makes a stale free self-route forever instead of resolving. The two sites
are made consistent, **and** both assert `shard_id < nshards` — an out-of-range
id is impossible for a live object, so hitting it is a corrupt or freed header
and must trap loudly rather than route somewhere. This lands with the root fix
because it is the guard that would have turned the original silent livelock into
an immediate diagnostic.

### Decision 3 — poison-on-free is a follow-up, not this iteration

The deeper defensive fix — stamping a freed block's header (a free bit in
`flags`, or a poison `class_id`) so a NULL freelist link can never forge a valid
class-0 object — is deferred to its own story. Decision 2's bounds assert already
catches the specific corrupt-header shape this bug produces at route time; the
general poison is broader and separable.

### Decision 4 — prove against the existing repro; a minimal corpus fixture is a follow-up

The marshal fix is proven against the `archive/porch-idempotency` reproduction:
recover the branch, cherry-pick `9dca0b4`, and run `scripts/web-app-accept.sh`
sections 18a–18h and 19 to stability (the idempotency legs that failed one run
in six). A minimal, deterministic corpus fixture — a cross-shard `send` of an
object carrying an owned subtree (`multi`/`Text`), both sides then dropping,
under `WO_SHARDS>1` and ASan — is worth pinning but is its own follow-up; it is
not required to land the fix.

### Phases

- **A — marshal.** Make cross-shard `send`/`call` (kinds 0 and 5) copy the
  payload into the receiver's arena on the crossing, mirroring `wo_db_rpc`. The
  monitor path (kind 7) carries a payload too and is audited the same way.
  Same-shard paths untouched.
- **B — the modulo/bounds guard.** Align the `shard_id` comparison in
  `wo_drop_obj` with the masking in `wo_route_free`, and assert `shard_id <
  nshards` at both the route and the home-check.
- **C — prove and close.** Re-run the archive repro's 18a–18h/19 to stability
  under ASan, confirm the settle loop no longer spins, and unblock
  [porch 9](../porch/09-idempotent-replay.md).

### Acceptance criteria

- **Given** a cross-shard `send`/`call` of an object with an owned subtree,
  **when** both the sender's graph and the receiver drop, **then** each block is
  freed exactly once and no free is routed across an arena boundary.
- **Given** the `archive/porch-idempotency` repro under `WO_SHARDS>1` and ASan,
  **when** sections 18a–18h and 19 run repeatedly, **then** they are stable —
  the one-run-in-six idempotency hang is gone — and the settle loop terminates.
- **Given** a header carrying a `shard_id >= nshards`, **when** it reaches the
  drop/route path, **then** it traps loudly rather than self-routing.
- **Given** every same-shard workload, **when** the runtime battery and corpus
  run, **then** they are byte-for-byte unchanged — the marshal cost falls only
  on the cross-shard path.

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

- **Fixing the porch feature that found it.** That is
  [porch 9](../porch/09-idempotent-replay.md), already written; it only needs
  this to land first.
- **Poison-on-free** (decision 3) — a separate defensive story: stamp a freed
  header so a NULL freelist link can never forge a valid class-0 object,
  trapping any stale drop rather than misreading it. Needs a language-track
  number when picked up.
- **A minimal deterministic corpus fixture** (decision 4) — a cross-shard `send`
  of an object with an owned subtree, both sides dropping, under `WO_SHARDS>1` +
  ASan. Worth pinning; its own follow-up.
- **The two smaller runtime defects found alongside** (above): the
  `try EXPR catch (e) nil` Int-0-vs-trap ambiguity and the `json.decode ... as T`
  cross-return-boundary corruption. Both worked around in the archived code;
  each deserves its own minimal fixture and fix, neither blocks this.
