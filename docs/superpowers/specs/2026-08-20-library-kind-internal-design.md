# Iteration 17 — library projects and dependency privacy: design

> **Status: ✅ LANDED 2026-08-20** — implemented as specified; the impact
> analysis held (driver-only, VM/`.wob`/GC untouched). Approved, then parked
> the same day by directive, then unparked and executed. Decisions were
> settled in
> [the iteration](../../stories/language-runtime-database/done/17-library-projects-internal.md)
> (four forks + impact analysis); this spec makes them buildable. The plan
> follows after review. Board: [docs/00-status.md](../../stories/00-status.md).
>
> Per repo convention this spec carries concept, reason, and required
> behavior in words only — no implementation code.

## Goal

A project can say it is a library, and a dependency can keep modules to
itself. Concretely: `docs/examples/writeonce-framework` declares
`kind = "library"` in its `wo.toml`, `woc <dir>` on it typechecks the whole
project with no entry required (the iteration-16 `--emit` workaround is
deleted), its parser and serve-loop plumbing move under `internal/` where
the web-app cannot import them, and `just web-app` proves nothing public
broke.

## Background (the two gaps, from iterations 15/16)

1. A project without `fn main` cannot be checked: manifest presence forces
   build mode, which errors "no `main` entry point found". The framework is
   verified today through `woc --emit` — a wart.
2. `pub` is module-public with no dep-private tier: `parse_request` is
   exactly as importable by the consuming app as `Handler`. Nothing marks
   "this module is the library's own business".

## Settled decisions (normative, from the iteration)

- Library-ness is manifest-declared: top-level `kind` key, values
  `"program"` (the default when absent) and `"library"`; any other value is
  a manifest error.
- Privacy is Go's `internal/` directory rule, applied at the `[deps]`
  boundary only: a consumer cannot `use` a dependency module whose path
  contains an `internal` segment; inside the dependency the same import
  stays legal.
- Lib+bin duality is allowed: a library may carry an entry-shaped `main`;
  `kind = "library"` changes only the DEFAULT action of `woc <dir>`.
- Inherited from Go, explicitly not checked: an internal type may appear in
  a public signature. The consumer can hold and pass such a value but
  cannot import the module to name its type. Library author's smell to
  avoid, not a diagnostic.

## Design

### 1. The manifest `kind` key

One top-level key in `wo.toml`, read where the manifest is already parsed
(the driver's manifest reader in `compiler/bin/main.ml`). Absent means
program — every existing project keeps its behavior. A value other than
`"program"` or `"library"` is rejected with the new WO-E109, naming the
value and the two legal ones. The key is meaningful only in the project
being invoked; a DEPENDENCY's `kind` is read but not enforced in v1
(iteration 15 already never uses a dep's `main`, so a program consumed as a
dep already behaves as a library — recorded, not policed).

### 2. Driver modes

- `woc <dir>` on a program: unchanged — build, requiring `main`.
- `woc <dir>` on a library: CHECK mode — the full pipeline runs (parse,
  typecheck, interface satisfaction, borrow/ownership pass, GC inference)
  over the whole project including its `[deps]`, and stops before image
  emission. No entry is required. Exit 0 with no output on success, 1 with
  diagnostics otherwise — a green check must mean exactly what a green
  build means, minus the artifact. Deps are fetched/locked the same as
  build mode (a library must be checkable offline once locked).
- `woc build <dir> -o <app>` on a library WITH a `main`: builds the binary
  — the dual case (demo/self-test). Without a `main`: the existing
  no-entry error, extended to name the kind so the message explains itself
  ("this project is a library; add a main or check it with woc <dir>").
- `--emit`, dump flags, `--update-deps`: unchanged; they never required an
  entry or already carry their own rules.

### 3. The `internal/` rule

Where: the dep-use resolution step in `compile_image` — the same place
iteration 15 prefixes dep module paths — because that is the only spot that
knows which file belongs to which project. The rule: a `use` in a file that
does NOT belong to dependency X, naming a module of dependency X whose
dep-relative path contains a segment exactly equal to `internal`, is
rejected with WO-E108 at that `use`, naming the dependency and the module.
Files INSIDE dependency X importing the same module are untouched. Modules
are directory-shaped (`module_of_multi`: dep name + relative directory), so
"segment" means a path component — `framework/internal` and anything under
it, never a substring match (a module named `internals_x` is not caught).

Scope notes, normative: the rule fires only across the `[deps]` boundary
(decision 3 — dep-boundary-only; Go's subtree rule is a recorded possible
tightening). The root project's own `internal/` directories are legal to
import from anywhere inside the root project. Transitive deps stay
rejected by iteration 15's WO-E106, so dep-to-dep imports cannot occur.

### 4. Diagnostics (error catalog additions)

- **WO-E108** — dep-internal module imported across the `[deps]` boundary.
  Points at the offending `use`, names the dependency and the internal
  module, and says the module is internal to that dependency.
- **WO-E109** — invalid manifest `kind` value. Names the given value and
  the two legal ones.
- The no-entry build error gains the library wording described in §2; no
  new code, better message.

### 5. Framework reorg (the proof by use)

- `wo.toml` gains `kind = "library"`.
- `http/parse.wo` and `http/serve.wo` move to `internal/` (module
  `framework/internal` when consumed). They are plumbing the web-app never
  imports: the request parser, the carry-state record, the serve loop, the
  `Dispatcher` seam.
- `http/types.wo` (Req/Resp + builders), `router/router.wo`
  (Handler/Middleware/Route/Mw), and `app.wo` (App) stay where they are —
  the public surface does not move.
- Intra-framework imports update to the new module path; the web-app
  changes NOTHING — it already imports only `framework`, `framework/http`,
  `framework/router`.
- The framework README's verification note replaces the `--emit`
  workaround with the check-mode invocation.

### 6. Acceptance gate

Extend `scripts/web-app-accept.sh` (it already builds the framework remote
and the consuming app): the standing 14 checks stay, plus (a) check mode —
`woc` on the framework copy exits 0 with no entry present; (b) privacy — a
temp copy of the app with one added `use framework/internal` fails
compile and the output names WO-E108; (c) kind validation — a temp
manifest with a junk `kind` fails naming WO-E109. Compiler-side, the
corpus grows compile-fail fixtures for WO-E108/E109 and a run fixture is
not needed (no runtime behavior exists to pin — the VM is untouched by
design). Standing gates (`just woc-test`, `just deps-accept`,
`just oop-e2e`, samples) stay green.

## Out of scope (recorded, deliberate)

Go's full subtree rule; a `[lib]` manifest section; `pub(lib)`-style
keyword visibility; manifest export allowlists; multiple named binaries
per project (`cmd/` convention); enforcing a dep's own `kind`; internal
types in public signatures as a diagnostic; any VM, `.wob`, or GC change
(impact analysis in the iteration: visibility is compile-time name
resolution; libraries compile whole-program into the consumer's image;
GC inference stays whole-program and app usage may promote dep classes —
intended).

## Success criteria

1. **Given** the framework with `kind = "library"` and no `main`, **when**
   `woc <dir>` runs, **then** it exits 0 having run the full pipeline, and
   `woc build` on it fails with the message naming the kind.
2. **Given** the web-app importing `framework/internal`, **when** it
   compiles, **then** WO-E108 points at the `use` and names the framework;
   the framework's own files importing it stay legal.
3. **Given** the reorganized framework, **when** `just web-app` runs,
   **then** all standing checks pass unchanged — the public surface did
   not move.
4. **Given** any existing project with no `kind` key, **when** it builds,
   **then** nothing changed — absent means program.
