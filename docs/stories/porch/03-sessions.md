---
track: porch
iteration: "3"
status: refine
---

# porch 3 — sessions: server-side state, revocable, durable

> Part of [Story — `porch`, the writeonce web framework](00-story.md).
> Source: [the Fiber parity study](../../plan/exploration/fiber/00-fiber-parity.md) §2.
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
  since you authenticated". Fiber ships both (`IdleTimeout`, `AbsoluteTimeout`)
  and a session with only the first never expires for an active attacker.
- **Durability is the differentiator.** Fiber's default store is in-memory: a
  restart logs everyone out. porch's sessions ride the WAL, so they survive —
  and the gate proves it, because an unexercised durability claim is not a
  claim.
- **Revocation that works.** Logout, and "log out everywhere for this
  principal" — the second is what a password change needs, and it is nearly free
  once sessions are rows with an owner column.

## Phases

### Phase A — the session table and its lifecycle

- The `@table` class: random id, principal, created-at, last-seen, and whatever
  payload shape fork 1 settles. Secondary index on principal, because
  "revoke all for this user" is an equality probe and that is the only index
  shape the engine has.
- Create, touch, expire and delete, with expiry evaluated on access (the lazy
  discipline iteration 1 established — there is still no timer).
- Verify: rows replay across a restart; an expired row is pruned when touched.

### Phase B — the middleware

- A `Session` middleware whose `before` reads the signed cookie, loads the row,
  checks both timeouts, and attaches identity. Decide whether it sets
  `req.principal` or writes into `req.ctx` — `principal` is the field the
  framework already means for identity, so it should be that unless the session
  carries more than identity.
- Rotate the id on privilege change (login especially). Session fixation is the
  attack that a login which reuses the pre-login id walks straight into.
- A missing, expired, unknown or tampered cookie all end at the same place —
  unauthenticated — but for distinguishable reasons, because debugging a login
  loop without that distinction is miserable.
- Verify: each of those four cases behaves; the id changes across login.

### Phase C — login, logout, revoke-all

- The three flows end to end in a sample, using porch's existing auth pieces
  (`bearer_token`, `basic_credentials`, `ct_eq`) for the credential check —
  sessions are about *keeping* identity, not establishing it.
- Logout deletes the row and clears the cookie with matching attributes.
- Revoke-all deletes by principal and is proven with two concurrent sessions.
- Verify: after logout the old cookie is inert even though it is still
  well-signed — the point of server-side state.

### Phase D — the gate and the ledger

- Extend `scripts/site-accept.sh` rather than only the storefront: the site has
  an admin route that currently checks a bearer token per request, which is
  exactly the thing sessions replace.
- Ledger rows, status board entry, and the standup questions answered.
- Verify: `just web-app`, `just site`, `just linkcheck` green.

## Acceptance Criteria

- **Given** a valid session cookie, **when** a request arrives, **then**
  identity is attached and the row's last-seen advances.
- **Given** a session whose signature is valid but whose row was deleted,
  **when** it is presented, **then** the request is unauthenticated — a
  well-formed cookie is not authority.
- **Given** an idle timeout of T, **when** a session is unused for longer than
  T, **then** it is rejected and its row pruned on that access.
- **Given** an absolute timeout of A, **when** a session is *continuously
  active* past A, **then** it is still rejected — the criterion an idle-only
  implementation fails.
- **Given** an anonymous session id, **when** the user logs in, **then** the id
  is rotated and the pre-login id is inert.
- **Given** active sessions and a SIGTERM plus restart, **when** the same
  cookies return, **then** the users are still logged in, replayed from the WAL.
- **Given** two sessions for one principal, **when** revoke-all runs, **then**
  both are inert and other principals are untouched.
- **Given** logout, **when** the cleared cookie is compared to the one that set
  it, **then** the attributes match, so the browser actually drops it.

## Out Of Scope

- **Flash messages and a general session bag.** A typed session with a known
  shape first; an untyped `map<Text,Text>` payload is a decision to defer, not
  a default to adopt.
- **OAuth, OIDC, SSO, "log in with X".** Every one needs an outbound socket,
  which does not exist — language
  [iteration 38](../language-runtime-database/38-content-platform-capabilities.md).
- **Remember-me tokens** — a second, longer-lived credential class with its own
  rotation story. Its own slice.
- **CSRF** — iteration [4](04-csrf.md). Sessions make CSRF *possible* to do
  properly; they do not provide it.
- **Sliding-window renewal of the absolute timeout.** That is not what absolute
  means.

## Info

Forks the spec must settle:

1. **What does the session row carry beyond identity?** Just principal, or a
   payload? A payload wants a shape, and without generics the shape is either a
   declared class per app or `Text`. The framework's cache already stores `Text`
   and that is the compromise this repo dislikes most — so leaning: identity
   plus declared columns, and apps that want more keep their own table keyed by
   session id.
2. **Idle-timeout writes on every request.** Touching last-seen means a WAL
   write per request, which turns every read into a durable write. Options:
   write at a coarser granularity, or accept the cost and say so with a number
   from `just db-bench`. This is a real performance fork, not a detail.
3. **Does `Session` set `req.principal` or `req.ctx`?** `principal` is the
   framework's declared home for identity and auth middleware already writes it,
   so two writers of one field need a documented precedence.
