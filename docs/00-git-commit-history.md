# Git commit history — features and their cherry-picks

Reference for **which commits carried which feature onto `master`**, so a
feature can be traced, re-reviewed, or reverted as a unit long after the
history it was written in has moved on.

## The workflow this file records

1. **Development happens on `dev`.** Not on `master`, and not on a fresh
   branch per feature.
2. **Every commit on `dev` carries a feature-specific unique prefix**, written
   as the conventional-commit scope — `feat(db2-keys): …`, `fix(db2-keys): …`.
   The scope, not a bare leading word, so the repo keeps the `feat`/`fix`/`docs`
   type it has used throughout. One prefix per feature, reused by every commit
   belonging to it, which makes a feature's commits selectable with
   `git log --grep` without reading a single diff.
3. **When the feature is ready** — complete, not merely green — `git checkout
   master` and **cherry-pick** that feature's commits, in order.
4. **Record the result below**: the `dev` hashes, the `master` hashes the
   cherry-pick produced, and the date. The two differ — a cherry-pick makes new
   commits — and that mapping is the whole reason this file exists.

Ready means the same gate as always: no half-implemented feature reaches
master. An annotation the compiler accepts but does not honour counts as
broken, however green the suite.

## Prefix registry

One row per feature. The prefix is claimed here before its first commit, so two
features cannot collide.

| Prefix | Feature | Status |
| --- | --- | --- |
| `commit-history` | this file and the workflow it records | ✅ on `dev` |
| `db2-keys` | databasev2 2 — `resident: keys` storage and readers | on `dev` (`125bd09`, `08abd09`, `0c97fa4`, `f606fc9`). **Not ready**: the loader still refuses the annotation, and updates on such a table are refused rather than implemented |

## Cherry-picks onto master

Newest first. `dev` hash is the original; `master` hash is what the cherry-pick
produced.

| Date | Prefix | Feature | `dev` → `master` |
| --- | --- | --- | --- |
| — | — | *nothing cherry-picked onto master under this convention yet* | — |

## Before this convention

Work up to 2026-08-29 landed on `master` by **merging** feature branches, so
those commits keep their original hashes and have no entry here.
`git log --merges master` is the record for that period.

The `db2-keys` commits are the seam: they were written on
`porch-store-middleware` before this convention (`18ce4d5`, `f9c36ef`,
`11a92df`, `91411ae`) and were replayed onto `dev` with prefixed titles. The
replay was verified identical, not merely applied — `git diff` between the two
branches over `database/`, `runtime/` and `docs/stories/` is empty.
