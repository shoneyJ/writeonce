# Iteration 17 — library projects and dependency privacy (`kind`, `internal/`)

> Format: fiberloom `product/story-iteration-template`. Part of
> [Story — one language, one runtime, one database, one binary](00-story.md).
>
> **Inserted 2026-08-20, needs further refinement** (developer decision: keep
> as an iteration, do not implement yet). The forks below are genuinely open;
> a brainstorm/spec settles them before any plan.

## Why this iteration exists

Iterations 15/16 made cross-repo libraries real — and exposed two gaps the
established ecosystems solved long ago:

1. **Library-ness is implicit and broken.** A project without `fn main` (the
   framework) cannot be checked by `woc <dir>`: manifest presence forces
   build mode, which errors "no `main` entry point found". Iteration 16
   verified the framework through a `woc --emit` workaround — a wart, not a
   design.
2. **The dependency boundary leaks internals.** `pub` is module-public with
   no dep-private tier: the framework's plumbing (`parse_request`, the hex
   decoder, `route_match`) is exactly as importable by the consuming app as
   its intended surface (`Handler`, `App`, the builders). Nothing marks "this
   module is the library's own business".

## The conventions, analyzed (the corpus for the spec)

**Go:** no manifest marker — the *package* decides (`package main` +
`func main` = executable; anything else = library), `cmd/<name>/` hosts
multiple binaries, and **`internal/`** carries all encapsulation: the
compiler refuses any import of a path containing `internal/` from outside
the subtree rooted at `internal/`'s parent. Directory-shaped privacy, zero
keywords — public repo, private API.

**Rust:** manifest-declared targets (`[lib]` / `[[bin]]`, `src/lib.rs` /
`src/main.rs` conventions; a crate can be both) and *keyword-grained*
visibility: `pub`, `pub(crate)`, `pub(super)`, `pub(in path)`. A dependency
sees only what is `pub`-reachable from the crate root. No `internal/`
convention — `pub(crate)` does that job.

**Fit to writeonce doctrine:** directory-as-module and keyword frugality
point at Go's shape — but writeonce *has* a manifest (Go does not), so the
library marker can be explicit where Go infers, giving clearer errors.

## Goals (draft — the spec refines)

- A library project is first-class: declared in `wo.toml`, `woc <dir>`
  typechecks it whole (no entry required), `woc build` on it refuses with a
  message that says what it is. The framework adopts it and loses the
  `--emit` workaround.
- A dependency has a private interior: some modules are importable inside
  the dep but not across the `[deps]` boundary; violations are a named
  compile diagnostic at the offending `use`.
- The framework reorganizes to demonstrate both (its parser plumbing moves
  behind the privacy line; `Handler`/`App`/builders stay public).

## Open forks (each a real decision for the spec)

1. **How library-ness is declared.** `kind = "library"` top-level key vs a
   `[lib]` section vs pure inference from "no entry-shaped main". Leaning:
   the explicit key — the manifest exists, and explicit beats inference in
   error messages — with "program" the default.
2. **Privacy mechanism.** Go's `internal/` directory rule (pure
   use-resolution change, zero new syntax, coarse) vs Rust's `pub(lib)`
   visibility keyword (fine-grained, touches parser + typechecker + the
   error catalog) vs a manifest `export = [...]` module allowlist (explicit
   surface, but a second place to maintain). Leaning: `internal/` — it
   matches directory-as-module exactly and costs a resolution rule.
3. **Scope of the internal rule.** Dep-boundary-only (importable anywhere
   inside the dep, refused from the consumer) vs Go's full subtree rule
   (importable only under `internal/`'s parent, even within one project).
   Dep-boundary-only is the smaller honest cut; Go's rule also disciplines
   large single projects.
4. **Can one project be both** (Rust's lib+bin)? A framework shipping a demo
   binary wants it; the entry-selection rule (iteration 15's "a dep's main is
   never the entry") already half-answers it. Decide explicitly.

## Acceptance Criteria (draft)

- **Given** the framework marked as a library, **when** `woc <dir>` runs,
  **then** it typechecks the whole project with no entry required, and
  `woc build` refuses with a message naming the kind.
- **Given** a framework module behind the privacy line, **when** the web-app
  `use`s it, **then** a named diagnostic points at the `use` and names the
  dependency; inside the framework the same import stays legal.
- **Given** the public surface (`Handler`, `App`, builders), **when** the
  web-app builds, **then** nothing changed — `just web-app` stays 14/0.

## Out Of Scope

Registries/semver (still future), transitive deps (15's flat-only stands),
`pub(super)`-style fine grains beyond the chosen mechanism, multiple named
binaries per project (`cmd/` convention — record, don't build).

## Proposed Solution

Brainstorm → spec settling the four forks, then a small plan: manifest key +
driver check-mode for libraries, the use-resolution privacy rule + WO-E1xx
diagnostic, the framework reorg, and `just web-app` as the regression gate.
