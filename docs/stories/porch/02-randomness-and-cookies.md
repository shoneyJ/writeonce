---
track: porch
iteration: "2"
status: pending
readiness: ready
---

# porch 2 — randomness and cookies: the foundation three iterations stand on

> Part of [Story — `porch`, the writeonce web framework](00-story.md).
> Source: [the Fiber parity study](../../plan/exploration/fiber/00-fiber-parity.md) §0–§1,
> re-checked 2026-09-06 against `.dev/reference/fiber` (v3, commit `3ca9a9d`):
> its `crypto/rand` consumers (`utils.SecureToken`, `utils.UUIDv4`,
> `encryptcookie.GenerateKey`), the `fiber.Cookie` struct in `res.go`, and the
> `Cookie`/`Cookies`/`ClearCookie` methods.
>
> **The study's sharpest finding lives here.** porch's own README claimed signed
> cookies, CSRF and session integrity were "UNBLOCKED — the primitives exist
> since iteration 34". For CSRF and sessions that is **wrong**: SHA-256 and HMAC
> let a program *authenticate* a token, they cannot *mint* one, and writeonce has
> no source of randomness anywhere. An HMAC over a guessable session id is a
> signed guess. Phase A closes that before anything depends on it.

## What the reference confirmed

The fiber read settled the scope precisely: **exactly one language enhancement
is needed — the CSPRNG — and nothing else.** Every other thing fiber does with
randomness and cookies maps onto primitives writeonce already has.

- **Randomness is the one gap.** fiber's `SecureToken` is
  `base64.RawURLEncoding` over 32 `crypto/rand` bytes; `UUIDv4` is the same
  entropy behind a format; `encryptcookie.GenerateKey` is a raw `rand.Read`.
  All three **panic** if the source fails. writeonce exposes no RNG to `.wo`
  yet — the runtime does have a `getrandom(2)` source internally (runtime-v2 9's
  TLS uses it for ephemeral keys), so phase A is surfacing that as a
  `random_bytes` builtin, not inventing entropy. It is the whole of the language
  work.
- **Repeated `Set-Cookie` is not language work.** fiber gets multiple lines from
  fasthttp appending them; porch expresses the same with a `multi SetCookie`
  field, and `multi <Class>` is an existing language feature.
- **Cookie attributes are not language work.** fiber's `Cookie` struct is plain
  scalars (name/value/path/domain/same-site as text, max-age as int,
  secure/http-only/partitioned as bool, plus an `Expires` date). A `SetCookie`
  record covers all of it, and choosing `Max-Age` over `Expires` avoids the only
  attribute that would need a date formatter.
- **Parsing and signing are not language work.** Reading the `Cookie:` header
  uses the existing `split`/`trim`/`substr`; signing uses the existing
  `hmac_sha256`/`sha256`/`base64_encode`/`base64_decode`/`ct_eq`/`bytes_of_text`.
- **One deliberate divergence.** fiber has no HMAC-signed-readable cookie — it
  ships plaintext, or AES-GCM via `encryptcookie`, or server-side storage. porch
  chooses signed-readable because writeonce has digests but no cipher. That is
  the honest choice for this runtime, not a gap; encryption stays out of scope.

## Goals

- **A CSPRNG builtin in the runtime.** No `getrandom`, no `/dev/urandom` read,
  no CSPRNG builtin exists today. This is the one phase of this track that is
  **language-track work** (C, a new builtin id, a corpus fixture); it is here
  because porch is what needs it and splitting it across two tracks would hide
  the dependency.
- **Repeated response headers, which `Resp` structurally cannot express.**
  `Resp.headers` is a `map<Text, Text>`. A login response setting a session
  cookie *and* a flash cookie needs two `Set-Cookie` lines and the map can hold
  one. The settled answer adds a typed `cookies` field beside the map rather
  than disturbing it — the cookie formatting is the easy half.
- **Cookies in both directions.** Parse a `Cookie:` request header into
  something typed; build a `Set-Cookie` with the attributes that matter for
  security — `HttpOnly`, `Secure`, `SameSite`, `Max-Age`, `Path`, `Domain`.
- **Signed cookies.** HMAC-SHA256 over the value with constant-time comparison
  on the way back (`hmac_sha256` and `ct_eq` both already exist). This is the
  half that genuinely *was* unblocked by iteration 34, and it is what makes a
  cookie tamper-evident without a server-side lookup.
- **Correct the ledger row that started this.** The README's crypto line, and
  anything else claiming the sessions/CSRF path was already open.

## Decisions locked (brainstorm 2026-09-06)

1. **`Resp` gains `cookies: multi SetCookie`; the `headers` map is untouched.**
   Of the three candidates — a raw `multi Text` of extra lines, a typed cookie
   field, or replacing the map with a repeated-header list — the typed field is
   the smallest diff and the most typed at once. `serialize()`'s existing
   `for k, v in resp.headers` loop does not move, so the byte-identical gate is
   trivial; a second loop renders the cookies. `SecurityHeaders` and `Cors`
   write into the map exactly as before. The one thing this shape cannot express
   — a repeated *non-cookie* header — has no consumer, so it is YAGNI.
2. **The RNG is a bare-name `random_bytes(n) -> Bytes`, one primitive, no
   wrappers.** It joins the crypto family it composes with — `sha256`,
   `hmac_sha256`, `base64_encode` — which live in the compiler's bare-name
   builtin-function table (`compiler/src/emit.ml`'s `b_*` ids plus the
   `types.ml` function list), **not** in `wob.h`'s module-member enum. This
   corrects the earlier note that pointed phase A at `wob.h` id 110: that is the
   wrong registry. A session id is then `base64_encode(random_bytes(32))`,
   mirroring fiber's `SecureToken`; no hex or UUID helper rides along because
   base64 already turns bytes into a text id.
3. **A malformed `Cookie:` header is a 400, checked structurally in
   `parse_request`; values are extracted on demand.** The developer chose to
   keep the 400 policy after it was questioned (RFC 6265 would skip bad pairs;
   the framing-critical analogy to duplicate `Content-Length` is weaker for a
   non-framing header). The resolution is a hybrid: `parse_request` does a
   cheap structural syntax check — nil-check first, so an absent `Cookie` header
   costs nothing — and refuses before routing; a `cookie(req, name)` helper
   parses and extracts the requested value on demand, so requests that read no
   cookie pay no parse into a map.
4. **Signed-cookie framing is `base64(value)` + `.` + `base64(mac)`.** Encoding
   the value first means the delimiter cannot appear inside either half — the
   base64 alphabet has no `.` — so a value containing the delimiter cannot shift
   the boundary. Verify splits on `.` into exactly two parts and compares the
   two base64 mac strings with `ct_eq`; they are always the same length (a
   32-byte HMAC is always 44 base64 chars), so only the value is decoded. Every
   character used — base64's `+ / =`, and `.` — is RFC 6265 cookie-safe, so the
   signed string needs no further escaping.
5. **The signing key is app-supplied policy, passed to free functions.** No
   `SignedCookies` middleware in this iteration — that is the shape iteration 3
   introduces for sessions. `sign` and `verify` take the key as an argument, the
   way `ct_eq` and `bearer_token` are helpers the app calls with its own secret.
   The framework neither invents nor persists the key.

## Phases

### Phase A — the runtime primitive (language-track work)

- Add `random_bytes` to the compiler's bare-name builtin table
  (`compiler/src/emit.ml` and the `types.ml` function list), returning `Bytes`
  and taking a byte count. Take the next free id there — `84` is a hole between
  `b_text_of_bytes` and `b_sha1`, with `90` and up also free; **confirm the
  chosen id is not a deliberate reservation before writing it**, since the id
  space is shared and drifts.
- Source it from the kernel (`getrandom(2)`, falling back to a `/dev/urandom`
  read only as the same kernel source). Decide the failure mode explicitly: if
  the source is unavailable the builtin **refuses**, loudly — it traps, it does
  not return short or weak bytes. A CSPRNG that quietly degrades is worse than
  none, because every layer above it assumes it worked. This matches fiber,
  which panics rather than continue.
- Corpus fixtures: the builtin's arity and type contract, and the refusal path.
  Statistical quality is not a corpus concern — the kernel's guarantee is the
  guarantee.
- Document it in the builtin-surface contract and the error catalog if it adds a
  diagnostic, in the same change. Both of those went stale once before by not
  doing this.
- Verify: `oop-e2e` green; the value differs across separate processes and is
  not derivable from the clock.

### Phase B — repeated response headers

- Add `cookies: multi SetCookie` to `Resp` and define the `SetCookie` record:
  `name` and `value`, plus `HttpOnly`, `Secure`, `SameSite`, `Max-Age`, `Path`,
  `Domain`. Its defaults are the safe ones — `HttpOnly` on, `SameSite` not
  `None` — because a cookie API whose defaults are insecure is a footgun that
  ships.
- Extend `serialize()` in `internal/serve.wo` with a second loop that renders
  each `SetCookie` to a `Set-Cookie:` line after the existing header loop, which
  does not change. Every response builder
  (`ok_text`/`ok_json`/`ok_html`/`created_json`/`not_found`/…) gains the new
  field as an empty container and is otherwise untouched.
- Verify: `serialize()` emits two distinct `Set-Cookie` lines for one response;
  every existing header behaviour is byte-identical, proven by both serving
  gates passing unchanged.

### Phase C — reading request cookies

- In `parse_request`, add a structural check of the `Cookie:` header — present
  and syntactically well-formed, or a 400 before any route runs. Absent is not
  malformed: the nil case is the common one and must cost nothing.
- Add a `cookie(req, name)` helper that parses the header on demand and returns
  the requested value, handling multiple pairs, quoted values, stray
  whitespace, and duplicate names. No new `Req` field: extraction is on demand,
  matching how `http/auth.wo` already pulls `bearer_token` and
  `basic_credentials`.
- Verify: values recovered exactly across the awkward cases above; a malformed
  header is refused with 400, never a silent partial parse.

### Phase D — writing and signing cookies

- A `Set-Cookie` builder covering the attribute set with the safe defaults from
  phase B, and a clear-cookie builder — its own case, because the attributes
  (`Path`, `Domain`) must match the original or the browser keeps the old
  cookie; expiry is expressed as `Max-Age` zero.
- `sign(key, value)` and `verify(key, signed)` free functions using the locked
  framing: base64 the value, HMAC it, base64 the mac, join with `.`; verify
  splits, recomputes, and compares the base64 macs with `ct_eq`. The key is an
  argument, app-supplied.
- Verify: a one-bit change to value or signature is rejected in constant time; a
  valid cookie round-trips; a value that itself contains the delimiter cannot be
  re-framed to forge a signature; a cleared cookie is actually gone.

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
  **and** a malformed header yields 400, never a partial parse; **and** an
  absent header costs no parse.
- **Given** a signed cookie altered by one bit in either the value or the
  signature, **when** it is verified, **then** it is rejected in constant time.
- **Given** a signed value that itself contains the payload delimiter, **when**
  it round-trips, **then** it cannot be re-framed to forge a valid signature.
- **Given** cookie defaults, **when** a cookie is created without explicit
  attributes, **then** `HttpOnly` is on and `SameSite` is not `None`.

## Out Of Scope

- **Encrypted cookies.** Fiber's `encryptcookie` needs a symmetric cipher
  (AES-GCM), and the runtime has digests only. Signed-and-readable is honest and
  sufficient for a session id; encrypting a payload is a separate ask with a
  separate primitive behind it.
- **UUID-formatted ids.** fiber's `UUIDv4` is a format over the same entropy
  `random_bytes` provides; a base64'd 32-byte token is stronger and needs no new
  builtin. A UUID *format* helper, if ever wanted, is pure `.wo`.
- **Cookie `Expires` as an absolute date.** `Max-Age` in seconds covers expiry
  and clear-cookie without a date formatter; an `Expires` attribute would build
  on `time.local` in pure `.wo` and is not needed here.
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

Phase A is the only part of this track that is language-track work. It is
sequenced here rather than filed as a language iteration because nothing else
wants it yet, and a primitive with no consumer is how the `Component` interface
became decoration.

The `Resp` change is deliberately additive: because `cookies` is a new field
beside the `headers` map rather than a replacement for it, the whole of the
existing response machinery — every builder, `serialize()`'s header loop,
`set_header`, and the `SecurityHeaders`/`Cors` middleware — is unchanged, which
is what makes the byte-identical gate cheap to meet.
