---
iteration: "7"
status: done
readiness: ready
---

# Iteration 7 — log-watcher proof workload

> Format: fiberloom `product/story-iteration-template`. Part of
> [Story — one language, one runtime, one database, one binary](00-story.md).

## Goals

- The language's driving workload runs for real: the Haxe log-watcher
  daemon (~1,200 lines) re-expressed file-for-file in `.wo` at
  `docs/examples/log-watcher/`, compiled by `woc`, detecting a silent
  death on a live file.
- The README mapping table's "could not express" column is empty — the
  systems track's acceptance bar: nothing in a real daemon exceeded the
  language.

> **Status: ✅ landed 2026-08-15.** Compile-and-run met 2026-08-14 (zero
> diagnostics, 106 KB standalone binary, all three modes work); the
> executable half landed 2026-08-15 — zero ASan leaks in all three modes,
> SIGTERM ends parked syscalls, fds flat across 200 requests, and the
> `LW_SOAK` gate holds RSS/descriptors flat under sustained load.
> `just log-watcher`: 7 checks, 0 failures. The work is recorded in
> [`plan/compiler/2026-08-14-logwatcher-executable.md`](../../plan/compiler/2026-08-14-logwatcher-executable.md)
> and nothing else blocks this iteration.

## Acceptance Criteria

- What to achieve?
    - **Given** the eight-file `.wo` port (logtail, watcher, cron, probes,
      supervisor, mcp, tools, main),
    - **when** `woc` compiles the sample and the ported fixture groups run,
    - **then** compilation is clean and every fixture mirrors its named
      Haxe test case with matching behavior.
- What to achieve?
    - **Given** the built sample running `watch` against a growing
      tempfile,
    - **when** the feed ends with an error line and the quiet period
      elapses,
    - **then** exactly one detection line appends to the JSONL sink —
      the original's measured behavior, reproduced.
- What to achieve?
    - **Given** the README mapping table,
    - **when** the iteration closes,
    - **then** every row names its `.hx` sibling and deliberate
      divergences, and the could-not-express column is empty.
- What to achieve? *(added 2026-08-14 — compiling is not running)*
    - **Given** any of the three modes started under a sanitizer build,
    - **when** it is signalled to stop after a soak,
    - **then** it exits cleanly on SIGTERM alone, reports zero leaks, and
      its resident size and descriptor count are flat across the soak.

## Out Of Scope

- The sqlite-backed minilog tools — expressible when iteration 9's SQL
  lands; recorded as scoped-out, not inexpressible.
- New language or stdlib features: a gap found here is a defect report
  against iterations 5/6, and this iteration stops until it's resolved.

## Info

- The `.wo` files may be authored docs-first ahead of this iteration
  (spec `2026-08-07-logwatcher-sample-and-principles-design.md`); this
  iteration is where they must actually compile and run.
- The pure-core discipline (tail state machine, cron math, MCP `handle`
  socket-free and clock-injected) is preserved — the original's best
  design decision.

## Proposed Solution

- The authoring plan (`docs/superpowers/plans/2026-08-01-log-watcher-sample.md`)
  is spent: the `.wo` files exist and compile.
- What remains is
  [`plan/compiler/2026-08-14-logwatcher-executable.md`](../../plan/compiler/2026-08-14-logwatcher-executable.md)
  — six tasks, every one traced to a measurement on this sample: the ownership
  pass learning stdlib return types, dropping a projected temporary, the
  runtime's own argv container, honouring the stop signal in blocking calls,
  closing accepted connections, and a soak that would have caught all of it.
- `just log-watcher` (`scripts/log-watcher-accept.sh`) is this iteration's gate:
  compile, watch alert, cron schedule, and three MCP checks today; the soak
  joins it in the last task.
