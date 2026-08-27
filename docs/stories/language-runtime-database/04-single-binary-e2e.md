---
iteration: "4"
status: done
readiness: ready
---

# Iteration 4 — single binary end-to-end

> Format: fiberloom `product/story-iteration-template`. Part of
> [Story — one language, one runtime, one database, one binary](00-story.md).

## Goals

- The two halves meet: `woc` emits `.wob` images the VM's loader accepts,
  a conformance corpus pins language behavior from both sides, and
  `woc build` produces the story's headline artifact — one self-contained
  executable.
- This is the milestone where "a new language with arithmetic and garbage
  collection" is demonstrably real: source in, running binary out.

## Acceptance Criteria

- What to achieve?
    - **Given** the pricing demo's logic subset,
    - **when** `woc` compiles it on a developer laptop,
    - **then** compilation finishes in under 100 ms and `wovm` runs the
      resulting module with correct output.
- What to achieve?
    - **Given** the three-kind conformance corpus (runs with expected
      stdout; must-fail-compile with expected `WO-E###`; must-trap with
      expected trap code),
    - **when** the harness runs the full suite,
    - **then** every fixture lands in its expected bucket and ASan/Valgrind
      report zero errors across the suite.
- What to achieve?
    - **Given** `woc build` on a sample project,
    - **when** the produced binary is copied to a machine with no OCaml, no
      compiler, nothing but Linux,
    - **then** it runs with no arguments and behaves identically.

## Out Of Scope

- Anything beyond the milestone grammar (iteration 5 grows the surface).
- Hot reload / deployment mechanics (iteration 11 — but the self-exec
  trailer this iteration ships is its foundation).

## Info

- The loader's validation battery is the executable spec: every image
  `emit` produces must round-trip through it — the golden rule both
  test suites enforce.
- The corpus becomes the spine every later iteration extends (lang, sys,
  db, actor fixture kinds already scaffolded under `tests/corpus/`).

## Proposed Solution

- Execute the existing plan: `compiler/plan/2026-08-01-wob-emit-e2e-single-binary.md`
  (emitter with ownership lowering + drop maps + vtables, corpus harness,
  pricing corpus, ownership/trap corpora, gc pump e2e, `woc build` trailer,
  `just oop-accept` gate over the five spec success criteria).
