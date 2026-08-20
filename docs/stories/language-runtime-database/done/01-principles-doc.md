# Iteration 1 — the principles doc

> Format: fiberloom `product/story-iteration-template`. Part of
> [Story — one language, one runtime, one database, one binary](../00-story.md).

## Goals

- The repo gains `docs/00-principles.md`: one page stating the twelve
  writeonce principles — the doctrine every later iteration links back to
  instead of re-arguing.
- The twelve: one binary is the whole system; zero dependencies (kernel
  primitives only); memory safety without a GC tax (MVS ownership, opt-in
  `@gc`, per-shard collection); no inheritance ever; thread-per-core shards
  with ownership moves; the runtime never stops (blue-green slots, embedded
  source); RAM authoritative + WAL durable; samples force the grammar;
  Linux is the target; capabilities are typed builtins (no FFI); plain
  diagnostics are the product; the runtime is a recipe box.

## Acceptance Criteria

- What to achieve?
    - **Given** a new contributor opening `docs/`,
    - **when** they read `docs/00-principles.md`,
    - **then** each principle is a short statement plus a one-line why plus
      a link to the spec or doc that enforces it, and no principle
      contradicts a locked spec decision.
- What to achieve?
    - **Given** CLAUDE.md's "Where to read next" list,
    - **when** the iteration lands,
    - **then** it contains one pointer line to the principles doc and
      nothing else about it changed.

## Out Of Scope

- Rewriting or relocating existing doctrine text in specs/plans — the doc
  links, it does not duplicate.
- Any `.wo` example content (iterations 2+ own code-adjacent artifacts).

## Info

- `docs/` numbering starts at `01-problem.md`; the `00-` slot is free and
  reads as "start here".
- Spec governing this slice: `docs/superpowers/specs/2026-08-07-logwatcher-sample-and-principles-design.md` §3.

## Proposed Solution

- Author the page with the twelve principles in the order listed in the
  governing spec; verify every link resolves; add the CLAUDE.md pointer
  line; record the commit draft in `.dev/commit.md`.
