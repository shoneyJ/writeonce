---
track: porch
iteration: "3"
status: pending
readiness: ready
---

# porch 3 — sessions: server-side state, revocable, durable

> Part of [Story — `porch`, the writeonce web framework](00-story.md).
> Source: [the Fiber parity study](../../plan/exploration/fiber/00-fiber-parity.md) §2,
> re-checked 2026-09-06 against `.dev/reference/fiber` (v3, `3ca9a9d`):
> `middleware/session` — its `IdleTimeout`/`AbsoluteTimeout` config (it panics
> if absolute < idle), and `Regenerate()`, documented as the post-authentication
> fixation defence.
> Needs [iteration 2](02-randomness-and-cookies.md) (a random session id and a
> signed cookie to carry it) and inherits the store convention from
> [iteration 1](01-store-backed-middleware.md).

## Goals

- **A session is a `@table` row keyed by a random id; the cookie carries only
  the id.** The alternative — a signed cookie carrying the whole payload — needs
  no store but cannot be revoked, and revocation is not optional for a real
  login. This is the fork iteration 1's store convention exists to answer.
- **Both timeouts, because they answer different questions.** Idle timeout
  bounds "how long since you did anything"; absolute timeout bounds "how long
  since you authenticated". Fiber ships both and a session with only the first
  never expires for an active attacker.
- **Durability is the differentiator.** Fiber's default store is in-memory: a
  restart logs everyone out. porch's sessions ride the WAL, so they survive —
  and the gate proves it, because an unexercised durability claim is not a
  claim.
- **Revocation that works.** Logout, and "log out everywhere for this
  principal" — the second is what a password change needs, and it is nearly free
  once sessions are rows with an owner column.

## Decisions locked (brainstorm 2026-09-06)

1. **The row is a pure auth primitive: `id`, `principal`, `created_at`,
   `last_seen` — no payload column.** An app wanting cart, flash or preferences
   keeps its own typed `@table` keyed by the session id. This rejects the
   untyped `data: Text` bag (fiber's shape) precisely because it is the
   Text-as-payload compromise the repo dislikes most — untyped, unversioned, and
   every read a `json.decode` exposed to the lang-41 decode-corruption gotcha.
2. **Timestamps are wall-clock (`time.now`, epoch seconds), not monotonic
   `time.ticks`.** This corrects an inherited assumption: iteration 1's store
   used `time.ticks` (µs monotonic), which is right for short rate-limit windows
   but **resets on restart**. Sessions survive restart by design, so a monotonic
   timestamp would make every idle/absolute check wrong the moment the process
   bounces. `time.now` is the only correct source here.
3. **Rotation is: login always mints a fresh id and a fresh row,
   unconditionally.** Any session id on the incoming request is ignored when
   establishing the logged-in session. That defeats fixation without an
   anonymous-session model — an attacker can plant a cookie regardless of
   whether the app creates pre-login sessions, so the fix is simply that login
   never reuses an incoming id. Matches fiber's `Regenerate()`.
4. **`last_seen` is written with a throttled touch, not on every request.** It
   advances (and WAL-writes) only when it is already older than a fraction of
   the idle window — `idle/20`, floored at a few seconds. This bounds writes to
   roughly one per active session per that interval no matter the request rate,
   so a hot read endpoint behind a session stops turning every read into a
   durable write. The cost is a bounded idle overshoot of at most `idle/20`,
   which is documented, not hidden. The granularity is a knob.
5. **`Session` writes `req.principal`; chain order is the precedence.** It writes
   the framework's declared identity field — the same slot `BearerAuth` and
   `BasicAuth` write. A valid session sets it; a missing, expired, unknown or
   tampered cookie leaves it empty and the route's policy decides. The rule for
   two writers is documented: last middleware in the registration chain wins,
   and `Session` is not stacked with a token-auth on the same route — sessions
   *replace* per-request auth, which is exactly what phase D does to the site's
   admin route.
6. **Config invariant, borrowed from fiber: absolute timeout must be ≥ idle
   timeout.** An absolute shorter than idle is a misconfiguration; the framework
   refuses it rather than silently making idle unreachable.

## Phases

### Phase A — the session table and its lifecycle

- The `@table` class with `id @unique`, `principal`, `created_at`, `last_seen`
  (all wall-clock), and two indexes — `index: [id]` for the load and
  `index: [principal]` for revoke-all. The non-unique secondary index on a
  plain column is the same shape `skill-catalog` already uses, so revoke-all is
  an equality probe, the only index shape the engine has.
- Create, touch (throttled per decision 4), expire and delete, with expiry
  evaluated on access — the lazy discipline iteration 1 established, since there
  is still no timer. Expiry is `now - created_at > absolute` OR
  `now - last_seen > idle`; a rejected session's row is deleted on that access.
- Verify: rows replay across a restart; an expired row is pruned when touched;
  a throttled touch does not write when `last_seen` is fresh.

### Phase B — the middleware

- A `Session` middleware whose `before` reads the signed cookie (iteration 2's
  `verify` with the app-supplied key), extracts the id, loads the row, checks
  both timeouts, and sets `req.principal` on success (decision 5).
- Rotate the id on privilege change — login especially — per decision 3: a fresh
  random id and row, the old row deleted, a fresh signed cookie set.
- A missing, expired, unknown or tampered cookie all end at the same place —
  unauthenticated — but for **distinguishable** reasons (no cookie / bad
  signature / no row / timed out), because debugging a login loop without that
  distinction is miserable. The reasons are logged, not returned.
- Verify: each of those four cases behaves; the id changes across login; the
  pre-login id is inert afterward.

### Phase C — login, logout, revoke-all

- The three flows end to end in a sample, using porch's existing auth pieces
  (`bearer_token`, `basic_credentials`, `ct_eq`) for the credential check —
  sessions are about *keeping* identity, not establishing it.
- Login mints the fresh row and cookie (decision 3). Logout deletes the row and
  clears the cookie with matching attributes (iteration 2's clear-cookie).
  Revoke-all deletes by principal through the secondary index and is proven with
  two concurrent sessions.
- Verify: after logout the old cookie is inert even though it is still
  well-signed — the point of server-side state.

### Phase D — the gate and the ledger

- Extend `scripts/site-accept.sh` rather than only the storefront: the site has
  an admin route that currently checks a bearer token per request, which is
  exactly the thing sessions replace — so the migration is the proof.
- Ledger rows, status board entry, and the standup questions answered, including
  the `.dev/reference` projects used.
- Verify: `just web-app`, `just site`, `just linkcheck` green.

## Acceptance Criteria

- **Given** a valid session cookie, **when** a request arrives, **then**
  identity is attached to `req.principal` and the row's last-seen advances —
  subject to the throttle, so a fresh last-seen is not rewritten.
- **Given** a session whose signature is valid but whose row was deleted,
  **when** it is presented, **then** the request is unauthenticated — a
  well-formed cookie is not authority.
- **Given** an idle timeout of T, **when** a session is unused for longer than
  T, **then** it is rejected and its row pruned on that access — within the
  documented `T/20` overshoot.
- **Given** an absolute timeout of A, **when** a session is *continuously
  active* past A, **then** it is still rejected — the criterion an idle-only
  implementation fails.
- **Given** an anonymous or planted session id, **when** the user logs in,
  **then** a fresh id is minted and the pre-login id is inert.
- **Given** active sessions and a SIGTERM plus restart, **when** the same
  cookies return, **then** the users are still logged in, replayed from the WAL
  with wall-clock timestamps intact.
- **Given** two sessions for one principal, **when** revoke-all runs, **then**
  both are inert and other principals are untouched.
- **Given** logout, **when** the cleared cookie is compared to the one that set
  it, **then** the attributes match, so the browser actually drops it.
- **Given** a configuration with absolute timeout below idle timeout, **when**
  the app starts, **then** it is refused rather than run with an unreachable
  idle timeout.

## Out Of Scope

- **Flash messages and a general session bag.** A typed session with a known
  shape first; an untyped payload is deferred (decision 1), not adopted as a
  default. Apps needing per-session state keep their own keyed table.
- **OAuth, OIDC, SSO, "log in with X".** Every one needs an outbound socket,
  which does not exist — language
  [iteration 38](../language-runtime-database/38-content-platform-capabilities.md).
- **Remember-me tokens** — a second, longer-lived credential class with its own
  rotation story. Its own slice.
- **CSRF** — iteration [4](04-csrf.md). Sessions make CSRF *possible* to do
  properly; they do not provide it.
- **Sliding-window renewal of the absolute timeout.** That is not what absolute
  means; the throttled touch (decision 4) slides only `last_seen`, never
  `created_at`.

## Info

The throttle in decision 4 interacts with durability in a way worth stating: a
restart replays the last *written* `last_seen`, which may be up to `idle/20`
staler than the true last access. That only ever makes the idle timer slightly
conservative — a session logs out marginally earlier after a crash, never
later — so it is safe in the direction that matters.

The one language dependency is entirely upstream: iteration 2's `random_bytes`
and signed-cookie helpers. This iteration is pure `.wo` on top of them plus the
`@table` engine that iteration 1 already proved under the actor store — no new
runtime work.

It also does **not** need the per-key actor pool, and so is **not blocked on
lang-41** (unlike iteration 9). The pool exists for the rate limiter's atomic
read-modify-write counter; sessions have no counter. Create is an insert of a
fresh unique id, load is a read, touch is a last-writer-wins on `last_seen` (a
race writes ≈ the same value, harmless), delete is idempotent — all plain
`@table` CRUD over the same gate-green DB path the storefront and skill-catalog
use, never the `keypool` actor that provokes the hang. Confirmed 2026-09-06
against `.dev/reference/fiber` `middleware/session`: fiber enforces idle timeout
by refreshing a storage TTL on a `Save` at the end of **every** request (a write
per request its in-memory store makes cheap); porch's throttled lazy touch is
the durable-WAL equivalent. The one place sessions would have forced language
work — fiber's general `Set(key, any)` bag with `msgp` codegen and
`RegisterType` reflection — is closed by decision 1's no-payload rule, which
principle 13 (no reflection) would otherwise have collided with.
