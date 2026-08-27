---
track: porch
iteration: "5"
status: refine
---

# porch 5 — routing and response ergonomics: the parity that is merely missing

> Part of [Story — `porch`, the writeonce web framework](00-story.md).
> Source: [the Fiber parity study](../../plan/exploration/fiber/00-fiber-parity.md) §5.
>
> Independent of iterations 2–4 and of the streaming seam — startable at any
> time, and a reasonable slice to interleave when the risky work needs a break.
> Nothing here is hard; all of it is felt.

## Goals

- **The rest of the method helpers.** `App` has `get`/`post`/`put`/`delete_`.
  A `Route { method: "PATCH" }` literal already works, so this is registration
  ergonomics rather than capability — but writing the literal by hand for
  `PATCH` while `get` exists is the kind of asymmetry that makes a framework
  feel unfinished. Add `patch`, `options`, `head`, and an `all`.
- **Named routes and reverse routing.** Fiber has `Name()` and `GetRouteURL()`.
  porch has neither, so every link in the site is a hand-written string that no
  compiler checks — and the site is exactly the app where a renamed path breaks
  a page silently.
- **A per-route body limit.** `BODY_MAX = 1048576` is one compile-time constant
  for the whole server. An upload route and a JSON route want different numbers,
  and the JSON route wants a much smaller one than the upload route can live
  with.
- **Request ids.** `req.ctx` already exists to carry one; there is no generator
  and no middleware. With iteration 2's builtin available this is a few lines,
  and it is the difference between logs you can correlate and logs you cannot.
- **The response helpers written by hand today.** `Location`, `Vary`,
  `Attachment`/`Download`, and a content-negotiated `format` dispatch on top of
  the existing `accepts()`. Also q-value *ranking*, which the ledger has carried
  as a known 🔶 since the negotiation slice landed.

## Phases

### Phase A — method helpers and route introspection

- The missing registration helpers, including `all`, and decide whether `head`
  auto-registers alongside `get` (Fiber has `DisableHeadAutoRegister`, which
  tells you the default is auto and that people want it off).
- Route introspection — list the table — because it is nearly free once routes
  are already a `multi Route`, and it is what makes a startup banner or a
  route-dump flag possible.
- Verify: each method dispatches; `405` still carries a correct `Allow` built
  from the real table; existing routes unchanged.

### Phase B — named routes and URL building

- A name on `Route`, a lookup, and a builder that fills `:param` captures.
- Decide the failure mode for a missing or extra parameter. A silently wrong URL
  is worse than a trap, and this is a compile-time-checkable shape only once
  language iteration 29's `@derive` exists — so for now it is a runtime check
  and should say so.
- Migrate the site's internal links onto it, which is the proof it is usable.
- Verify: every site link resolves through the builder; a wrong parameter set is
  refused loudly.

### Phase C — per-route body limits and request ids

- Move the limit from a module constant to route-level configuration with the
  current value as the default, so no existing app changes behaviour.
- A request-id middleware writing into `req.ctx`, and settle whether an inbound
  header is trusted (fork 2).
- Thread the id into the logging middleware's output, since a request id nothing
  logs is decoration.
- Verify: an oversized body is refused per-route; the id appears in logs and is
  stable across a request's lifetime.

### Phase D — response helpers and negotiation ranking

- `Location`, `Vary`, `Attachment`/`Download`, and a `format`-style dispatch
  choosing a builder from `accepts()`.
- Rank q-values properly instead of stripping them, retiring the ledger's 🔶.
- Verify: `Vary` accumulates rather than overwrites (which the iteration-2
  repeated-header work makes possible); negotiation picks the highest-q match,
  not the first.

### Phase E — the gate and the ledger

- Both serving gates, the ledger rows, the board entry.
- Verify: `just web-app`, `just site`, `just linkcheck` green.

## Acceptance Criteria

- **Given** a route registered with each new helper, **when** the matching
  method arrives, **then** it dispatches; **and** an unmatched method still
  yields `405` with an `Allow` listing exactly the registered methods.
- **Given** `head` auto-registration, **when** a `HEAD` request hits a `GET`
  route, **then** the response is headers-only with the `Content-Length` a `GET`
  would have sent — the behaviour `serialize()` already implements, now
  reachable by registration.
- **Given** a named route with `:param` captures, **when** a URL is built with
  the right parameters, **then** it matches that route's pattern exactly;
  **and** a wrong or missing parameter is refused rather than producing a
  plausible-looking wrong URL.
- **Given** two routes with different body limits, **when** a body exceeding the
  smaller arrives at each, **then** it is refused at the small route and
  accepted at the large one.
- **Given** no per-route limit, **when** a request arrives, **then** the
  previous global limit applies unchanged.
- **Given** a request-id middleware, **when** a request is handled, **then** the
  same id appears in every log line for that request and in the response header.
- **Given** an `Accept` header with q-values out of order, **when** negotiation
  runs, **then** the highest-q acceptable type wins — not the first listed.
- **Given** two `Vary` contributions from different middleware, **when** the
  response leaves, **then** both appear.

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
- **Streaming responses, `SendFile`, byte ranges** — iterations
  [6](06-streaming-core.md) and [8](08-static-and-lifecycle.md).
- **Typed binding of params into a class** — language iteration 29 again.

## Info

Forks the spec must settle:

1. **Does `head` auto-register?** Fiber's default is yes with an opt-out. Auto is
   friendlier; explicit is more predictable and never surprises someone
   debugging why a route they did not register is answering.
2. **Is an inbound request-id header trusted?** Behind the mandated proxy,
   trusting it is what makes tracing work across hops. On an open port it lets a
   client forge correlation ids and poison logs. `client_ip` and `net.peer`
   already exist for exactly this trust decision — reuse that conclusion.
3. **Where does a route's body limit live?** A field on `Route` is the obvious
   home but widens a record that the conformance corpus pins the ownership shape
   of. Check that fixture before choosing.
