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
| `db2-keys` | databasev2 2 — `resident: keys` storage and readers | ✅ on `master` 2026-08-30 (with `db2-delta` and `db2-chains`). The “Not ready” note this row carried is spent: the loader refusal was lifted and updates are implemented |
| `db2-chain-review` | review of the databasev2 chain and dependency graph | ✅ on `master` 2026-08-30 |
| `db2-delta` | databasev2 2 — keys-resident updates as WAL delta records | ✅ on `master` 2026-08-30 |
| `db2-chains` / `db2-chain` | databasev2 11 — bounding a keys-resident row's delta chain | ✅ on `master` 2026-08-30 |
| `site` | the writeonce.de tutorial site | ✅ on `master` 2026-08-30 |
| `db2-migrate` | databasev2 12 — schema migrations (add/delete, declarative, auto on boot) | ✅ on `master` 2026-08-31 |
| `site-deploy` | the writeonce.de redeploy runbook (`docs/guides/deploying-site.md`) | ✅ on `master` 2026-08-31, picked as iteration 12's docs dependency |
| `site-update` | the developer loop for changing the site app (`docs/guides/updating-site.md`) | on `dev` 2026-08-31 |
| `site-submodule` | `docs/examples/site` extracted to github.com/shoneyJ/writeonce-site and consumed as a submodule | ✅ on `master` 2026-08-30. Both branches now track the site by revision; an edit to it is a commit in that repo plus a pointer bump here |
| `lang41` | runtime: unadopted shard must not impersonate shard 0 | on `dev` (`9dca0b4`); independent of the residency stack, not picked |
| `porch-store` | porch store tables, Limiter and Idempotent middleware (Phases A, B, C) | on `dev` (`519d411`, `5b1e82a`, `aee7926`). **In progress**: Phase C was uncommitted work from a parallel session, committed as-is, and calls `json.decode`/`json.encode` with no `use json` import |
| `query-corpus` | databasev2 query-grammar corpus #1 | on `dev` (`4c82461`). Conclusion was "no new grammar needed" |
| `lang42` | iteration 42 — bounded subprocess: `proc.run` bounded + parked (pidfd, caps, ceiling, owner-bound reaping), `proc.run_dl`; carries the alacritty/tmux/zen parity studies and the porch dependency-graph section from the same sweep | ✅ on `master` 2026-09-01 |
| `wmux` | the wmux track (`docs/stories/wmux/`, iteration 1 was language 43) — the terminal multiplexer, first of the softwares built with writeonce; story + gap-chain remap first, code follows gap by gap | on `dev` 2026-09-01 |
| `rt2` | the runtime-v2 track (`docs/stories/runtime-v2/`) — the runtime beyond sockets: streaming subprocess, PTY, signals-as-events, termios, fd passing, term.size/width; six iterations, all landed 2026-09-02 | on `dev` 2026-09-01 |
| `wmux` (code) | wmux rung 1 — the multiplexer example (`docs/examples/wmux`) + `just wmux` gate; sessions, attach by fd-handover, durable scrollback, restart replay | on `dev` 2026-09-02 (extends the `wmux` docs prefix) |
| `db2-7` | databasev2 7 — single-file store `WO_DATA=<path>.db` (registered after its first commit, `b31bd40`) | on `dev`, closed 2026-09-10 |
| `lang-18` | language 18 — `transaction { }` over the WAL's staged batch (registered after its first commit, `6b4b960`) | on `dev`, in progress since 2026-09-11 |
| `db2-ephemeral` | databasev2 2 task 6a — refuse `durable: true` without `WO_DATA`, `WO_EPHEMERAL=1` escape hatch, `.wob` v8 table bit (`WO_CLASSF_TABLE`); closes iteration 2 | on `dev` 2026-09-15 |
| `db2-4b` | databasev2 4 part B — the async barrier, re-brainstormed 2026-09-10 (docs only until the fold lands) | on `dev` 2026-09-15 |
| `db2-5` | databasev2 5 — bounded tables and eviction, the resident byte budget as Phase A; brainstormed to `ready` 2026-09-10 | on `dev` 2026-09-15 (docs) |
| `db2-14` | databasev2 14 — the shop workload story (`refine`) | on `dev` 2026-09-15 (docs) |
| `agents` | `.claude/agents` persona roster — codd/fielding/ada families, `lintor`, the README | on `dev` 2026-09-15 |
| `status` | cross-track reconciliation sweeps of the board, dependency graph and story tables (in use since `732c221`) | on `dev` |
| `tls` / `crypto` / `rv2-tls` / `rv2-aead` / `net` | runtime-v2 8 (the AEADs) and 9 (in-process TLS 1.3, both directions): `net.connect` (id 110), `net.connect_tls`/`read_tls`/`write_tls`, `net.accept_tls`, RSA-PSS + ECDSA-P256 signing, PEM/DER parsing; `just tls`, `just tls-server` | ✅ on `master` 2026-09-15 (registered after the fact) |
| `porch2-rng` | porch 2 phase A — `random_bytes` builtin (id 119) | on `dev` — not picked 2026-09-15: porch 2 is `in-progress` |
| `jarvis`, `rv2-obs`, `porch-cookies`/`-csrf`/`-routing`/`-streaming`/`-sse`/`-static`, `audit`, `workflow`, `runtime` (docs) | docs-only prefixes: the jarvis stories, rv2 7 brainstorm, the porch 2–8 brainstorms, the doc audit, the prebuild-feature workflow, the TLS CODE-LOGIC | ✅ on `master` 2026-09-15 |
| `gate`, `vm`, `arena`, `compiler`, `runtime` (fix) | one-off fixes: `e274f4a` + `ec797d9` (gates), `63065ff` (lang 41 double free), `78ae3be` (lang 44 poison-on-free), `2d54710` (lang-41 side defects), `35efa21` (poisoned class NULL fmap) | ✅ on `master` 2026-09-15 |

## Cherry-picks onto master

Newest first. `dev` hash is the original; `master` hash is what the cherry-pick
produced.

| Date | Prefix | Feature | `dev` → `master` |
| --- | --- | --- | --- |
| 2026-09-15 | `porch-store`, `rt2`, `tls`+`crypto`+`rv2-*`+`net`, `lang41` + one-off fixes, `db2-7`, `db2-keys` (13), `db2-ephemeral`, `db2-chains`, `query-corpus`, `agents`, the docs prefixes | **the 2026-09-01 → 09-15 `dev` catch-up, minus three unfinished features**: 129 commits picked in `dev` order (127 in the sweep, plus `e9213bb` and `1ce195d` — two fixes the verification on `master` forced: `woc build -o` failing on a fresh checkout, and the web-app keypool leg refused since 6a — committed on `dev` first, then picked) with `-x` (each `master` commit names its `dev` source), mapped per prefix below. **Left on `dev` on purpose:** the **wmux** track (59 commits — rungs 10/12/13/15/16/18 are `in-progress` and share the prefix with the done rungs), **language 18** `transaction { }` (10 commits — T7's durability legs open, criterion 1 outstanding), **porch 2** phase A `random_bytes` (2 commits — the iteration is `in-progress`). Conflicts: two `justfile` hunks (kept `tls`/`tls-server`, dropped the `wmux` recipe), `scripts/wmux-accept.sh` dropped from `4553ca1`, seven markdown files taken from the picked commit; three master-only follow-ups in `69114ab`. Verified on `master` after a fresh build in its own worktree: woc-test clean; `make -C runtime test` 21 suites 0 fail (test_wal 6660/0, test_tls 123/0, test_crypto 130/0, test_loader 36/0), `test-iso` 21 suites 0 fail, cli_smoke OK; `just oop-e2e` **127/0** (single-binary smoke 4/0 — the new `build-into-missing-dir` check), `just residency` **32/0**, `just db-actor` **10/0** (from a deleted target/), `just web-app` **56/0**, `just chat` **11/0**, `just subprocess` **12/0**, `just tls` **5/0**, `just tls-server` **5/0**, `just deps-accept` **8/0**, `just db-bench-quick` **185 checks, 0 failures**. `just fibers` 10 checks / **1 failure — the KNOWN TSan race in `wo_engine_stop` (vm.c:719)**, red on `dev` the same way (codd.md "Next bugs"), not a pick regression. Not run: `just site` (submodule not initialised in the worktree), `just wmux` (track not picked). | see the sub-table below; `69114ab` is master-only |
| 2026-09-01 | `lang42` | **iteration 42 — bounded subprocess**: `proc.run` parked (pidfd + epoll bundle, `_dl` retry mould) with deadline/output-cap/ceiling refusals by name and owner-bound reaping; `proc.run_dl` (id 96) states bounds per call; the pre-42 sequential-drain deadlock proven then dissolved. Includes the alacritty/tmux/zen-browser parity studies and the porch graph section. Zero conflicts. Verified on `master` after rebuild: 38 runtime suites 0 fail both dispatch flavors (`test_proc` 128/0, `test_wal` 5966/0), woc-test 557/0 (forced, not cached), subprocess-accept 12/0, site-accept 23/0 | `5b92e20` → `2f6d39d`, `75fbd30` → `b287bf7`, `821899b` → `afa16e5`, `c30507b` → `81c28d8`, `975959a` → `64542e5`, `a3b5dc3` → `ce98fa1`, `b147dd4` → `346f885`, `5dfbeda` → `49b0193` |
| 2026-08-31 | `db2-migrate` + `site-deploy` | **databasev2 12 — schema migrations v1**: WO_WAL_SCHEMA head record, name-keyed boot diff, record-level transcode for add/delete, poisons that bite only with records; plus the redeploy runbook the close-out edits (dev-only until now). Zero conflicts. Verified on `master`: 36 suites 0 fail (`test_wal` 5966/0), woc-test clean, residency-accept 14/0, site-accept 23/0 | `930a715` → `b594717`, `072e007` → `8d9207d`, `ba8519f` → `570e0d6`, `63a063b` → `b1b7984`, `b69092a` → `4a70fc7`, `b21943a` → `ace5699`, `4bb6ece` → `4f1fda1` |
| 2026-08-30 | `site-submodule` | **`docs/examples/site` becomes a submodule** — extracted to github.com/shoneyJ/writeonce-site with `git subtree split` (its own 9 commits of history, not a snapshot) | `4b56348` → `a5497a3`, `4eead89` → `565b894` |
| 2026-08-30 | `db2-keys` + `db2-delta` + `db2-chains` + `site` | **databasev2 `resident: keys`, end to end** — storage, readers, deletes, updates as delta records, bounded delta chains, and the tutorial chapter documenting them | 37 commits, mapped one-to-one below |

### 2026-09-15 — the `dev` catch-up

Picked in `dev` order onto `master` in a separate worktree (`git worktree add`), each with `cherry-pick -x`, so this table can be regenerated from `git log master` (`cherry picked from commit …` trailers). One row per prefix, pairs in `dev` order.

| Prefix | n | `dev` → `master` |
| --- | --- | --- |
| `porch-store` | 26 | `519d411` → `ddc8b99`, `5b1e82a` → `8eb36a9`, `aee7926` → `3e6eab7`, `fc09e94` → `3828c76`, `5c3544d` → `7061646`, `f079455` → `06b7722`, `a96ebe2` → `4a22da6`, `3a9bddc` → `0d98a52`, `676e651` → `9fb0cff`, `77e06c1` → `9190507`, `153fd29` → `eb8e019`, `a653dd0` → `bf69f82`, `831e9d8` → `569abef`, `eae1b06` → `75965b5`, `e61015f` → `0542cda`, `464147a` → `2d47671`, `9ad5947` → `f27fe3b`, `21934b1` → `e4d7922`, `2ac1b8b` → `360ca47`, `91099cf` → `b641c37`, `b738269` → `d4e5cce`, `c53ad58` → `f991f48`, `86e7244` → `47e5acf`, `6d48dbc` → `8904be8`, `a919ab1` → `23e5b0f`, `79e6da4` → `6d1288b` |
| `query-corpus` | 1 | `4c82461` → `2b70306` |
| `commit-history` | 5 | `bc8fec0` → `a95f58d`, `aa8abfb` → `bb7e1a1`, `22b5675` → `aa5f3da`, `ab7df69` → `d9d9632`, `d0e658f` → `c3c5d67` |
| `lang41` | 1 | `9dca0b4` → `6360088` |
| `site-update` | 1 | `a21a02f` → `d5da3ac` |
| `rt2` | 9 | `e0451cb` → `6ae251f`, `d313cde` → `0be01b7`, `9be87f1` → `b79597e`, `9836c9c` → `7485c66`, `14e03a6` → `055cb70`, `b439387` → `5340ef2`, `1d68902` → `3605e11`, `bc1b4f0` → `5eef0fc`, `1514fb4` → `1e5d81f` |
| `runtime` | 2 | `35efa21` → `784cd25`, `5670304` → `6c22cd3` |
| `porch-cookies` | 1 | `4d31d53` → `602daa6` |
| `porch-csrf` | 1 | `3a4fb42` → `c103df7` |
| `porch-routing` | 1 | `0589a13` → `5a04513` |
| `porch-streaming` | 1 | `1520540` → `d37c583` |
| `porch-sse` | 1 | `07f5357` → `699f811` |
| `porch-static` | 1 | `9801fce` → `a509656` |
| `net` | 1 | `13c6f12` → `92ac803` |
| `(no scope)` | 1 | `203470c` → `83335cf` |
| `rv2-tls` | 13 | `f1881cc` → `69c6822`, `e24b8ec` → `4f8a0ae`, `b929a20` → `8f4fbd2`, `ae42943` → `0f0cc60`, `3eab98c` → `7f0b189`, `796ed88` → `7eb0708`, `d49bc38` → `836c09f`, `8b6e721` → `8649d59`, `9662cd8` → `5b70ac1`, `9fcb4a9` → `3787a14`, `f02518c` → `a3f3dbd`, `57613bd` → `1f3358c`, `f3a3c96` → `f1f11c3` |
| `rv2-aead` | 4 | `c8a5a31` → `cb92908`, `db5bdf3` → `a15dfa0`, `249b1db` → `b83832c`, `138de17` → `be35434` |
| `crypto` | 12 | `961854a` → `74f3370`, `f12a745` → `74db22b`, `dccf650` → `411e8cc`, `c8d27b6` → `3fa0445`, `f41b1c5` → `579130a`, `9118177` → `781589d`, `92c996b` → `d502866`, `4ec1c75` → `4da6ab7`, `cf8fdfc` → `2181d6d`, `1bc6d04` → `e17986c`, `819d672` → `f7aebb2`, `fba3035` → `fb34da7` |
| `jarvis` | 2 | `a615ee8` → `f862dc2`, `8e160c3` → `a81135e` |
| `tls` | 15 | `5021a99` → `74d66ec`, `417fcc1` → `c2eb996`, `541c71b` → `2617bf4`, `afd9f23` → `de3984c`, `74c332d` → `ba34017`, `319ce8b` → `f9ed841`, `9d40055` → `2c0dd55`, `3811418` → `ab07609`, `6445d55` → `07c6dee`, `9a922b3` → `7ab9e3d`, `3d8bb14` → `398fbcc`, `34d2b8f` → `05d4d08`, `2d4c300` → `989fcdd`, `54020a4` → `a804ad4`, `ac3bf74` → `db6e414` |
| `rv2-tls,jarvis` | 1 | `4fdf071` → `2b33589` |
| `rv2-tls,status` | 1 | `ad87974` → `104e805` |
| `workflow` | 1 | `2bfbb0c` → `fff86d3` |
| `rv2-tls,jarvis,status` | 1 | `732c221` → `a4d4b34` |
| `vm` | 1 | `63065ff` → `6948cd2` |
| `audit` | 1 | `f1049dd` → `ba23483` |
| `arena` | 1 | `78ae3be` → `cddda8c` |
| `rv2-obs` | 1 | `feb11c3` → `3b97569` |
| `compiler` | 2 | `2d54710` → `35331ac`, `e9213bb` → `23504ee` |
| `db2-7` | 5 | `b31bd40` → `92bf6de`, `ccee2d0` → `5739a6c`, `f1985ba` → `39da4b4`, `aaea6b2` → `7200406`, `38f4f1e` → `8d48cac` |
| `gate` | 3 | `e274f4a` → `65745e6`, `ec797d9` → `3c1161a`, `1ce195d` → `896516c` |
| `db2-keys` | 3 | `6310078` → `30ea9eb`, `1b6750d` → `2019364`, `b5b1da7` → `5a6c702` |
| `db2-ephemeral` | 3 | `863692a` → `0e2eee6`, `4553ca1` → `719f7f3`, `2c35319` → `561bb2a` |
| `db2-chains` | 1 | `d841390` → `47ae475` |
| `agents` | 1 | `830bbb1` → `a12a0ec` |
| `db2-4b` | 1 | `7ceb7b8` → `07e368c` |
| `db2-5` | 1 | `579199a` → `6eddd8c` |
| `db2-14` | 1 | `f33ae98` → `2ed50f2` |
| `status` | 1 | `423b3c1` → `c41ed17` |

**What the pick taught.**

- **"Already on master" is the mapping table, never a prose mention.** `79e6da4`
  (porch 1 re-scoped to the limiter, idempotency reverted to porch 9) was named
  in the 2026-08-30 prose and had never been picked, so `master` still carried
  the reverted middleware and 638 lines of gate legs for it. Filtering the pick
  list on "hash appears anywhere in this file" skipped it; the trailing
  `git diff --name-only dev` minus the excluded commits' footprint caught it.
  `git cherry master dev` answers patch-equivalence; this table answers
  "picked with conflicts".
- **Earlier conflict-resolved picks had dropped hunks**: `89a7456` lost
  `b3d8c40`'s skill-catalog README link fix, the porch-store pick lost the
  porch 9 story file. Both restored by `69114ab`.
- **`ec797d9` (executable bit) was a no-op until `79e6da4` reset the mode**, so
  it is picked after it (`3c1161a`), out of `dev` order.
- **Excluding a track leaves its documentation dangling.** The board and graph
  on `master` describe wmux and language 18 as the project's state (they are),
  so `just linkcheck` on `master` reports the wmux story and spec links as
  broken until that track is picked; `.dev/reference` links break in any
  checkout without the developer-local symlinks and are not defects.
- **Proof of equality:** re-applying the 70 excluded commits onto `master` in a
  scratch branch reproduces `dev` in every code path except the two files
  below — so `master` is exactly `dev` minus wmux, language 18 and porch 2.

**Obligations when wmux is picked:** re-add the `wmux:` recipe to the
`justfile` (dropped in both `justfile` conflicts), and re-apply the
`WO_EPHEMERAL=1` edits to `scripts/wmux-accept.sh` from `dev`'s `4553ca1`
(the client legs refuse without them since databasev2 2 task 6a).

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
