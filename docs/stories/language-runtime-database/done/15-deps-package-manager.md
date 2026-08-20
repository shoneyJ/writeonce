# Iteration 15 — dependencies: `wo.toml [deps]`, git fetch, `wo.lock`

> Format: fiberloom `product/story-iteration-template`. Part of
> [Story — one language, one runtime, one database, one binary](../00-story.md).
>
> **Inserted 2026-08-18. LANDED the same day** (branch `web-framework`): all
> acceptance criteria met — `just deps-accept` 8/0 (cold fetch + lock, use
> <dep> and <dep>/sub, offline-when-locked with the remote deleted,
> lock-beats-moved-tag, --update-deps, drift/transitive/collision
> diagnostics WO-E106/E107, dep `fn main` never the entry, manifest shape).
> The enabler for code shared between writeonce repositories — the web
> framework (iteration 16) is the driving consumer.
>
> **Spec exists:** [`2026-08-18-web-framework-design.md`](../../../superpowers/specs/2026-08-18-web-framework-design.md)
> section A is normative for this iteration. **Plan:**
> [`2026-08-18-deps-package-manager.md`](../../../superpowers/plans/2026-08-18-deps-package-manager.md)
> (5 tasks: manifest inline-table + [deps]; resolver fetch/cache/lock;
> multi-root discovery + module mapping + entry restriction; the
> `just deps-accept` gate over file:// remotes; docs closeout).

## Goals

- A project declares exact-rev git dependencies in `wo.toml [deps]`
  (`name = { git = "...", rev = "..." }`); `woc` fetches them (via the `git`
  binary — no network code in the compiler) into `.wo-deps/<name>/` and
  resolves `use <name>` / `use <name>/sub` into the dep's module tree.
- `wo.lock` pins resolved SHAs: builds are reproducible, a moved tag is
  reported rather than silently followed, and a lock-satisfied build never
  touches the network.
- Honest edges: transitive `[deps]` refused with a diagnostic (flat-only v1),
  dep/local module-name collisions diagnosed, a dep's `fn main` ignored,
  `pub` applies across the boundary exactly as across modules.

## Acceptance Criteria

- **Given** an app whose `[deps]` names a framework in a local `file://` git
  repo, **when** `woc <app>` runs twice, **then** the first run fetches +
  writes `wo.lock`, the second builds offline from `.wo-deps`, and the built
  binary runs.
- **Given** the dep's tag moved after `wo.lock` was written, **when** the app
  builds, **then** the lock wins and the drift is reported;
  `woc --update-deps` refreshes it.
- **Given** a dep whose own `wo.toml` has `[deps]`, or a dep name colliding
  with a local module, **when** the app builds, **then** each is a named
  diagnostic, never silent misresolution.

## Out Of Scope

Registries, version ranges/semver solving, transitive dependencies (the
successor's first fork), private-remote auth handling beyond what ambient
`git` config provides, vendoring commands.

## Proposed Solution

Manifest parsing extends `compiler/bin/main.ml`'s existing wo.toml reader;
fetch = `Sys.command` over the `git` binary; resolution plugs the dep root
into the existing directory-as-module discovery. Fixtures under `tests/` use
local `file://` remotes so the suite stays network-free.
