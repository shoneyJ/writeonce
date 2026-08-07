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

- Execute the existing plan: `docs/superpowers/plans/2026-08-01-log-watcher-sample.md`
  (five tasks: tail state machine, cron, probes+supervisor, MCP subset,
  main + README + live acceptance scenario in the `oop-accept` gate).
