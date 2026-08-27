---
slice: "24"          # the story that owns the status; see stories/24-chat-websocket-workload.md
status: in-progress
---

# Active slice — chat + actor lifecycle (iteration 24, absorbing 31 + 34)

Branch `chat-ws-lifecycle`. Spec:
[`superpowers/specs/2026-08-23-chat-websocket-actor-lifecycle-design.md`](superpowers/specs/2026-08-23-chat-websocket-actor-lifecycle-design.md)
· plan:
[`superpowers/plans/2026-08-23-chat-ws-lifecycle.md`](superpowers/plans/2026-08-23-chat-ws-lifecycle.md)
· board: [`stories/00-status.md`](stories/00-status.md).

## Progress (2026-08-23)

- ✅ **T1 crypto** (`d14fa9f`): sha1/sha256/hmac_sha256, ids 85–87, RFC
  vectors 18/0, corpus pin. Story 34's C-builtin resolution delivered.
- ✅ **T2 bounded mailboxes** (`92754a8`): cap 1024 + `WO_MAILBOX`,
  sender-side atomic reserve, WO_T_ACTOR (trap 13) catchable. Plus a
  pre-existing compiler fix: try-arm Text places (bare `e.msg`) now
  copy before the arm's scope dies (was ASan use-after-free + SEGV).
- ✅ **T6 WS upgrade** (`79cfa01`): `ws_accept` + accept-key + the
  101 hijack sentinel; plain HTTP byte-identical (web-app 26/26).
- ✅ **T7 frame codec** (`7ad2ced`): pure-`.wo` RFC 6455 parse/serialize,
  probe-verified against the RFC's own bytes.
- ✅ **T3 call/reply** (`ed69841`): `call` parks + typed scalar reply
  (WO-E226 through actor-M erasure); actor DEATH landed with it —
  callers never hang (mid-call + to-dead both trap catchably). Fixed
  TRAPF's fiber-death leak/dangle en route.

- ✅ **T4 monitor + T5 time.after** (`56fe41a`, ids 89/90): the lifecycle
  core. Corpus fixtures monitor-death, timer-delivery, timer-generation.
- 🔄 **T8 chat sample + T9 gate** (`6d729cc`, then `bbe0216`): the sample
  and all five gate legs exist and run.

Every landed task: full battery 12/12, fresh-built.

## Verified 2026-08-27 (branch merged up to master)

Merged `master` in (clean; the porch rename means chat now says `use porch/...`
and its `[deps]` key is `porch`). Baseline on this branch: **18 runtime suites
× both dispatch flavors, 0 fail, `cli_smoke: OK`.**

`just chat` at `CHAT_SOAK=20` — **11 of 12 legs green**, including the two the
plan required and the gate was missing (`WO_SHARDS=1`, `WO_MAILBOX=8`).

**Three of the four failures found on 2026-08-27 were stale build artifacts,
not code.** Switching branches leaves `compiler/_build/` and `runtime/build/`
holding the *other* branch's binaries: a `woc` emitting `.wob` v7 against a
runtime expecting v6 reports only `wovm: unsupported version 7`, which the gate
surfaces as "no listener". `runtime/build/wovm_asan` bit the same way. **Rebuild
both after any branch switch** (`just woc-build`, `make -C runtime wovm-asan`)
before believing a gate failure.

**The remaining failure is a real bug and is NOT fixed** —
[`2026-08-27-chat-drain-finding.md`](2026-08-27-chat-drain-finding.md). On a
*fresh* server the SIGTERM drain leaves a client at EOF with no close frame in
5 of 16 runs. Traced: main → Registry → Room → Writer; the Registry runs but
the **Room never processes its shutdown message**, so the Writer's close branch
never runs. Ruled out: the spin budget (a 1 s wall-clock deadline still failed
2 of 12), `dummy_writer()` spawning during shutdown, and write failure. The
gate had been hiding it by draining a server the soak had already warmed.

## Pending

- 🔴 **The drain guarantee — the one blocker.** A `send` issued before the
  stop flag must be delivered before the engine stops. `main` cannot park
  after the flag (a park unwinds), so it spins, and **spinning is not a
  barrier** — the evidence says the Room's shard never adopts its inbox, not
  that it adopts it late. This is a semantic guarantee belonging to the actor
  lifecycle (31), not a tuning parameter: it wants a stated rule in the
  runtime lifecycle docs and a corpus fixture, not a bigger spin count.
  **Nothing else in the slice should land before this**, because the drain is
  half of what "actor lifecycle" means.
- ⬜ **T9 remainder** — the 1k soak has only been run trimmed
  (`CHAT_SOAK=20`); run it at the default 1000 once the drain is fixed.
- ⬜ **T10 closeout** — stories 24/31/34 → `status: done` with banners (note the
  scalar-reply v1 narrowing + three-argument monitor deviations), board
  standup entry, graph nodes, framework README ledger rows, runtime +
  chat CODE-LOGIC sections, delete this marker. Final battery.

This file is deleted when the slice lands (board convention). It lives flat in
`docs/` rather than a status folder — since 2026-08-26 no directory in this repo
encodes state; `status:` above is the only place it is recorded.
