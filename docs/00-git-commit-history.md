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
| `commit-history` | this file and the workflow it records | ✅ on `master` 2026-08-30 |
| `db2-keys` | databasev2 2 — `resident: keys` storage and readers | ✅ on `master` 2026-08-30. The “not ready” note this row used to carry is spent: `db2-delta` lifted the loader refusal and implemented updates |
| `db2-chain-review` | review of the databasev2 chain and dependency graph | ✅ on `master` 2026-08-30 |
| `db2-delta` | databasev2 2 — keys-resident updates as WAL delta records | ✅ on `master` 2026-08-30 |
| `db2-chains` / `db2-chain` | databasev2 11 — bounding a keys-resident row's delta chain | ✅ on `master` 2026-08-30 |
| `site` | the writeonce.de tutorial site | ✅ on `master` 2026-08-30 |
| `site-submodule` | `docs/examples/site` extracted to github.com/shoneyJ/writeonce-site and consumed as a submodule | ✅ on `master` 2026-08-30. Both branches now track the site by revision; an edit to it is a commit in that repo plus a pointer bump here |
| `lang41` | runtime: unadopted shard must not impersonate shard 0 | on `dev` (`9dca0b4`); independent of the residency stack, not picked |
| `porch-store` | porch store tables, Limiter and Idempotent middleware (Phases A, B, C) | on `dev` (`519d411`, `5b1e82a`, `aee7926`). **In progress**: Phase C was uncommitted work from a parallel session, committed as-is, and calls `json.decode`/`json.encode` with no `use json` import |
| `query-corpus` | databasev2 query-grammar corpus #1 | on `dev` (`4c82461`). Conclusion was "no new grammar needed" |

## Cherry-picks onto master

Newest first. `dev` hash is the original; `master` hash is what the cherry-pick
produced.

| Date | Prefix | Feature | `dev` → `master` |
| --- | --- | --- | --- |
| 2026-08-30 | `db2-keys` + `db2-delta` + `db2-chains` + `site` | **databasev2 `resident: keys`, end to end** — storage, readers, deletes, updates as delta records, bounded delta chains, and the tutorial chapter documenting them | 37 commits, mapped one-to-one below |

### 2026-08-30 — the databasev2 residency stack

The first cherry-pick under this convention, and it could not be a single
iteration: **databasev2 11 (bounded delta chains) does not stand alone.** Its
commits touch `wo_wal_fold_row_at`, `keys_fold_into` and `row_apply_field_keys`,
none of which existed on `master` — so the whole stack it sits on came with it,
in dev order:

| # | Prefix | `dev` | `master` | Title |
| --- | --- | --- | --- | --- |
| 1 | `db2-keys` | `125bd09` | `620c0a7` | feat(db2-keys): storage — drop the payload, read it back from the log |
| 2 | `db2-keys` | `08abd09` | `d985901` | feat(db2-keys): inserts and boot — payload dropped after the barrier |
| 3 | `db2-keys` | `0c97fa4` | `d8839c0` | feat(db2-keys): the query paths read through the iterator and borrow |
| 4 | `db2-keys` | `f606fc9` | `234b1f0` | feat(db2-keys): rewire remaining readers, survive compaction |
| 5 | `db2-keys` | `b3d8c40` | `89a7456` | docs(db2-keys): reconcile databasev2 and porch markdown with the code |
| 6 | `db2-chain-review` | `2ecaf0c` | `1af4910` | docs(db2-chain-review): review the databasev2 chain and dependency graph |
| 7 | `db2-keys` | `76b8fd9` | `390635c` | fix(db2-keys): delete on a keys-resident table was memory corruption |
| 8 | `db2-keys` | `c9c7e03` | `d27e774` | docs(db2-keys): a runnable example for per-table storage |
| 9 | `db2-keys` | `dc25462` | `c8a0c7b` | fix(db2-keys): a logged delete must replay on a keys-resident table |
| 10 | `db2-keys` | `d4104dc` | `4105f1c` | docs(db2-keys): the residency example becomes a product catalogue |
| 11 | `db2-keys` | `c9a88b0` | `daba10c` | docs(db2-keys): spec — delta records for keys-resident updates |
| 12 | `db2-delta` | `abb8fc9` | `b5cc77d` | docs(db2-delta): implementation plan for keys-resident delta updates |
| 13 | `db2-delta` | `c6cd486` | `efa118b` | docs(db2-delta): correct a line citation before execution |
| 14 | `db2-delta` | `9c6f832` | `ff73da7` | feat(db2-delta): WAL delta record kind and encoder |
| 15 | `db2-delta` | `20ba096` | `82dbd4a` | fix(db2-delta): make delta test detect a field_idx/back_off transposition |
| 16 | `db2-delta` | `a60231c` | `1f04cff` | feat(db2-delta): fold a delta chain, route reads through it |
| 17 | `db2-delta` | `173dbf2` | `38159b0` | fix(db2-delta): fold's cycle guard checks direction, not step count |
| 18 | `db2-delta` | `89c56a1` | `5b9ffb7` | feat(db2-delta): keys-resident updates append, indexes follow |
| 19 | `db2-delta` | `409186d` | `f1f4d13` | fix(db2-delta): unique shadow-check gets its own buffer, not r's |
| 20 | `db2-delta` | `4d13bce` | `dbfa385` | feat(db2-delta): wire the request path, defer re-point to the barrier |
| 21 | `db2-delta` | `c049ab9` | `d6eeacb` | fix(db2-delta): close the unique-shadow-check's same-drain blind spot |
| 22 | `db2-delta` | `7e4ae70` | `ef606e1` | feat(db2-delta): replay and compaction fold delta chains |
| 23 | `db2-delta` | `b87c68f` | `8dbeb2a` | feat(db2-delta): lift the resident:keys refusal, prove it end to end |
| 24 | `db2-delta` | `3ea6d64` | `76a9f17` | fix(db2-delta): refuse resident:keys with no WO_DATA at runtime |
| 25 | `db2-delta` | `d4b12d1` | `4ae3af2` | fix(db2-delta): borrow the pending re-point, not the stale durable offset |
| 26 | `db2-delta` | `fed9fe8` | `b8e4bc9` | fix(db2-delta): pend_repoint failure fatal; delta fold no longer trusts a live WAL |
| 27 | `db2-delta` | `b575678` | `bdedc50` | docs(db2-delta): resident:keys has storage; move done criteria to Met |
| 28 | `db2-delta` | `e643440` | `35aa0be` | docs(db2-delta): guide to log-structured rows for a new reader |
| 29 | `db2-chains` | `f667cad` | `0c874a6` | docs(db2-chains): spec + story for bounding a row's delta chain |
| 30 | `db2-keys` | `7cba9b1` | `ab292a4` | feat(db2-keys): task 7 — measure resident: keys against swapping |
| 31 | `db2-keys` | `abc276a` | `152b5ea` | feat(db2-keys): GB-scale bench modes, unmeasured |
| 32 | `db2-keys` | `a310496` | `3855e58` | feat(db2-keys): gate the residency measurement, close out task 7 |
| 33 | `db2-chains` | `1b808ab` | `e3c544c` | feat(db2-chains): bound a keys-resident row's delta chain |
| 34 | `db2-chain` | `f93b5d9` | `b375772` | test(db2-chain): cover flattening, and drop a ceiling no input could reach |
| 35 | `db2-chain` | `de39a88` | `5a98730` | docs(db2-chain): close out iteration 11 on the board |
| 36 | `site` | `3b503c0` | `461ba18` | feat(site): tutorial chapter for durable and resident storage modes |
| 37 | `commit-history` | `41923eb` | `397a2b6` | docs(commit-history): feature-to-cherry-pick reference |

**What was deliberately left on `dev`:** the 26 `porch-store` commits. porch 1
was re-scoped mid-flight (`79e6da4` reverts idempotency to porch 9), so it is
the exact case this file's “ready means complete, not merely green” bar exists to
catch. `lang41` and `query-corpus` also stayed — independent features, not
dependencies of this one.

**Three conflicts, all in docs, all resolved toward what `master` can honestly
claim:**

- `docs/examples/skill-catalog/README.md` — a one-line link fix inside a file
  belonging to `query-corpus`, which is not on `master`. Edit dropped; the file
  stays absent.
- `docs/00-databasev2-chain-review.md` — created by `db2-chain-review`, which the
  prefix filter had excluded while later `db2-keys` commits kept editing it. Resolved
  by picking that commit too, rather than dropping edit after edit.
- `docs/stories/00-status.md` — `b87c68f` carried one databasev2 status entry
  bundled with two porch-1 entries. **Only the databasev2 entry was kept.** Taking
  the whole block would have left `master` claiming porch 1 was done while none
  of its code was there.

**Verified on `master` after the pick, not assumed:** `woc-test` clean; `wovm-test`
all 20 suites green (`test_wal` 5700/0, `test_table` 856/0); `just site` 23 checks,
0 failures; `residency-accept` 14 checks, 0 failures — including the leg proving
`resident: keys` without `WO_DATA` exits 2 and names the offending class.

**Still outstanding on `master`, and known:** databasev2 2 task 6's byte-budget
refusal. A missing *guard*, not an unhonoured annotation — the annotation is now
genuinely honoured, measured at a 2.55× smaller resident set. The other half of
task 6 (`durable: true` with no `WO_DATA` silently discarding writes) predates this
pick and is unchanged by it.


## Before this convention

Work up to 2026-08-29 landed on `master` by **merging** feature branches, so
those commits keep their original hashes and have no entry here.
`git log --merges master` is the record for that period.

The `db2-keys` commits are the seam: they were written on
`porch-store-middleware` before this convention (`18ce4d5`, `f9c36ef`,
`11a92df`, `91411ae`) and were replayed onto `dev` with prefixed titles. The
replay was verified identical, not merely applied — `git diff` between the two
branches over `database/`, `runtime/` and `docs/stories/` is empty.
