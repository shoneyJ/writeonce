# Iteration 15 — deps (`wo.toml [deps]`, git fetch, `wo.lock`): implementation plan

> **For agentic workers:** use superpowers:executing-plans (inline) or
> subagent-driven-development. Steps are checkboxes. Per repo rule, this plan
> carries **actions in words + verification commands, no code blocks** — the
> normative design is the spec, which travels with this plan.

**Goal:** a writeonce project declares exact-rev git dependencies in
`wo.toml [deps]`; `woc` fetches them (via the `git` binary) into
`.wo-deps/<name>/`, pins SHAs in `wo.lock`, and resolves `use <name>` /
`use <name>/sub` into the dependency's module tree — reproducibly, offline
when the lock is satisfied, with honest diagnostics at every edge.

**Architecture:** all driver-side (`compiler/bin/main.ml`): the manifest
parser grows one value shape (the one-line inline table) and one section
(`[deps]`); a resolver runs between manifest parse and file discovery;
discovery becomes multi-root (app root + one root per dep) and `module_of`
maps each dep root to its dep name, after which the existing module/`use`/
`pub` machinery does everything else unchanged. No `types.ml`/`owner.ml`/
`emit.ml` semantics change; the entry-point selection gains one restriction.

**Tech stack:** OCaml stdlib only (`Sys.command` drives the `git` binary — no
network code in the compiler; `git` joins `cc` as an external tool the
toolchain may invoke). Acceptance via a shell gate over local `file://`
remotes — the suite stays network-free.

**Spec:** [`../specs/2026-08-18-web-framework-design.md`](../specs/2026-08-18-web-framework-design.md)
section A (normative). Story: [`15-deps-package-manager.md`](../../stories/language-runtime-database/15-deps-package-manager.md).

## Global Constraints

- OCaml stdlib only; no opam, no network code — every fetch is the `git`
  binary, every failure of it a plain diagnostic (missing binary, bad URL,
  bad rev), never a silent hang.
- `rev` is mandatory and exact (tag or SHA). No ranges, no registry, no
  transitive deps (a fetched dep whose own `wo.toml` has `[deps]` is a
  named diagnostic).
- A lock-satisfied build never touches the network; the lock wins over a
  moved tag, loudly.
- Deps activate only on the manifest path (`woc <dir>` with `wo.toml`);
  check-only and `--emit` on bare files/dirs are unchanged.
- New diagnostics are driver-level and continue the `WO-E1xx` file: E106
  (dependency fetch/shape failure — one code, message names the dep and the
  reason), E107 (dep name collides with a local module directory).
- Gates unchanged in name: `just woc-test`, `just oop-e2e`, plus this
  iteration's own `just deps-accept`.

---

## Task 1 — manifest: the `[deps]` section and the inline-table value

**Files:** modify `compiler/bin/main.ml` (manifest_parse + its doc comment).

- [x] Extend the value grammar with the one-line inline table — a braced,
  comma-separated list of `key = "string"` pairs — accepted ONLY under
  `[deps]`; anywhere else it stays "only quoted string values are supported".
  Inside it, `git` and `rev` are required, both non-empty; any other key is
  the existing unknown-key error. Section whitelist gains `deps`.
- [x] Represent each entry as (name, git URL, rev) in the parsed manifest;
  a duplicate dep name is the duplicate-key error family.
- [x] Verify: a manifest with a well-formed `[deps]` parses (no behavior
  yet); a missing `rev`, a bare unquoted value, and an inline table outside
  `[deps]` each produce their named diagnostic. Run `just woc-test` — all
  existing golden/manifest behavior unchanged.
- [x] Commit.

## Task 2 — resolver: fetch, cache, lock

**Files:** modify `compiler/bin/main.ml` (a `resolve_deps` step inside
manifest_build, before discovery); `.gitignore` (`.wo-deps/`).

- [x] Layout: `.wo-deps/<name>/` beside `wo.toml`; `wo.lock` beside it too —
  one line per dep, name and resolved commit SHA, sorted, with a one-line
  header comment naming the generator.
- [x] Cold path (no lock entry or no cache dir): run `git clone` into the
  cache dir and `git -C <dir> checkout <rev>`, then read the resolved SHA via
  `git -C <dir> rev-parse HEAD`; write/refresh the lock entry. Every git
  invocation's failure is WO-E106 naming the dep, the URL/rev, and which step
  failed. A missing `git` binary is its own WO-E106 message.
- [x] Warm path (lock entry present, cache dir present): compare the cache's
  HEAD SHA to the lock; equal means proceed with zero network. A cache
  matching the manifest `rev` label but not the lock (a moved tag) is
  WO-E106 "lock drift" naming both SHAs and pointing at `--update-deps`.
- [x] `woc --update-deps <dir>`: re-fetches every dep at its manifest rev and
  rewrites the lock; document in the usage text.
- [x] Guard rails, each its own diagnostic: the fetched dep has no `wo.toml`
  or no `name` (not a writeonce project); the dep's `wo.toml` contains
  `[deps]` (transitive — refused flat-only, per spec); the dep name collides
  with a local top-level module directory in the app (WO-E107).
- [x] Verify manually against a local `file://` remote: cold build fetches
  and writes the lock; second build is offline (prove by running with
  `GIT_TRACE` absent and the remote renamed away); tag-move produces the
  drift diagnostic; `--update-deps` clears it. Commit.

## Task 3 — multi-root discovery, module mapping, entry restriction

**Files:** modify `compiler/bin/main.ml` (discovery + `module_of` + entry
candidate filtering in manifest_build's pipeline call).

- [x] Discovery: after resolve_deps, discover each dep root exactly as the
  app root is discovered (same skip rules — dot-dirs, `target/`, and now
  `.wo-deps/` itself under the app root so dep trees are never discovered
  twice) and append its files to the compilation unit list. Deterministic
  order: app files first (sorted, as today), then deps sorted by name.
- [x] Module mapping: `module_of` for a dep file prefixes the dep name — the
  dep's root maps to module `<name>`, its subdirectory `sub/` to
  `<name>/sub` — so the existing `use` resolution, collision diagnostics,
  and `pub` visibility work across the boundary with no changes in
  `types.ml`.
- [x] Entry restriction: the `main` the emitter selects (and WO-E405 checks)
  must come from the app root's own files; a dep's `fn main` is never an
  entry candidate. The spec's rule "a dep's main is ignored" means exactly
  the selection filter — the fn itself still compiles as an ordinary
  module-scoped fn.
- [x] Verify: an app importing one dep via `use <name>` and `use <name>/sub`
  builds and runs; the dep having its own `fn main` changes nothing;
  `woc --dump-owner`/`--dump-bc` on the app still work (multi-file dump
  conventions apply to dep files like any other unit). Commit.

## Task 4 — the acceptance gate and fixtures

**Files:** create `scripts/deps-accept.sh`; modify `justfile` (recipe
`deps-accept`); fixtures under `tests/deps/` (fixture projects only — the
script builds its `file://` remotes in a temp dir at run time, so nothing
network-shaped or `.git`-shaped is committed).

- [x] The gate script, one check per behavior, log-watcher-accept style:
  (1) cold fetch + lock written + app runs; (2) offline rebuild with the
  remote removed; (3) moved-tag drift diagnostic; (4) `--update-deps`
  refresh; (5) transitive-dep refusal; (6) dep-name/local-module collision;
  (7) dep `fn main` ignored — app's entry wins; (8) missing-`rev` manifest
  diagnostic. Exit nonzero on the first failure, count summary at the end.
- [x] `just deps-accept` wired; run it plus `just woc-test` and
  `just oop-e2e` — all green, nothing pre-existing re-blessed.
- [x] Commit.

## Task 5 — docs closeout

**Files:** modify `docs/plan/oop-vm/01-error-catalog.md` (E106/E107 rows),
`README.md` (a short "Dependencies" subsection under the manifest docs),
`docs/00-status.md` (iteration 15 row → done, with what actually landed),
story `15-deps-package-manager.md` (status note), `docs/08-project-structure.md`
(`.wo-deps/` + `wo.lock` in the project-layout listing).

- [x] Apply; `just deps-accept` still green; commit.

## Success criteria (from the story, restated as the gate)

1. Cold build fetches through `[deps]`, writes `wo.lock`, runs; warm build is
   provably offline.
2. Lock beats a moved tag with a named diagnostic; `--update-deps` refreshes.
3. Transitive deps, missing rev, non-writeonce deps, and name collisions are
   named diagnostics — never silence, never misresolution.
4. A dep is consumed purely through `use <name>`; its `fn main` never becomes
   the entry.

## Self-review notes

- Spec §A coverage: manifest shape → Task 1; fetch/lock/cache + guard rails →
  Task 2; resolution + entry rule + `pub` boundary → Task 3; network-free
  testing promise → Task 4; catalog/docs → Task 5. No gaps found.
- The one deliberate interpretation: the spec's `[deps]` inline-table syntax
  is implemented as written (Task 1 grows the value grammar) rather than bent
  to `[deps.<name>]` subsections — the manifest stays the document users saw
  in the spec.
- Risk called out: discovery skip rules must exclude `.wo-deps/` from the app
  root's own walk, or every dep compiles twice with colliding modules — Task
  3 carries that explicitly and fixture (1) would catch it.
