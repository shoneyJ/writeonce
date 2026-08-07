# Iteration 3 — compiler front (`woc`)

> Format: fiberloom `product/story-iteration-template`. Part of
> [Story — one language, one runtime, one database, one binary](00-story.md).

## Goals

- An OCaml, stdlib-only compiler front end: `.wo` source becomes a typed
  AST with ownership annotations, or plain, precise diagnostics — the
  half of the language the developer converses with.
- Ownership errors read like sentences naming both sites ("moved at
  pricing.wo:14, used at pricing.wo:17") — the ergonomics bet that makes
  MVS beat Rust for this audience.

## Acceptance Criteria

- What to achieve?
    - **Given** the pricing-demo classes,
    - **when** `woc` runs its front pipeline (lex → parse → typecheck →
      ownership),
    - **then** `--dump-ast` output matches the golden file and zero
      diagnostics are emitted.
- What to achieve?
    - **Given** a file violating ownership rules (move-after-use, borrow
      escape, double exclusive borrow),
    - **when** it compiles,
    - **then** each violation reports a stable `WO-E###` code with both
      source sites, and one bad declaration does not stop diagnosis of the
      rest of the file.
- What to achieve?
    - **Given** a class that satisfies an interface structurally,
    - **when** typechecking runs,
    - **then** satisfaction is recognized with no `implements` declaration,
      and per-method `self` mutability is inferred (reads shared, writes
      exclusive).

## Out Of Scope

- Bytecode emission and running anything (iteration 4).
- The Haxe-parity adoptions (iteration 5) — milestone grammar only.

## Info

- Newline-significant lexing and the identifier gotchas mirror
  `crates/rt`'s lexer — grammar parity is a stated contract.
- Architecture map: `compiler/plan/architecture.md` (pipeline, module
  contracts, study references).

## Proposed Solution

- Execute the existing plan: `compiler/plan/2026-08-01-woc-compiler-front.md`
  (diagnostics module, lexer, declaration/statement/expression parsers with
  skip-on-block, typechecker with field-kind derivation, MVS ownership pass
  producing the four emitter tables, driver + error catalog).
