# porch vs fiber — the scope gap, and who prefers which

> Companion to [00-fiber-parity.md](00-fiber-parity.md). Written 2026-09-06,
> after the porch track (stories 1–8) was brainstormed to `ready`. It reads
> fiber v3 (`.dev/reference/fiber`, commit `3ca9a9d`) against porch's **approved
> scope** — what ships today, plus what stories 1–8 add — and asks what remains
> missing, what fiber does better, and whether a developer would choose porch.
> An exploration doc: no status banner by convention.

## The 32 middleware, mapped against approved scope

Nine already had counterparts before the track. The track adds eight more. The
rest are excluded, and the exclusions divide into *principled* (a different
philosophy) and *blocked* (a primitive porch does not have yet).

- **Shipped before the track (9):** cors, basicauth, keyauth (bearer), helmet
  (SecurityHeaders), hostauthorization (HostAllow), etag, static, recover
  (trap=500), logger.
- **Added by the track (8):** compress + sse (story 7), csrf (4), session (3),
  favicon + healthcheck + rewrite + skip + redirect (5/8), requestid (5),
  limiter (story 1, already done).
- **Blocked on the lang-41 runtime hang (1):** idempotency — built, reviewed,
  gate-passed, reverted; it is porch story 9, held, not a design gap.
- **Excluded, owner named (rest):** cache/paginate (language 18), expvar/pprof
  (runtime-v2 7 observability), proxy (language 38, needs `net.connect`),
  encryptcookie (runtime-v2 8 symmetric cipher), adaptor (no net/http
  ecosystem), earlydata (TLS at the
  proxy), timeout (per-handler, needs cancellation — language 31), envvar +
  responsetime (niche helpers, unscoped).

## What porch lacks even after the whole track lands

Ranked by how much a real app feels it.

1. **Typed request binding.** Fiber's `Bind().Body/Query/Cookie(&struct)` is the
   single biggest ergonomic gap. porch cannot do it without reflection
   (principle 13 forbids it) until `@derive` lands (language iteration 29). Today
   a porch handler pulls fields out of maps by hand. This is the one place fiber
   is *dramatically* more pleasant, and it is felt on every form and every JSON
   endpoint.
2. **Outbound HTTP: client and reverse proxy.** Fiber ships an HTTP client and a
   `proxy` middleware. porch has no `net.connect`, so neither exists — an app
   that calls another service or fronts one cannot be written in porch at all.
   Owner: language iteration 38.
3. **Encrypted cookies.** Fiber's `encryptcookie` is AES-GCM; porch has digests
   but no symmetric cipher, so it offers signed-and-readable only. Fine for a
   session id, not for a payload an app wants to hide from the client.
4. **Per-handler timeouts.** Fiber wraps a handler in a deadline. porch has
   per-*call* net deadlines (iteration 35) but no per-handler timeout, because
   cancelling a running handler needs the actor-lifecycle cancellation that
   language iteration 31 owns.
5. **Runtime templating.** Fiber renders views at request time against a dozen
   template engines. porch rejects this by doctrine — markup is a compile-time
   literal (`writeonce-view`). A principled choice, but it rules out the
   user-editable-template use case entirely.
6. **The ecosystem.** `adaptor` plugs fiber into all of Go's `net/http` universe;
   its session/cache/limiter middleware take pluggable storage drivers (Redis,
   Postgres, dozens more). porch has none of that surface and, by single-binary
   doctrine, does not want most of it — but it means no drop-in Redis, no
   community middleware, no third-party integrations.
7. **Minor, unscoped helpers:** pagination, response-time header, env-var
   exposure. Each is a few lines an app can write; none is in the track.

## What fiber is better at, even where porch has a counterpart

- **Maturity and ecosystem.** Battle-tested, huge community, pluggable storage
  behind every stateful middleware, and the whole Go module world one adaptor
  away. This is fiber's decisive, structural advantage and porch will not close
  it.
- **Ergonomics.** Binding, generic helpers, reflection-driven convenience — less
  hand-written glue per endpoint.
- **Raw performance ceiling.** fasthttp is extreme; a bytecode VM with
  single-threaded-per-connection serving will not match its throughput on a
  synthetic benchmark. (porch trades this for a different model, below.)
- **Configurability.** Case-sensitivity, strict-slash routing, prefork, storage
  backends — many knobs. porch is deliberately opinionated with few.
- **All-in-one networking.** Client, proxy, TLS termination, HTTP/2 in one
  process. porch delegates TLS/HTTP2 to a front proxy by doctrine.

## Where porch is actually better

The honest counter-case, because "prefer" is not decided on fiber's axes alone.

- **Durable by default, in one binary.** Sessions, rate-limiting and idempotency
  ride the WAL and survive a restart with no Redis, no external store. Fiber's
  defaults are in-memory — a restart logs everyone out and resets every counter;
  durability means operating a second system. porch's whole stateful surface is
  durable with zero extra infrastructure.
- **One static binary, zero dependencies.** Hand-rolled crypto, no CGO, no
  driver matrix. Deploy a file.
- **Safety by construction.** No reflection, ownership/borrow checking, and a
  house style of secure-by-default (HttpOnly/SameSite defaults, session-id
  rotation on login, refusal classes distinguishable in logs but opaque in the
  body) and refuse-the-unconstructible (an incoherent heartbeat/idle pair, a
  streaming route under header-mutating middleware — both rejected at
  construction, never silently half-applied).
- **A concurrency model that fits the hard parts.** Actors/fibers/shards make
  SSE fan-out and WebSocket rooms natural rather than bolted on, and idle
  connections park for almost nothing.
- **Honesty about limits.** Every gap above is named with an owner; nothing
  degrades silently.

## Would developers prefer porch over fiber?

It depends on who is asking, and the answer is not "porch wins on features."

- **A Go developer choosing a framework today: no.** Not on parity terms. Fiber
  wins on ecosystem, binding ergonomics, maturity, performance ceiling, storage
  flexibility and sheer breadth. porch cannot out-fiber fiber at fiber's own
  game, and trying would be the wrong goal.
- **A developer already choosing writeonce: yes, and gladly.** porch is a
  coherent, durable-by-default, single-binary framework with strong security
  defaults, written *in* the language it serves. Inside the ecosystem there is no
  contest — it is the framework, and a good one.
- **A developer choosing on values, either language aside: sometimes.** Someone
  who weights one-binary durability with no Redis, memory safety without GC
  pauses, opinionated secure defaults, and named-not-hidden limits above
  ecosystem breadth may genuinely prefer porch. That is a real but narrow
  constituency.

**The honest positioning.** porch is not a fiber-killer and the approved scope
does not try to be one. It is deliberately *complete enough* to prove the
language can carry a serious web framework, with a distinct thesis —
durable-by-default, single-binary, safe-by-construction — that fiber does not
compete on. Its remaining gaps are almost all one upstream primitive away
(`net.connect` → client/proxy, `@derive` → binding, a cipher → encrypted
cookies, cancellation → handler timeouts), which means the ceiling is set by the
language track, not by porch's design. Developers will prefer porch when they
have already bought the thesis; they will prefer fiber when they are shopping on
breadth.
