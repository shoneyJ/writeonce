---
track: runtime-v2
iteration: "7"
was_language_iteration: "30"
status: pending
readiness: ready
review_pending: "forks auto-approved 2026-09-09 for autonomous execution — developer second review before code lands; four forks settled with KISS defaults grounded in runtime/src (existing gauges, existing line table)"
---

# runtime-v2 7 — observability: trace on trap, then counters and gauges

> Moved 2026-09-06 from the language track (was language iteration 30, the
> number a dozen docs still point at) into runtime-v2, whose builtin-sized-seam
> shape it fits. **Brainstormed to `ready` 2026-09-09**: the four forks below are
> settled, grounded in what the runtime already holds rather than in what an
> observability stack usually ships. Profiling is split out (fork 1) — this
> iteration is the cheap, high-value half.

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
- **Debugging a trap** today is one stderr line — `trap CODE in METHOD at line
  N: MESSAGE` — with no call stack, so a trap three calls deep names only the
  innermost frame.

Fiber ships `expvar` and `pprof` as middleware; the equivalents here are runtime
work, because the numbers they expose (allocations, fiber counts, shard load,
GC pauses) live in the C runtime, not in `.wo`.

## Decisions locked (2026-09-09)

1. **Counters and gauges only; profiling is its own later iteration.** The
   gauges this iteration exposes already exist as runtime fields —
   `gc_traced_cnt`, `gc_alloc_bytes`, `gc_step_no` (obj.h), the arena's `used`,
   `nfibers`, `nchildren`, `ntls` (vm.h) — so exposing them is a read, not new
   machinery. CPU/heap profiling needs sampling infrastructure in the VM and is a
   different size of commitment; it gets its own runtime-v2 iteration when a
   consumer measures the need. (The story always allowed this split.)
2. **Exposition is Prometheus text, rendered in `.wo`.** The runtime returns
   numbers; the *format* is the consumer's. The builtin hands back a
   `map<Text, Int>` (an existing container — no new record class), and porch
   renders the ops-standard Prometheus text (`name value` lines) from it in a
   few `.wo` lines. No `expvar`-style JSON in this iteration (a `json.encode` of
   the same map is a one-liner if a consumer asks); no format negotiation.
3. **Pull, not push.** A `proc.metrics() -> map<Text, Int>` builtin — `proc`
   because it is this process/shard's introspection, beside `proc.run`/`spawn` —
   and porch mounts `/metrics` on it. Push to a collector is possible now that
   `net.connect`/`net.connect_tls` exist, but it needs a collector protocol and a
   consumer; deferred by name. The snapshot is **per shard** (the calling
   shard's gauges); a cross-shard aggregate would need an inbox round-trip and
   is deferred — scrape each shard or sum in the app.
4. **Stack trace on trap lands first, alone, in this iteration.** Highest
   debugging value per line and zero dependency on the metrics half. The trap
   path already resolves method + line through the per-method line table
   (loader.c validates it ascending); `vm_unwind` already walks the frame stack.
   Phase A walks the frames *before* unwinding and prints one `  at METHOD line
   N` line per frame under the existing trap line — both the main-fiber trap
   (main.c) and the fiber trap (vm.c) sites. Stderr only, always on (a trap is
   already a stderr event); no new builtin, no format flag.

Builtin id: the next free after the ones being claimed ahead of it
(`random_bytes` takes 119 per porch 2's brief) — **confirm against `WO_B_MAX`
in `runtime/src/wob.h` at build time; the id space is one shared enum**
(wob.h + emit.ml/types.ml + loader.c arity), the lesson porch 2's brief
recorded.

## Phases

- **A — stack trace on trap.** At both trap-report sites, walk the trapping
  fiber's frames innermost-first and print `  at <method> line <n>` per frame
  (line from the method's line table at the frame's pc; `?` when a method has
  no table). Verify: a corpus/regress fixture that traps three calls deep shows
  three `at` lines in order under the trap line; a top-level trap shows one;
  every existing fixture's first stderr line is byte-unchanged (the trace is
  appended, never prepended).
- **B — `proc.metrics()`.** The builtin fills a `map<Text, Int>` from the
  shard's existing fields — at least `arena_used_bytes`, `gc_traced_objects`,
  `gc_alloc_bytes_since_cycle`, `gc_slices`, `live_fibers`, `live_children`,
  `live_tls_conns`, plus `shard_id` — registered in wob.h + types.ml + loader.c
  (module member, `proc`). Verify: a runtime test asserts every key is present
  and that `live_fibers` rises with spawned fibers and falls when they finish;
  the map's keys are stable names (they are the metric names porch will emit).
- **C — porch mounts `/metrics`** rendering Prometheus text from the map. This is
  the **consumer's** phase — pure `.wo`, owned by porch 8's lifecycle slice — and
  is named here so the builtin ships with its consumer visible, not built here.

## Acceptance Criteria

- **Given** a program that traps three calls deep, **when** it traps, **then**
  stderr carries the existing trap line unchanged followed by three `at METHOD
  line N` lines, innermost first.
- **Given** every existing fixture and gate, **when** run, **then** the first
  stderr line of each trap is byte-identical to before (the trace only appends).
- **Given** a `.wo` program that spawns N fibers and calls `proc.metrics()`,
  **when** inspected, **then** `live_fibers` reflects them and every named key is
  present with an `Int`.
- **Given** the map, **when** porch renders it, **then** the output is valid
  Prometheus text (one `name value` line per key) — proven in porch's own gate
  when phase C lands.

## Out of scope (named, owned elsewhere)

- **CPU/heap profiling** (`pprof`'s equivalent) — its own runtime-v2 iteration
  when a consumer measures the need (fork 1).
- **Push / OpenTelemetry export** — needs a collector protocol and a consumer;
  `net.connect_tls` now exists, so the transport is no longer the blocker.
- **Cross-shard aggregation** of the gauges — an inbox round-trip; scrape or sum
  in the app for now.
- **`expvar`-style JSON**, format flags, per-request latency histograms — later,
  on demand.
- **Per-change CI and fuzzing.** Tooling and process, not a runtime surface;
  the benchmark *harness* already exists (language iteration 22).
- **Alerting, dashboards.** Downstream of exposition, not the runtime's job.

## Info

Consumers exist and are named above, so this is not a primitive shipped as
decoration. Ordering: phase A (trace on trap) has no dependency and leads;
phase B (counters) follows; phase C is porch's. Nothing here depends on the
porch track — the dependency runs the other way. Small: a frame walk at two
trap sites, one module builtin reading fields that already exist.
