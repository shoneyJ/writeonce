---
track: porch
iteration: "4"
status: pending
readiness: ready
---

# porch 4 — CSRF: tokens that are unguessable, bound, and spendable once

> Part of [Story — `porch`, the writeonce web framework](00-story.md).
> Source: [the Fiber parity study](../../plan/exploration/fiber/00-fiber-parity.md) §2,
> re-checked 2026-09-06 against `.dev/reference/fiber` (v3, `3ca9a9d`):
> `middleware/csrf` — its hybrid double-submit-plus-session-stored model
> (`csrf.go`, `session_manager.go`), `SingleUseToken`, the
> origin/referer/`Sec-Fetch-Site` checks, and `KeyGenerator` = `utils.SecureToken`.
> Needs [2](02-randomness-and-cookies.md) for randomness and cookies, and
> [3](03-sessions.md) for something to bind a token to.

## Goals

- **Tokens that cannot be guessed or forged.** Minted from the iteration-2
  builtin, verified with `ct_eq`. This is the iteration that could not have been
  written honestly before phase A of 2 existed, which is the whole reason the
  track is ordered this way.
- **Bound to a session, not floating.** An unbound token is a token an attacker
  can fetch for themselves and replay against a victim. Binding is what makes
  the defence real, and it is why this follows sessions rather than preceding
  them.
- **Origin checking as the cheap second layer.** Fiber ships `TrustedOrigins`
  alongside the token. Same-site cookies plus an origin check stop most of what
  tokens stop, for almost no cost — and the two layers fail differently, which
  is the argument for having both.
- **Single-use where it matters.** Fiber's `SingleUseToken` exists because a
  long-lived token in a browser history or a referrer header is a credential
  left lying around. Decide which routes get it rather than making everything
  pay.
- **Refusals that are distinguishable.** Missing, stale, foreign-origin and
  already-spent must be told apart in the logs, or nobody can debug a form that
  stopped working.

## Decisions locked (brainstorm 2026-09-06)

1. **Fiber's hybrid transport: a session-stored token AND a double-submit cookie
   compare, both must pass.** Because iteration 3's session row carries no
   payload bag, the token lives in a dedicated framework `@table` keyed by the
   token with a `session_id` index — the "own table keyed by session id" pattern
   iteration 3 established. A CSRF cookie carries the same token; the client also
   echoes it in a form field or header. Verify requires (a) the echoed token
   equals the cookie value, and (b) the stored row exists and binds to the
   current session. The stored row is the authority, so signing the CSRF cookie
   is optional and not relied upon — the token is high-entropy and a tampered
   cookie simply misses the row. **porch has no CSRF story for sessionless apps,
   said plainly rather than shipping the weaker double-submit-only silently.**
2. **Single-use is opt-in per route; the default token is multi-use.** Matches
   fiber (`SingleUseToken` defaults off). A multi-use token is valid until idle
   expiry and has no double-click problem at all, so the sharp edge shrinks to
   exactly the routes that opt in. Worked examples: the storefront checkout is
   single-use, the site's admin edit is multi-use.
3. **A double-submitted single-use token yields a distinct `SPENT` refusal, and
   CSRF does not couple to idempotency.** The story floated reusing iteration 1's
   idempotency so a replay returns the same response — but that middleware is
   reverted and blocked on the lang-41 arena hang, so coupling would drag CSRF
   behind that blocker. Instead the spent-token refusal is its own class,
   distinguishable in the logs from forged/missing/stale, so an app can present
   "already submitted" rather than a raw 403. True exactly-once *execution* is
   idempotency's job (iteration 9), a separate concern.
4. **No actor pool; not blocked on lang-41.** Like sessions, the `CsrfToken`
   table is plain `@table` CRUD — insert on mint, load on verify, delete on spend
   or rotation. None of it is the read-modify-write that iteration 1's per-key
   actor pool exists for, so this iteration rides the same gate-green DB path the
   storefront uses and is unblocked today.
5. **Refusal classes are distinguishable in logs, opaque in the body.** `MISSING`,
   `FORGED` (echo/cookie mismatch or unknown token), `STALE` (expired row),
   `FOREIGN` (origin/referer untrusted), `SPENT` (single-use replay) are logged
   distinctly; the response body says only "forbidden" and never which check
   failed.

## Phases

### Phase A — mint and verify

- The `CsrfToken` `@table`: `token @unique`, `session_id` (indexed for
  rotation cleanup), `created_at` (wall-clock, lazy idle expiry like sessions).
  Mint draws from iteration 2's `random_bytes`, stores the row, and sets the
  CSRF cookie carrying the token.
- Verify does the hybrid check (decision 1) with `ct_eq` on the token halves,
  then the session-binding lookup.
- Extraction from where forms and fetch clients actually put it: a form field
  (`form_values`, which exists) and a header. A query parameter is never
  allowed — it leaks into logs and referrers.
- Verify: a valid token passes; altered, absent and foreign tokens each fail
  distinctly; a token minted for session A presented under session B is refused.

### Phase B — the middleware and safe-method policy

- A `Csrf` middleware gating unsafe methods only. `GET`/`HEAD`/`OPTIONS` pass
  untouched — and a `GET` of a form page is where the token is minted for the
  form that will submit it — or every link on the site breaks.
- Origin and `Referer` checking against a configured trusted set (the app's own
  host, which behind the proxy is the forwarded `Host` that `HostAllow` already
  validates, plus configured extras), including the awkward cases fiber handles:
  absent Origin (fall back to Referer on HTTPS, allow on plain HTTP where it
  cannot be told), `null` origin, and a same-origin request without the header.
  A `Sec-Fetch-Site` the browser would never have sent is rejected.
- Failure is a distinct status with a body that does not leak which check failed
  (decision 5).
- Verify: the site's admin edit flow works through the middleware; unsafe
  methods without a token are refused; safe methods are unaffected.

### Phase C — single use and rotation

- Mark-spent-on-use (delete the row) for the routes that opt in (decision 2); a
  double-click produces the `SPENT` class (decision 3), not a raw 403.
- Rotate on privilege change, matching the session-id rotation from iteration 3:
  when the session id rotates at login, delete the `CsrfToken` rows for the old
  session id through the `session_id` index — the same shape as sessions'
  revoke-all-by-principal.
- Verify: a spent token is refused; a double-click produces a comprehensible
  outcome; a token outstanding for the pre-login session is inert after login.

### Phase D — the gate and the ledger

- Both serving gates: the site's admin edit is the natural CSRF subject (and the
  phase migrates it from its current per-request bearer check onto session +
  CSRF), the storefront's checkout the natural single-use subject.
- Ledger and status board, standup questions answered including the
  `.dev/reference` projects used.
- Verify: `just web-app`, `just site`, `just linkcheck` green.

## Acceptance Criteria

- **Given** an unsafe request with no token, **when** it is dispatched, **then**
  it is refused and the handler never runs.
- **Given** a token minted for session A, **when** it is presented with session
  B's cookie, **then** it is refused — binding is enforced, not decorative.
- **Given** a token whose echoed copy and cookie disagree, **when** it is
  verified, **then** it is refused — the double-submit half is enforced too.
- **Given** a token altered by one bit, **when** it is verified, **then** it is
  refused in constant time.
- **Given** a request from an untrusted origin carrying an otherwise valid
  token, **when** it arrives, **then** it is refused — the layers are
  independent.
- **Given** a safe method (`GET`, `HEAD`, `OPTIONS`), **when** it arrives with
  no token, **then** it passes untouched.
- **Given** a single-use token, **when** it is submitted twice, **then** the
  second attempt is refused as `SPENT`, told apart from a forged token in the
  logs.
- **Given** login, **when** the session id rotates, **then** the outstanding
  token for the old session is no longer valid.
- **Given** each refusal class, **when** the logs are read, **then** missing,
  stale, foreign-origin and spent are told apart — while the response body
  tells the client none of it.

## Out Of Scope

- **CORS.** Already shipped (`Cors` before/after middleware). It is a different
  problem — CORS decides who may *read* a response, CSRF stops a forged write.
  Conflating them is the most common way both get misconfigured.
- **Same-site cookie attributes as the whole answer.** Iteration 2 sets a safe
  `SameSite` default and it does real work, but it is a browser behaviour, not a
  server guarantee, and older clients ignore it.
- **Captcha, rate-limited forms, bot defence.** Rate limiting shipped in
  iteration [1](01-store-backed-middleware.md); the rest is not porch's.
- **Encrypted or stateless tokens.** No symmetric cipher exists, and a
  stateless token cannot be revoked or spent once.
- **Exactly-once execution of a replayed unsafe request.** That is idempotency's
  job — iteration [9](09-idempotent-replay.md) — not CSRF's (decision 3). CSRF
  only makes the double-submit refusal comprehensible.
- **CSRF for sessionless apps.** The hybrid model binds to a session; an app
  with no sessions has no CSRF story here (decision 1).

## Info

The one language dependency is entirely upstream: iteration 2's `random_bytes`
and cookie helpers. This iteration is pure `.wo` on top of them plus the
`@table` engine — no new runtime work, and, like sessions, no dependence on the
per-key actor pool or on the lang-41 fix. Origin and Referer matching is string
comparison against the trusted set; form-field extraction is `form_values`,
which already exists.
