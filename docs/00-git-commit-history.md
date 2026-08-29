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
| `porch-store` | porch store tables, Limiter and Idempotent middleware (Phases A, B, C) | on `dev` (`519d411`, `5b1e82a`, `aee7926`). **In progress**: Phase C was uncommitted work from a parallel session, committed as-is, and calls `json.decode`/`json.encode` with no `use json` import |
| `query-corpus` | databasev2 query-grammar corpus #1 | on `dev` (`4c82461`). Conclusion was "no new grammar needed" |

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

The `db2-keys` and `porch-store` commits are the seam: they were written on
`porch-store-middleware` before this convention (`18ce4d5`, `f9c36ef`,
`11a92df`, `91411ae`, `6c8550a`, `01af1b9`) and were replayed onto `dev` with
prefixed titles. The replay was verified identical, not merely applied — after
it, `git diff porch-store-middleware dev` over the whole tree was empty.

On 2026-08-29 every other branch was consolidated so only `dev` and `master`
remain. Three could not be replayed and were preserved as **annotated tags**
instead — nothing is lost, and each tag's message says why:

| Tag | Why it is not on `dev` |
| --- | --- |
| `archive/cleanup-pre-existing-changes` | Aug 10, based on an Aug 8 commit. Carries `crates/` and `Cargo.toml` — the Rust runtime `master` has since deleted entirely. Replaying it would resurrect it. |
| `archive/ipc-attach` | Iteration 9c attach channel. Refactors `wo_row_insert`/`wo_row_update_field` into engine-encoded cores; `dev` rewrote those same functions for `db2-keys`. Two overlapping refactors of one function, ~250 conflicted lines. |
| `archive/keypair-auth` | Iteration 9d, builds on 9c — blocked by the same overlap. |

The 9c/9d hazard is specific and worth stating: that branch's contract
transfers ownership of `vals` **on failure as well as success**, while `dev`'s
keys-resident arm returns early *without* freeing. A merge that compiles and
passes could still leak or double-free. Reconciling them is an integration
task, not a conflict resolution — recover the work with
`git checkout -b <name> archive/ipc-attach` when it is scheduled.
