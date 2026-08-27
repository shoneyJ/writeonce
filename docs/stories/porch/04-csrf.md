---
track: porch
iteration: "4"
status: pending
readiness: refine
---

# porch 4 — CSRF: tokens that are unguessable, bound, and spendable once

> Part of [Story — `porch`, the writeonce web framework](00-story.md).
> Source: [the Fiber parity study](../../plan/exploration/fiber/00-fiber-parity.md) §2.
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

## Phases

### Phase A — mint and verify

- Token generation, storage keyed by session, and constant-time verification.
- Decide the transport: a dedicated cookie plus a form field (double-submit), or
  session-stored plus a form field. The second needs no second cookie and is the
  stronger of the two once sessions exist.
- Extraction from where forms and fetch clients actually put it: a form field, a
  header, and decide whether a query parameter is ever allowed (it should not be
  — it leaks into logs and referrers).
- Verify: a valid token passes; altered, absent and foreign tokens each fail
  distinctly.

### Phase B — the middleware and safe-method policy

- A `Csrf` middleware gating unsafe methods only. `GET`/`HEAD`/`OPTIONS` must
  pass untouched or every link on the site breaks.
- Origin and `Referer` checking against a configured trusted set, including the
  awkward cases: absent origin, `null` origin, and a same-origin request that
  arrives without the header.
- Failure is a distinct status with a body that does not leak whether the token
  was wrong or merely stale.
- Verify: the site's admin edit flow works through the middleware; unsafe
  methods without a token are refused; safe methods are unaffected.

### Phase C — single use and rotation

- Mark-spent-on-use for the routes that opt in, and decide what happens to a
  double-submitted form (the user double-clicking is not an attack, and treating
  it as one is a support ticket).
- Rotate on privilege change, matching the session-id rotation from iteration 3.
- Verify: a spent token is refused; a double-click produces a comprehensible
  outcome rather than a raw 403.

### Phase D — the gate and the ledger

- Both serving gates: the site's admin edit is the natural CSRF subject, the
  storefront's checkout the natural single-use subject.
- Ledger and status board.
- Verify: `just web-app`, `just site`, `just linkcheck` green.

## Acceptance Criteria

- **Given** an unsafe request with no token, **when** it is dispatched, **then**
  it is refused and the handler never runs.
- **Given** a token minted for session A, **when** it is presented with session
  B's cookie, **then** it is refused — binding is enforced, not decorative.
- **Given** a token altered by one bit, **when** it is verified, **then** it is
  refused in constant time.
- **Given** a request from an untrusted origin carrying an otherwise valid
  token, **when** it arrives, **then** it is refused — the layers are
  independent.
- **Given** a safe method (`GET`, `HEAD`, `OPTIONS`), **when** it arrives with
  no token, **then** it passes untouched.
- **Given** a single-use token, **when** it is submitted twice, **then** the
  second attempt is refused and the outcome is distinguishable from a forged
  token in the logs.
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

## Info

Forks the spec must settle:

1. **Double-submit cookie, or session-stored token?** Double-submit needs no
   store and works without sessions; session-stored needs no second cookie and
   is strictly stronger. Since iteration 3 lands first, leaning session-stored —
   and if so, say plainly that porch has no CSRF story for sessionless apps
   rather than shipping the weaker one silently.
2. **Which routes default to single-use?** All of them is safest and the most
   annoying; opt-in is pleasant and easy to forget on the one route that
   mattered. Leaning opt-in with the checkout as the worked example, because a
   default nobody can live with gets disabled wholesale.
3. **What happens when a form is submitted twice by a human?** This is the fork
   that decides whether the feature is usable. Iteration
   [1](01-store-backed-middleware.md)'s idempotency machinery may be the honest
   answer — the same key, replayed, gets the same response — which would make
   these two features compose rather than collide.
