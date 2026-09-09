# Story — `porch`, the writeonce web framework

The second track. Where
[`language-runtime-database/`](../language-runtime-database/00-story.md) grows
the *language*, this track grows the one library written **in** it:
[`porch`](../../examples/porch/README.md), consumed by every serving sample
through `wo.toml [deps]`.

Numbering restarts at 1 and is local to this track. Frontmatter carries
`track: porch` so a query over `docs/stories/` can tell a porch iteration 3 from
a language iteration 3. Status rules are the repo's, unchanged: `status:` in
frontmatter is the only place state lives, no directory encodes it.

## Why a separate track

Three reasons, all practical:

1. **Different substrate, different gates.** porch is `.wo` source. Its
   iterations are proven by `just web-app` and `just site`, never by the
   conformance corpus or `oop-accept`. Mixing them into the language track's
   sequence made both harder to read.
2. **Different cadence.** A porch slice is days; a language slice that touches
   `wob.h` and the VM is longer and riskier. Interleaving them in one numbering
   forced false ordering decisions.
3. **The framework is now the product surface.** `writeonce.de` is served by
   porch. Its gaps are what a visitor hits first, so they deserve a roadmap that
   is not buried behind runtime work.

The language track stays upstream: when a porch iteration needs a new builtin,
that half is called out explicitly and the language track owns it.

## Where the sequence came from

The [Fiber v3.5.0 parity study](../../plan/exploration/fiber/00-fiber-parity.md)
— gofiber/fiber read end to end against porch's actual `.wo` source: its routing
surface, `Req`/`Res` API, binder, lifecycle hooks and the `Config` of all 32 of
its `middleware/` packages. Nine of those 32 already have a working porch
counterpart, so this is a breadth roadmap, not a rescue.

That study replaced language-track
[iteration 39](../language-runtime-database/39-web-framework-parity.md), which is
now a pointer here.

## The sequence

Ordered by dependency, not by importance — and the first slice is deliberately
the *cheapest*, so the store pattern and the gate shape are proven before the
risky work starts.

| # | Iteration | Delivers | Needs |
| --- | --- | --- | --- |
| 1 | [Store-backed middleware](01-store-backed-middleware.md) | rate limiting + idempotency over a `@table` store, serialized through a sharded actor pool | 🔄 in progress; needs no new primitive (`call`/`send`/`monitor`/`time.after` all landed) |
| 2 | [Randomness and cookies](02-randomness-and-cookies.md) | a `random_bytes` runtime builtin, repeated response headers, `Cookie:` parsing, signed cookies | a language-track builtin (phase A) |
| 3 | [Sessions](03-sessions.md) | server-side sessions, idle + absolute timeout, revocation | 2 |
| 4 | [CSRF](04-csrf.md) | token mint/verify, trusted origins, single-use tokens | 2, 3 |
| 5 | [Routing and response ergonomics](05-routing-response-ergonomics.md) | the remaining method helpers, named routes, per-route body limit, request ids, the missing response helpers | nothing — parallel to 2–4 |
| 6 | [Streaming core](06-streaming-core.md) | incremental response writes and chunked framing — the seam three iterations wait on | nothing new, but it changes `Resp` |
| 7 | [SSE and compression](07-sse-and-compression.md) | server-sent events, gzip/deflate | 6 |
| 8 | [Static files and lifecycle](08-static-and-lifecycle.md) | byte ranges, cache headers, directory listing, lifecycle hooks, the small middleware everyone ships | 6 |

```
1 ─ independent, start here
5 ─ independent, any time
2 ──▶ 3 ──▶ 4
6 ──▶ 7
  └──▶ 8
```

## What this track does NOT own

| Not porch's | Owner |
| --- | --- |
| typed binding of query/params/form into a class | language: [`@derive`](../language-runtime-database/29-compile-time-metaprogramming.md) — reflection is forbidden by principle 13 |
| TTL cache, `transaction { }`, durable job queue | language: [iteration 18](../language-runtime-database/18-memory-db-features.md) |
| a `proxy` middleware | language: [iteration 38](../language-runtime-database/38-content-platform-capabilities.md) — needs `net.connect`, ✅ landed 2026-09-07 (id 110; plus `net.connect_tls` for an HTTPS upstream, runtime-v2 9). Buildable now |
| metrics, profiling, per-change CI, fuzzing | [runtime-v2 7](../runtime-v2/07-observability.md) — observability (was language iteration 30; metrics/profiling/trace-on-trap; CI + fuzz are tooling, split out) |
| TLS | ✅ [runtime-v2 9](../runtime-v2/09-in-process-tls.md) — in-process TLS 1.3 both directions (2026-09-09); porch can terminate inbound TLS with `net.accept_tls`, no front proxy required. The proxy-termination doctrine is retired |
| HTTP/2 | nobody yet — a separate protocol slice; TLS is its prerequisite, now met |
| a runtime template engine | nobody — rejected; markup is a compile-time literal (`writeonce-view`) |
| a radix-tree router | nobody yet — waiting on a *measurement*, not a decision |

## Review protocol

Same as the language track: the developer reads one iteration, approves or
amends; the next starts only after approval. Each iteration is an unsplittable
value slice with phases, per-phase tasks, Given/When/Then acceptance criteria,
and an out-of-scope list. Every phase ends with both serving gates green —
`just web-app` and `just site` — because porch has two consumers and a change
that only satisfies one is not done.
