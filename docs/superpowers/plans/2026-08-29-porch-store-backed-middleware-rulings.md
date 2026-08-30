# porch 1 — decisions taken during execution

Fifteen rulings made while executing
[the plan](2026-08-29-porch-store-backed-middleware.md) without stopping to ask.
Each says what was decided, why, and what it costs if wrong. **One is known
wrong and was overturned mid-flight (R9/R14).**

| # | Ruling | Cost if wrong |
| --- | --- | --- |
| — | Work on `dev`, not a fresh worktree — `dev` is the project's designated development branch, so this is not the "never implement on master" case | Interleaves with other dev commits; separable by the `porch-store` prefix |
| R1 | The pool takes ONE message class with a `kind: Int` discriminator, not a `receive` per message type. An actor handle is typed to a single message class, so a second `receive` compiles but is unreachable. Verified by fixture before ruling | Task 2's protocol restructured; contained to `keypool.wo` |
| R2 | Task 4's file list had to include `keypool.wo` — its step 4 edits the actor arm that lives there | None; a documentation correction |
| R3 | "Never delete-then-insert" bans that pair as an UPDATE. Deleting an expired row is required and is not covered | An implementer leaves rows unpruned, leaking one per key ever seen |
| R4 | `call`'s reply must be a copyable scalar (WO-E226) and every `receive` program-wide must agree, so **the response travels through the `@table`, not the mailbox** | Task 4's protocol changes shape again |
| R5 | Task 3 calls the `pool_count` wrapper, never `call()` directly | Fails to typecheck immediately — cheap to detect |
| R6 | `trust_proxy: true` with an absent `X-Forwarded-For` collapsed every client onto the key `"ip:"`. Falls back to `net.peer`. **My brief caused this** | None identified; the fallback is strictly safer than a shared bucket |
| R7 | Kept the `Limiter.after()` added beyond brief scope — "headers on both paths" cannot be met by stashing values nothing consumes | A little surface to remove later |
| R8 | Kept `curl` in the gate legs, with an up-front presence check so its absence fails loudly | One more host dependency for the gate |
| R9 | **WRONG — see R14.** Ruled that `Pool` being traced forces a one-slot re-wrap | The feature's headline property was unreachable as documented |
| R10 | The hardcoded `Retry-After: 1` on 503 stands; the brief requires the header present, not computed | Clients retry marginally too eagerly under sustained saturation |
| R11 | `Idempotent` becomes a `Handler` decorator, not a `Middleware` — the actor needs the route's handler, and only that slot exposes it | Users register it differently; caught at compile time |
| R12 | Overrode a reviewer's **Minor** to Important: caching any status meant a transient 500 replayed for 24h and a retry could never succeed | One extra fix round |
| R13 | Sent two Minors to a fix round against the usual rule, because one inflated the check count with an assertion that could never fail | One cheap extra round |
| R14 | **Overturns R9.** WO-E222 fires on the class `Pool`, not on `multi`; an actor can hold `slots: multi PoolSlot`. Tested by the final reviewer | Already paid: a README shipped prescribing a permanent 1-actor pool |
| R15 | **PARKED**: `pool_slots`/`pool_of` are compile-proven and precedent-backed but have zero call sites, so real N-actor sharding is gate-unproven | The sharding path could hold a runtime defect nobody has exercised |

## The gate is not stably green

Measured over ten consecutive runs of `scripts/web-app-accept.sh` after the
final fix wave: most runs report 0 or 1 failures, and the failing check is
almost always `idempotent-stop` / `idempotent-stop-2` — the SIGTERM teardown
assertions. One run lost **six** checks at once, with `000` status codes
meaning the server returned nothing at all.

Both symptoms trace to the C-runtime defect below, not to this feature's logic:
the failures are teardown timeouts and, when the runtime crashes, whole
sections. **Do not read "83 checks, 0 failures" as a stable result** — it is one
outcome of a distribution.

## Three C-runtime defects found, none fixed

They live in the C runtime, not in porch, and each is worked around here:

1. `try EXPR catch (e) nil` cannot distinguish a literal `Int 0` reply from a
   trap. Worked around by never packing a zero outcome code.
2. A `Text`/map value read off `json.decode(...) as T` is **corrupted** once
   embedded in a struct crossing a function-return boundary. Worked around by
   forcing fresh text with `.. ""` on every field copied out of a decoded record.
3. Under concurrent `call()`-parked callers with real table I/O, the process
   sometimes hangs after `main()` returns, with an observed **SIGSEGV**.
   Localised by `gdb` to `wo_arena_alloc` / `wo_str_new` / `vm_run`, and it
   fires more readily at higher sequential insert+delete volume against one key
   (N=4/5 crashed; N=1-3 clean over 12+ trials).

The third is the reason the gate flakes, and it is the highest-value thing on
this branch to fix next — ahead of any remaining porch work.
