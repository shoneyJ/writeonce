# In progress — chat + actor lifecycle (iteration 24, absorbing 31 + 34)

Active slice, branch `chat-ws-lifecycle`. Spec:
[`../superpowers/specs/2026-08-23-chat-websocket-actor-lifecycle-design.md`](../superpowers/specs/2026-08-23-chat-websocket-actor-lifecycle-design.md)
· plan:
[`../superpowers/plans/2026-08-23-chat-ws-lifecycle.md`](../superpowers/plans/2026-08-23-chat-ws-lifecycle.md).

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

Every landed task: full battery 12/12, fresh-built.

## Pending

- ⬜ **T4 monitor(watched, observer, msg)** — id 89. Most of the death
  machinery exists (`actor_die`); T4 adds the per-actor monitor list,
  the death walk delivering the observer's own M-typed notice,
  monitor-of-already-dead firing immediately, full-observer notice =
  disclosed stderr drop. Three-argument form (spec deviation, disclosed
  in the plan: the caller may be `main`, which has no mailbox).
- ⬜ **T5 time.after(ms, addr, msg)** — id 90, one-shot, no cancel;
  rides the T4 deadline plumbing; delivery = runtime send (full = drop
  + stderr line, dead = silent). Corpus: timer-delivery,
  timer-generation (the cancel idiom). Both WO_IO backends.
- ⬜ **T8 chat sample** — docs/examples/chat: registry (`call`'s first
  consumer), room actors (cap-trap drops slow members, `monitor` reaps
  dead writers), reader/writer actor pair per connection over
  ws_accept/wsframe; SIGTERM close choreography.
- ⬜ **T9 chat gate** — scripts/chat-accept.sh + raw-RFC6455 python
  client; the spec's five checks (functional cross-shard — also the
  deferred cross-shard `call` proof — handshake vector, 1k soak with a
  `WO_MAILBOX=8` sub-run, drain under both backends + ASan, battery).
- ⬜ **T10 closeout** — stories 24/31/34 → done/ with banners (note the
  scalar-reply v1 narrowing + three-argument monitor deviations), board
  standup entry, graph nodes, framework README ledger rows, runtime +
  chat CODE-LOGIC sections, delete this marker. Final battery.

This file is deleted when the slice lands (board convention).
