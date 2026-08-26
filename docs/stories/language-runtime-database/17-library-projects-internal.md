---
iteration: "17"
status: done
---

# Iteration 17 — library projects and dependency privacy (`kind`, `internal/`)

> Format: fiberloom `product/story-iteration-template`. Part of
> [Story — one language, one runtime, one database, one binary](00-story.md).
>
> **Inserted 2026-08-20, needs further refinement** (developer decision: keep
> as an iteration, do not implement yet).
>
> **Refined 2026-08-20: all four forks SETTLED** (developer decisions, no code
> changed). See "Settled decisions" and "Impact analysis" below.
>
> **⏸ PARKED 2026-08-20** (developer directive: framework v1 work first).
> The spec and plan were written and approved before parking
> (`docs/superpowers/specs/2026-08-20-library-kind-internal-design.md`,
> `docs/superpowers/plans/2026-08-20-library-kind-internal.md`).
>
> **LANDED 2026-08-20** — unparked and executed against that plan, all six
> tasks. Every change is in the driver (`compiler/bin/main.ml`); the VM,
> `.wob`, and GC are untouched exactly as the impact analysis predicted.
> Reasoning-under-the-code in `compiler/src/CODE-LOGIC.md`.
>
> Gates: `just web-app` **26/0** (three new checks — library check mode,
> WO-E108 at the boundary, WO-E109 on a bad kind), and `just woc-test`,
> `just oop-e2e` (103/0), `just deps-accept`, `just log-watcher`,
> `just employee` all unchanged.
>
> Two deviations from the plan, both because the framework grew after the plan
> was written:
>
> 1. **`http/parse.wo` was SPLIT, not moved whole.** The plan said move it
>    under `internal/`, but the framework-v1 slices had since added
>    `media_type` and `form_values` to that file and the web-app calls both —
>    moving the file whole would have put public surface behind the privacy
>    line and broken the consumer. The parsing plumbing (`Parsed`,
>    `parse_request`, `url_decode`, `parse_query`) is now `internal/parse.wo`;
>    the two public functions are `http/form.wo`, which does `use internal`
>    (legal inside the library).
> 2. **The gate is 26/0, not the plan's 17/0.** `just web-app` had grown from
>    14 to 23 checks (framework v1 plus iteration 19's Float price) before this
>    iteration started; the three new checks make 26. The plan's numbers were
>    written against a 14-check gate. Acceptance criterion 3 below still holds
>    in substance: no pre-existing check changed.

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

## Settled decisions (2026-08-20 — the former open forks)

1. **Library-ness is manifest-declared: `kind = "library"`**, a top-level
   `wo.toml` key, default `"program"`. Explicit beats inference in error
   messages both ways: a library refuses the default build with a message
   naming its kind, and a program missing `main` stays a loud error instead
   of silently becoming a library. (Rejected: a `[lib]` section — ceremony
   with nothing to hold yet; Go-style inference — makes forgot-`main` and
   is-a-library indistinguishable.)
2. **Privacy mechanism: Go's `internal/` directory rule.** A dependency
   module whose path contains the segment `internal` is not importable
   across the `[deps]` boundary; the violation is a named diagnostic at the
   offending `use`, naming the dependency. Zero new syntax — a pure
   use-resolution rule, matching the directory-as-module doctrine exactly.
   (Rejected: `pub(lib)` keyword — parser + typechecker + catalog surface
   for granularity nothing needs; manifest export allowlist — a second
   place that drifts from the code.)
3. **Scope: dep-boundary only.** `internal/` modules stay importable
   anywhere INSIDE the dependency; only the consumer is refused. The
   smaller honest cut; Go's full subtree rule (parent-of-`internal/` scope,
   enforced even within one project) is recorded as a possible later
   tightening — adopting it later only ever rejects more, never breaks a
   consumer.
4. **Lib+bin duality: allowed.** A library MAY carry an entry-shaped
   `main` (demo/self-test binary). `kind = "library"` changes the DEFAULT
   action of `woc <dir>` to whole-project typecheck; an explicit build
   invocation still produces the binary when a `main` exists. As a
   dependency its `main` is never the entry — iteration 15 already ships
   that rule.

Follow-on rule inherited from Go, recorded so the spec doesn't relitigate
it: an internal type MAY appear in a public signature (Go permits exported
functions returning internal types — the consumer can hold and pass the
value but cannot `use` the module to name the type). No extra check in v1;
it is the library author's own smell to avoid.

## Impact analysis (what each layer feels)

**Framework (`docs/examples/writeonce-framework/`).** Gains one manifest
line (`kind = "library"`); `woc <dir>` then typechecks the whole project
with no entry required — the iteration-16 `--emit` verification workaround
dies. Reorg: the plumbing the web-app never imports — the request parser
module (parse_request, url_decode, the carry-state record) and the serve
loop internals — moves under `internal/`; the public surface (`Handler`,
`Middleware`, `App`, `Req`/`Resp`, the response builders) stays where it
is. The framework's own `use` of its internal modules stays legal
(decision 3). The web-app changes nothing: it already imports only the
public modules, so `just web-app` staying 14/0 is the regression gate, not
a migration.

**Compiler (`woc`) — the only place code would change.** Two seams, both in
existing passes: the driver reads the `kind` key and picks
check-mode-by-default for libraries (build mode already exists; check mode
must still run the FULL pipeline — parse, typecheck, borrow check, GC
inference — so a green check means what a green build means); and the
dep-use resolution (the iteration-15 prefixing step) refuses a consumer
`use` whose dep-relative path contains `internal`, with a new WO-E1xx.
No lexer, parser, or type-system syntax changes — `internal` is a path
shape, not a keyword.

**VM (`wovm`): zero impact, by construction.** Visibility is name
resolution at compile time; the `.wob` format carries no module or
visibility metadata to extend — no new opcodes, no version bump, no loader
change. A library never yields its own `.wob` at all: iteration 15's model
compiles dependencies whole-program into the consumer's single image, and
that stands. The VM never learns "library" exists. A dual-built demo
binary is an ordinary program image. One honest disclosure: `internal/`
modules still compile INTO the consumer's image (there is no dead-code
elimination) — privacy restricts naming, it strips nothing.

**GC: zero mechanism impact, one semantic note worth pinning.** GC-ness is
whole-program inferred (iteration 7b): structural SCC plus demand
promotion run over the app's AND every dep's classes together, AFTER use
resolution — privacy is invisible to inference. So a framework class can
still be promoted to traced by how the APP uses it (an escape in app code
promotes the escaping projection's class, wherever that class lives), and
`internal/` does not fence that off. This is correct and intended:
GC-ness stays a per-consumer, whole-program property, not a library
promise — a library author cannot pin "my class is untraced" any more
than before. Library check mode runs the same inference (over the library
alone), so a library-standalone check and an app-embedded compile may
legitimately disagree about traced-ness — that is the design, restated
here so nobody files it as a bug. Runtime GC (header, barrier, safepoints,
budgets) untouched.

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

Forks settled above (2026-08-20). Remaining path: spec + small plan —
manifest `kind` key + driver check-mode for libraries, the use-resolution
privacy rule + WO-E1xx diagnostic, the framework reorg (plumbing under
`internal/`), and `just web-app` as the regression gate. VM and GC are
untouched by design (see Impact analysis).
