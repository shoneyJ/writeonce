---
track: runtime-v2
iteration: "7"
was_language_iteration: "30"
status: pending
readiness: refine
---

# runtime-v2 7 — observability: metrics, profiling, and traces on trap

> Moved 2026-09-06 from the language track (was language iteration 30, the
> number a dozen docs still point at) into runtime-v2, whose builtin-sized-seam
> shape it fits. It stretches the track's original processes/terminals/signals
> charter — observability is runtime instrumentation of the VM, GC and shards —
> but the track already grew past its first five seams. **`readiness: refine`** —
> the gap, its consumers and its forks are named here, nothing is brainstormed to
> `ready` yet.

## Why this exists

The runtime has no observability surface. A healthcheck answers one bit
(up/not-up); nothing exposes counters, gauges, latencies, memory, or a
profile. Every attempt to reason about the system's behaviour at runtime hits
the same wall, which is why the number is referenced from five directions at
once:

- **porch** excludes `expvar`, `pprof` and metrics endpoints by pointing here
  ([story 8](../porch/08-static-and-lifecycle.md),
  [39](../language-runtime-database/39-web-framework-parity.md)) — a healthcheck is one bit, not
  observability.
- **databasev2** cannot observe table size without it
  ([bounded-tables 5](../databasev2/05-bounded-tables-eviction.md)) and leans on
  it for per-change benchmark CI
  ([databasev2 story](../databasev2/00-story.md)); the RAM-ceiling study read
  RSS from `/proc` by hand ([databasev2 1](../databasev2/01-ram-ceiling-measurement.md))
  precisely because this does not exist.
- The **rate limiter**'s ephemeral-row expiry is lazy "because porch has no
  timer and iteration 30 owns" the sweep story (that "iteration 30" is now this
  one — [porch 1](../porch/01-store-backed-middleware.md)).

Fiber ships `expvar` and `pprof` as middleware; the equivalents here are runtime
work, because the numbers they expose (allocations, fiber counts, shard load,
GC pauses) live in the C runtime, not in `.wo`.

## What it should deliver (scope to be refined)

- **Runtime counters and gauges** — allocations, arena high-water, live fiber and
  actor counts, per-shard load, GC pause totals, request counters — exposed
  through one endpoint the app can mount.
- **A profiling story** — CPU and heap sampling, the `pprof` equivalent, so a hot
  path can be found rather than guessed at.
- **A stack trace on trap** — today a trap is a 500 and a line; a trace at the
  trap site is the cheapest debugging win and may be separable from the metrics
  work.

## Forks the brainstorm must settle

1. **Counters only, or profiling too?** Counters and gauges are a bounded, mostly
   `.wo`-plus-a-few-builtins surface; CPU/heap profiling needs sampling
   machinery in the runtime and is a much larger commitment. Splitting profiling
   into its own iteration is a legitimate outcome.
2. **Exposition format.** Prometheus text (the ops-standard, scrape-friendly),
   an `expvar`-style JSON blob, or both. The format decides who can consume it
   without a translator.
3. **Pull endpoint or push.** A mounted `/metrics` endpoint (pull) fits the
   single-binary model; a push to a collector needs `net.connect` — which now
   exists (id 110, landed 2026-09-07; `net.connect_tls` for an HTTPS collector,
   runtime-v2 9) — so push is possible, but pull is still the simpler default;
   say which.
4. **Is stack-trace-on-trap in this iteration at all?** It is separable, it is
   the highest debugging value per line, and it touches the trap path rather than
   the metrics path — a candidate to land first and alone.

## Out of scope (named, owned elsewhere)

- **Per-change CI and fuzzing.** Frequently lumped under the old "iteration 30"
  but they are tooling and process, not a runtime surface; they belong to a
  CI/ops story, not this one. The benchmark *harness* already exists (language
  iteration 22).
- **Distributed tracing / OpenTelemetry export.** Needs `net.connect`
  (iteration 38) and a wire protocol; a later slice if a consumer appears.
- **Alerting, dashboards.** Downstream of exposition, not the runtime's job.

## Info

Consumers exist and are named above, so this is not a primitive shipped as
decoration. Ordering: stack-trace-on-trap has no dependency and could lead;
counters/gauges are next; profiling is the heaviest and most separable. Nothing
here depends on the porch track — the dependency runs the other way.
