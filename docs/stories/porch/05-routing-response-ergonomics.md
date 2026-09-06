---
track: porch
iteration: "5"
status: pending
readiness: ready
---

# porch 5 — routing and response ergonomics: the parity that is merely missing

> Part of [Story — `porch`, the writeonce web framework](00-story.md).
> Source: [the Fiber parity study](../../plan/exploration/fiber/00-fiber-parity.md) §5,
> re-checked 2026-09-06 against `.dev/reference/fiber` (v3, `3ca9a9d`):
> `DisableHeadAutoRegister`, `Name()`/`GetRouteURL()`, `BodyLimit`, and the
> `requestid` middleware's trust model.
>
> Independent of iterations 2–4 and of the streaming seam — and, as the
> brainstorm confirmed, with **no upstream dependency at all** (not even
> iteration 2): startable at any time, a reasonable slice to interleave when the
> risky work needs a break. Nothing here is hard; all of it is felt.

## Goals

- **The rest of the method helpers.** `App` has `get`/`post`/`put`/`delete_`.
  A `Route { method: "PATCH" }` literal already works, so this is registration
  ergonomics rather than capability. Add `patch`, `options`, `head`, and an
  `all`.
- **Named routes and reverse routing.** Fiber has `Name()` and `GetRouteURL()`.
  porch has neither, so every link in the site is a hand-written string that no
  compiler checks — and the site is exactly the app where a renamed path breaks
  a page silently.
- **A per-route body limit.** `BODY_MAX = 1048576` is one compile-time constant
  for the whole server. An upload route and a JSON route want different numbers,
  and the JSON route wants a much smaller one than the upload route can live
  with.
- **Request ids.** `req.ctx` already exists to carry one; there is no generator
  and no middleware. It is the difference between logs you can correlate and
  logs you cannot.
- **The response helpers written by hand today.** `Location`, `Vary`,
  `Attachment`/`Download`, and a content-negotiated `format` dispatch on top of
  the existing `accepts()`. Also q-value *ranking*, which the ledger has carried
  as a known 🔶 since the negotiation slice landed.

## Decisions locked (brainstorm 2026-09-06)

1. **`head` auto-registers alongside `get`, with an opt-out.** Matches fiber
   (`DisableHeadAutoRegister` proves the default is auto and people want the
   knob). `serialize()` already produces headers-only with a GET's
   `Content-Length`, so this is pure registration — the behaviour exists, this
   makes it reachable without a hand-written literal. `patch`/`options`/`all`
   are plain additions; `all` registers one handler for every method.
2. **Request ids mirror the limiter's trust conclusion, and use a non-crypto
   source.** Default (`trust_inbound` off): always generate a fresh id and
   ignore any inbound `X-Request-Id`, because an open port can forge correlation
   ids and poison logs — exactly what the limiter's `trust_proxy` guards.
   Opt-in (behind the mandated proxy): honor an inbound `X-Request-Id` for
   cross-hop tracing, generate when absent. The id is minted from a non-crypto
   unique source (`time.ticks` plus a per-process counter), **not** iteration
   2's `random_bytes` — a request id is not a secret, and this keeps the whole
   iteration free of upstream dependencies. It is written to `req.ctx` and
   echoed in the response header, and the logging middleware includes it.
3. **The per-route body limit is a second check after routing; the global
   `BODY_MAX` stays as the pre-routing ceiling.** The body is read in
   `parse_request` before the route is known (`parse.wo`), so something must cap
   bytes first — the global remains that DoS ceiling. A `body_limit: Int` field
   on `Route` (default the global, and never above it) is checked in dispatch
   against `len(req.body)`; over-limit is a 413. This corrects the story's
   implication that the per-route limit replaces the global — it cannot, because
   routing happens after the read.
4. **Adding fields to `Route` is free of the conformance corpus.** Fork 3 warned
   the corpus pins `Route`'s ownership shape; it does not — `container-owned-move`
   defines its own local `Route`/`App` to pin move-on-push and is decoupled from
   porch's. The new fields (`name: Text`, `body_limit: Int`) are scalar/Text,
   copy-stored, with no ownership complication for the owned `h: Handler`.
5. **`Vary` accumulates by comma-joining in the existing header map — no
   iteration 2 needed.** One `Vary: A, B` header keeps the iteration truly
   independent; the story's tie to iteration 2's repeated-header work is not
   required for this.

## Phases

### Phase A — method helpers and route introspection

- Add `patch`, `options`, `head`, `all` to `App`, each pushing a `Route` literal
  as `get`/`post` already do; `all` registers the handler for every method.
  `head` auto-registration (decision 1) is wired here with its opt-out.
- Route introspection — list the table — nearly free once routes are a
  `multi Route`, and what makes a startup banner or a route-dump flag possible.
- Verify: each method dispatches; `405` still carries a correct `Allow` built
  from the real table (the `app.wo` dispatch loop already assembles it);
  existing routes unchanged.

### Phase B — named routes and URL building

- A `name` field on `Route`, a lookup by name, and a builder that fills `:param`
  captures against the route's pattern.
- The failure mode for a missing or extra parameter is a loud runtime refusal,
  not a plausible-looking wrong URL. This is compile-time-checkable only once
  language iteration 29's `@derive` exists — so it is a runtime check now and
  says so.
- Migrate the site's internal links onto the builder, which is the proof it is
  usable.
- Verify: every site link resolves through the builder; a wrong parameter set is
  refused loudly.

### Phase C — per-route body limits and request ids

- Add `body_limit: Int` to `Route`, defaulting to the global `BODY_MAX`, checked
  in dispatch after the route matches (decision 3); an oversized body is a 413
  at that route while the global ceiling still bounds the pre-routing read.
- A request-id middleware (decision 2): non-crypto id, trust model mirroring the
  limiter, written to `req.ctx` and the response header.
- Thread the id into the logging middleware's output, since a request id nothing
  logs is decoration.
- Verify: an oversized body is refused per-route; a request under the global but
  over a route's tighter limit is refused only at that route; the id appears in
  logs and the response header and is stable across a request's lifetime.

### Phase D — response helpers and negotiation ranking

- `Location`, `Vary` (comma-join accumulation, decision 5), `Attachment`/
  `Download`, and a `format`-style dispatch choosing a builder from `accepts()`.
- Rank q-values properly instead of stripping them, retiring the ledger's 🔶:
  parse `Accept` into type/q pairs, and pick the highest-q acceptable match.
- Verify: `Vary` accumulates rather than overwrites; negotiation picks the
  highest-q match, not the first listed.

### Phase E — the gate and the ledger

- Both serving gates, the ledger rows, the board entry, standup questions
  answered including the `.dev/reference` projects used.
- Verify: `just web-app`, `just site`, `just linkcheck` green.

## Acceptance Criteria

- **Given** a route registered with each new helper, **when** the matching
  method arrives, **then** it dispatches; **and** an unmatched method still
  yields `405` with an `Allow` listing exactly the registered methods.
- **Given** `head` auto-registration, **when** a `HEAD` request hits a `GET`
  route, **then** the response is headers-only with the `Content-Length` a `GET`
  would have sent; **and** with auto-registration disabled the `GET` route
  answers `GET` only.
- **Given** a named route with `:param` captures, **when** a URL is built with
  the right parameters, **then** it matches that route's pattern exactly;
  **and** a wrong or missing parameter is refused rather than producing a
  plausible-looking wrong URL.
- **Given** two routes with different body limits, **when** a body exceeding the
  smaller arrives at each, **then** it is refused (413) at the small route and
  accepted at the large one.
- **Given** no per-route limit, **when** a request arrives, **then** the
  previous global limit applies unchanged.
- **Given** a request-id middleware with inbound trust off, **when** a request
  arrives carrying `X-Request-Id`, **then** a fresh id is generated and the
  inbound one ignored; **and** with trust on, the inbound id is honored.
- **Given** a request-id middleware, **when** a request is handled, **then** the
  same id appears in every log line for that request and in the response header.
- **Given** an `Accept` header with q-values out of order, **when** negotiation
  runs, **then** the highest-q acceptable type wins — not the first listed.
- **Given** two `Vary` contributions from different middleware, **when** the
  response leaves, **then** both appear in one comma-joined `Vary`.

## Out Of Scope

- **A radix-tree router.** Path matching is a linear scan and the ledger marks
  it 🔶 pending a *measurement*. Language iteration 22 built the benchmark
  harness but pointed it at the database. Until someone benches the router, this
  is an optimisation without evidence.
- **Case-insensitive or strict-slash routing.** Fiber exposes both as config.
  porch is case-sensitive and lenient; changing that is a behaviour change for
  existing apps and wants its own decision.
- **Compile-time-checked URL building.** The typed version needs language
  iteration [29](../language-runtime-database/29-compile-time-metaprogramming.md).
  Runtime-checked now, upgraded later.
- **Cryptographically random request ids.** Not needed — a request id is not a
  secret (decision 2). If iteration 2 is landed, `random_bytes` is an acceptable
  alternative source, but this iteration must not depend on it.
- **Streaming responses, `SendFile`, byte ranges** — iterations
  [6](06-streaming-core.md) and [8](08-static-and-lifecycle.md).
- **Typed binding of params into a class** — language iteration 29 again.

## Info

Forks are settled above. This iteration is entirely pure `.wo` and, unusually
for the track, has **no upstream dependency** — not the streaming seam, not
sessions, not even iteration 2's builtin (decision 2 keeps request ids off the
CSPRNG). It is the safest slice to pick up at any time, which is exactly why the
track lists it as independent.
