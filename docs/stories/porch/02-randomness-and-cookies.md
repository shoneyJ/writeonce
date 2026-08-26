---
track: porch
iteration: "2"
status: refine
---

# porch 2 — randomness and cookies: the foundation three iterations stand on

> Part of [Story — `porch`, the writeonce web framework](00-story.md).
> Source: [the Fiber parity study](../../plan/exploration/fiber/00-fiber-parity.md) §0–§1.
>
> **The study's sharpest finding lives here.** porch's own README claimed signed
> cookies, CSRF and session integrity were "UNBLOCKED — the primitives exist
> since iteration 34". For CSRF and sessions that is **wrong**: SHA-256 and HMAC
> let a program *authenticate* a token, they cannot *mint* one, and writeonce has
> no source of randomness anywhere. An HMAC over a guessable session id is a
> signed guess. Phase A closes that before anything depends on it.

## Goals

- **A CSPRNG builtin in the runtime.** No `getrandom`, no `/dev/urandom` read,
  no CSPRNG builtin exists today — grep the runtime and nothing comes back. This
  is the one phase of this track that is **language-track work** (C, a new
  builtin id, a corpus fixture); it is here because porch is what needs it and
  splitting it across two tracks would hide the dependency.
- **Repeated response headers, which `Resp` structurally cannot express.**
  `Resp.headers` is a `map<Text, Text>`. A login response setting a session
  cookie *and* a flash cookie needs two `Set-Cookie` lines and the map can hold
  one. This is a change to porch's most public type, and deciding its shape is
  the real work of this iteration — the cookie formatting is the easy half.
- **Cookies in both directions.** Parse a `Cookie:` request header into
  something typed; build a `Set-Cookie` with the attributes that matter for
  security — `HttpOnly`, `Secure`, `SameSite`, `Max-Age`, `Path`, `Domain`.
- **Signed cookies.** HMAC-SHA256 over the value with constant-time comparison
  on the way back (`hmac_sha256` and `ct_eq` both already exist). This is the
  half that genuinely *was* unblocked by iteration 34, and it is what makes a
  cookie tamper-evident without a server-side lookup.
- **Correct the ledger row that started this.** The README's crypto line, and
  anything else claiming the sessions/CSRF path was already open.

## Phases

### Phase A — the runtime primitive (language-track work)

- Add a random-bytes builtin at the next free id (89 and 90 are reserved holes
  for language iteration 31's `monitor` and `time.after`, so this starts at 96).
- Source it from the kernel. Decide the failure mode explicitly: if the source
  is unavailable the builtin **refuses**, loudly. A CSPRNG that quietly degrades
  to something weaker is worse than none, because every layer above it will
  assume it worked.
- Corpus fixtures: the builtin's arity and type contract, and the refusal path.
  Statistical quality is not a corpus concern — the kernel's guarantee is the
  guarantee.
- Document it in the builtin-surface contract and the error catalog if it adds a
  diagnostic, in the same change. Both of those went stale once before by not
  doing this.
- Verify: `oop-e2e` green; the value differs across separate processes and is
  not derivable from the clock.

### Phase B — repeated response headers

- Settle fork 1 and change `Resp`. Every response builder
  (`ok_text`/`ok_json`/`ok_html`/`created_json`/`not_found`/…) and
  `serialize()` in `internal/serve.wo` move with it.
- Keep the single-value path ergonomic: the overwhelmingly common case is one
  value per header, and it must not get worse to write.
- Verify: `serialize()` emits two distinct `Set-Cookie` lines for one response;
  every existing header behaviour is byte-identical, proven by both serving
  gates passing unchanged.

### Phase C — reading request cookies

- Parse the `Cookie:` header: multiple pairs, quoted values, stray whitespace,
  and duplicate names.
- Decide where parsed cookies live — a lazily-parsed field on `Req`, or a
  helper the handler calls. `Req` already carries a `ctx` bag and a `params`
  map, so the precedent exists either way.
- A malformed header is a 400, not a silent partial parse. The parser's
  existing discipline around duplicate `Content-Length` is the model.
- Verify: values recovered exactly across the awkward cases above; malformed
  input refused.

### Phase D — writing and signing cookies

- A `Set-Cookie` builder covering the attribute set, with `HttpOnly` and
  `SameSite` defaulted to the safe choice rather than the permissive one — a
  cookie API whose defaults are insecure is a footgun that ships.
- Sign with `hmac_sha256`, verify with `ct_eq`. Decide the encoding (`base64` is
  already available) and the payload framing so a value containing the delimiter
  cannot forge a signature.
- Clear-cookie support, which is its own case: the attributes must match or the
  browser keeps the old one.
- Verify: a one-bit change to value or signature is rejected; a valid cookie
  round-trips; a cleared cookie is actually gone.

### Phase E — prove it and correct the record

- A login/logout flow in a sample exercising two cookies on one response, using
  the signed-cookie path only — no sessions yet, that is iteration 3.
- Correct porch's README crypto row and re-point the ledger rows this iteration
  touched.
- Verify: `just web-app`, `just site`, `oop-accept`, `just linkcheck` all green.

## Acceptance Criteria

- **Given** the random builtin, **when** many values are drawn across separate
  processes, **then** none repeats and none is derivable from the clock; **and**
  when the kernel source is unavailable, the builtin refuses loudly rather than
  returning weak bytes.
- **Given** a handler setting a session cookie and a flash cookie on one
  response, **when** it is serialized, **then** **two** distinct `Set-Cookie`
  headers reach the wire. This is the criterion today's `map<Text, Text>`
  provably cannot satisfy.
- **Given** every pre-existing response shape, **when** both serving gates run,
  **then** output is byte-identical to before phase B — the `Resp` change is
  additive or it is wrong.
- **Given** a `Cookie:` header with several pairs, quoted values and stray
  whitespace, **when** it is parsed, **then** each value is recovered exactly;
  **and** a malformed header yields 400, never a partial parse.
- **Given** a signed cookie altered by one bit in either the value or the
  signature, **when** it is verified, **then** it is rejected in constant time.
- **Given** a signed value that itself contains the payload delimiter, **when**
  it round-trips, **then** it cannot be re-framed to forge a valid signature.
- **Given** cookie defaults, **when** a cookie is created without explicit
  attributes, **then** `HttpOnly` is on and `SameSite` is not `None`.

## Out Of Scope

- **Encrypted cookies.** Fiber's `encryptcookie` needs a symmetric cipher, and
  the runtime has digests only. Signed-and-readable is honest and sufficient for
  a session id; encrypting a payload is a separate ask with a separate primitive
  behind it.
- **Server-side session state** — iteration [3](03-sessions.md). This iteration
  stops at a signed cookie carrying a value the app chose.
- **CSRF** — iteration [4](04-csrf.md), which needs both this and 3.
- **JWT.** Verification is already possible with `hmac_sha256`; issuing needs
  phase A. Either way it is a library slice with a hard stop at HS256 — no
  RS256, no JOSE — and not this iteration.
- **Cookie-based *cache* keys.** Fiber's cache middleware has `KeyCookies`; the
  cache belongs to language
  [iteration 18](../language-runtime-database/18-memory-db-features.md).

## Info

Forks the spec must settle, in order of how much they move:

1. **What replaces `Resp.headers: map<Text, Text>`?** Three candidate shapes: a
   `multi Text` of raw extra header lines beside the existing map; a dedicated
   typed `cookies` field on `Resp` that `serialize()` renders; or a general
   repeated-header list replacing the map. The first is the smallest change, the
   second the most typed, the third the most honest about HTTP — and the third
   touches every builder and both consumers. This fork decides the size of the
   whole iteration, so settle it first.
2. **What shape is the builtin?** A bytes-returning primitive composes with
   everything iterations 19 and 34 added (`Bytes`, `base64_encode`,
   `hmac_sha256`), which argues for exactly one function and no convenience
   wrappers. Decide whether a hex/id helper rides along or whether
   `base64_encode` is enough.
3. **Where do parsed cookies live?** A field on `Req` parsed eagerly costs every
   request that has no cookies; a helper parsed on demand costs nothing but is
   easy to call twice. `Req` is a `typedef` record, so adding a field is cheap —
   the cost is the eager parse, not the shape.
4. **Signed-cookie framing.** Value-then-signature with a delimiter is the
   obvious encoding and the obvious place to get it wrong. Decide the framing so
   that a value containing the delimiter cannot shift the boundary.

Phase A is the only part of this track that is language-track work. It is
sequenced here rather than filed as a language iteration because nothing else
wants it yet, and a primitive with no consumer is how the `Component` interface
became decoration.
