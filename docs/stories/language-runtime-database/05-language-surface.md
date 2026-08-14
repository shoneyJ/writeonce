# Iteration 5 — language surface (Haxe-parity adoptions)

> Format: fiberloom `product/story-iteration-template`. Part of
> [Story — one language, one runtime, one database, one binary](00-story.md).


> **Status (2026-08-14):** the *grammar* half landed — modules, `and`/`or`,
> interpolation, `const`, loop control, switch expressions, typedef records,
> enum payloads, try/catch, `nil`/`?T`, statics, `pub(read)` syntax, container
> literals, `for k, v in m`, and `as` — which is what let the driving workload
> compile. The *strictness* half (`?T` forced handling `WO-E211`–`E213`,
> `pub(read)` write enforcement, `using`, `#if`, reject-row diagnostics) is
> **deliberately deferred** behind
> [`plan/compiler/2026-08-14-logwatcher-executable.md`](../../plan/compiler/2026-08-14-logwatcher-executable.md):
> it makes the language refuse more, not the program run. Plan 8 stays open
> for it.

## Goals

- The language grows from milestone grammar to a daily-driver surface: the
  systems-track verdict table's every **adopt** row — switch expressions,
  typedef records with `?fields`, `?T` optionals with null-narrowing,
  enum payload variants, try/catch/throw over traps, `static` members,
  `abstract` newtypes, `using` extensions, `use` modules, `is`, `pub` /
  `pub(read)`, `#if` build flags, string interpolation, loop control.
- Every **reject** row (inheritance, `Dynamic`, `cast`, macros, FFI…)
  refuses with a diagnostic citing doctrine — the language's boundaries are
  as deliberate as its features.

## Acceptance Criteria

- What to achieve?
    - **Given** the verdict table in the systems-track spec,
    - **when** the corpus runs,
    - **then** every adopt row has at least one golden fixture and one
      must-fail fixture passing, and every reject row that would parse
      produces its doctrine-citing diagnostic.
- What to achieve?
    - **Given** a switch over a union missing one variant and lacking
      `default`,
    - **when** it compiles,
    - **then** the diagnostic names exactly the missing variants.
- What to achieve?
    - **Given** the three fenced VM changes (catch frames, variant objects,
      boxed scalar optionals),
    - **when** they land,
    - **then** the format doc is updated in the same change and all prior
      trap fixtures still pass byte-for-byte.

## Out Of Scope

- Stdlib modules and program mode (iteration 6).
- Any new VM capability beyond the three fenced changes.

## Info

- Normative table: `docs/superpowers/specs/2026-08-01-systems-track-design.md` Part 1.
- Order inside the plan matters: modules first (everything imports through
  them), data shapes before optionals, rejects last.

## Proposed Solution

- Execute the existing plan: `compiler/plan/2026-08-01-haxe-parity-language.md`
  (nine tasks, each shipping its fixtures and error-catalog entries in the
  same task).
