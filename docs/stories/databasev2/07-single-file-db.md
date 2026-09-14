---
track: databasev2
iteration: "7"
was_language_iteration: "33"
status: done
readiness: ready
review_pending: "fork settled 2026-09-10 for autonomous execution (nonexistent-path rule below) — developer second review before close"
---

# databasev2 7 — `WO_DATA=<path>.db`: the persistent store as one file

> **Moved 2026-08-26** from the language track, where this was iteration 33.
> Part of [Story — databasev2: the database beyond RAM](00-story.md). Content unchanged by
> the move; its dependencies are restated in that track index.

> Format: fiberloom `product/story-iteration-template`. Part of
> [Story — one language, one runtime, one database, one binary](../language-runtime-database/00-story.md)
> — the track this iteration was authored in before the 2026-08-26 move.
>
> **Inserted 2026-08-22** (developer ask: "can the persistent db be in
> file.db form?"). The truth is already almost there: `WO_DATA=<dir>`
> holds exactly ONE file (`shard-0.wal`) — the entire persistent state,
> since the log is the whole store and no data pages exist. This iteration
> makes the surface say so: point `WO_DATA` at a file and THAT file is
> the store. Small, driver-only, independent of the concurrency chain.

## Goals

- **`WO_DATA=<path>` accepts a file path.** A path that is not an
  existing directory is treated as THE wal file (`app.db`,
  `store.wo.db` — the name is the operator's). The directory form stays
  and keeps meaning `<dir>/shard-0.wal` — every existing deployment and
  gate is byte-identical.
- **One file remains the whole truth at any core count** — stage 3 made
  the WAL owner-shard-only (shard 0 is the sole writer), so nothing
  multi-shard ever adds a second file.
- **The contract says it out loud**: `04-db-binding.md` documents the
  file form, including that compaction (databasev2 3, landed 2026-08-29) and
  schema migration (databasev2 12) both rewrite the log through a temp file +
  `rename` beside it and fsync the parent directory — so the file form must
  derive that directory from the file path for both.
- **`WO_DATA` is always a path.** Ephemerality is a separate variable —
  `WO_EPHEMERAL=1`, decided with [databasev2 2](02-table-storage-modes.md)'s
  task 6a on 2026-09-09 — so the file-vs-directory parse here never
  special-cases a sentinel such as `:memory:`. Both variables set together is
  already a startup refusal there; the file form inherits it unchanged.

## Acceptance Criteria

Met:

- **Given** `WO_DATA=/tmp/app.db` (no such directory), **when** a
  program seeds, restarts, and verifies, **then** replay is byte-true
  and `/tmp/app.db` is the only artifact on disk. ✅ task 1 smoke A
  (`b31bd40`): seed creates the file, restart+`order` prints the same
  lines as the directory form, `ls` shows `app.db` as the only artifact.
  Confirmed at gate level by `residency-accept.sh` section 8 check (i)
  (`aaea6b2`): `mix.wob` seed → restart prints `kept=1 scratch=0`;
  `find f8/a -mindepth 1 -printf %P` == `app.db`.
- **Given** `WO_DATA=<dir>` (existing directory), **when** the same
  program runs, **then** behavior is byte-identical to today —
  `<dir>/shard-0.wal`, every standing gate unchanged. ✅ task 1 smoke D
  (`$W/d1` and `$W/d2/` both produce `shard-0.wal` inside the dir,
  unchanged form); section 8 check (iv) pins the same as a regression.
- **Given** the db-bench durability legs pointed at the file form,
  **when** the restart proof and kill -9 battery run, **then** every
  guarantee holds identically (the file IS the same WAL, only named by
  the operator). ✅ `./scripts/db-bench.py --quick --wo-data-file`:
  **`db-bench: 181 checks, 5 failures`** — the same 5 as the directory
  form (`residency.keys.fit` rc 74, not this defect; see databasev2 13);
  `restart.s1.file`, `crash.s1.0.file`, `restart.sN.file`,
  `crash.sN.0.file` all ok. `residency-accept.sh` section 8 check (vi):
  inline `onefile.wo` kill -9 battery, N=1e6, killed at 0.20–0.49s,
  asserts rc 137 (the kill landed) before verifying — landed after
  45781 acks on tmpfs, all replayed.
- **Given** `WO_DATA=/tmp/app.db` where `/tmp/app.db` does not exist and
  `/tmp` does, **when** the program starts, **then** the file is created
  and used as the log; **given** `WO_DATA=/tmp/app.db/` (trailing slash)
  or an existing directory, **then** the directory form applies; **given**
  a path whose parent directory does not exist, **then** startup refuses
  with exit 2 naming the path and the parent — never a silent `mkdir -p`.
  ✅ task 1 smoke B (missing parent: rc 2, `its parent <path>/nope is not
  an existing directory; create it first`) and C (fifo: rc 2, `neither a
  regular file nor a directory`); section 8 checks (ii) missing parent
  (rc 2, message matches `its parent .*/nope is not an existing
  directory` + `nope/app.db`, asserts `! -e nope`) and (iii) fifo (rc 2
  under `timeout 10`, so a wrong resolution blocking on the fifo also
  fails); section 8 check (iv) `nodir/` (trailing slash, missing dir) →
  rc 2 `cannot open .*/nodir//shard-0.wal`, byte-identical to pre-7.
- **Given** the file form, **when** compaction (databasev2 3) or a schema
  migration (databasev2 12) rewrites the log, **then** the temp file is
  `<file><suffix>` beside it and the fsync'd parent is the file's own
  directory — pinned by a test, since both paths today derive from the
  log path rather than from `WO_DATA`. ✅ `test_file_form_temps_beside_log`
  (`ccee2d0`, `test_wal: 6629 pass, 0 fail`, +57 assertions), teeth proven
  by a mutation control (compaction temp redirected into a decoy dir):
  14 assertions turn red. Gate-level: section 8 check (vii)
  (`WO_CHECKPOINT_BYTES=1 WO_WAL_STATS=1 wal 300`): `compactions=` >= 1
  asserted (5 measured), `app.db` still the only artifact, `verify 300`
  rc 0.

## Out Of Scope

- A paged database file (SQLite's shape) — still rejected; the
  disk story is the WAL, full stop.
- Checkpoint/compaction — [iteration 32](03-wal-checkpoint.md)'s; its
  rename-swap (write snapshot+tail to a NEW file, fsync, `rename()`
  over the old) is exactly what keeps the single-file promise crash-safe
  when it lands. 33 before or after 32 works; landing 33 first means
  32's spec inherits the file form as a stated constraint. *(2026-09-10:
  32 is databasev2 3, landed 2026-08-29; this iteration lands after it
  and pins the constraint with a test — see Progress task 2.)*
- Multiple stores per process, attach-by-file — held iteration 20's
  territory.

## Info

- The whole change is `runtime/src/main.c`'s hardcoded
  `snprintf("%s/shard-0.wal", dir)` growing a stat-based fork
  (directory → today's path; otherwise → the path itself), plus a gate
  check and the binding-doc note. No engine, no WAL format, no
  compiler.
- Fork for the (tiny) spec: what does a NONEXISTENT path mean? Leaning:
  a path whose parent exists and that does not end in `/` is a file to
  create; a trailing `/` or existing directory keeps the directory
  form. Refusing ambiguity loudly (WO exit 2) beats guessing.
  **Settled 2026-09-10 (auto-approved for autonomous execution,
  `review_pending`): the leaning IS the rule.** Existing directory or
  trailing `/` → `<dir>/shard-0.wal` (byte-identical to today). Otherwise
  the path is the log file: created if absent, opened if a regular file.
  A parent directory that does not exist → startup refusal, exit 2, one
  stderr line naming the path and the missing parent (no `mkdir -p`: a
  typo must not create a store somewhere unexpected). A path that exists
  but is neither a regular file nor a directory (fifo, socket, device) →
  the same refusal shape. `WO_EPHEMERAL` interplay is inherited from
  databasev2 2 task 6a unchanged. Compaction and migration already build
  their temp as `<log path><suffix>` and sync the log's parent directory
  (`database/src/wal.c` `sync_parent_dir`), so the file form inherits
  crash safety without engine changes; a test pins that.

## Progress

| # | Task | State |
| --- | --- | --- |
| 1 | `runtime/src/main.c`: stat-based resolution of `WO_DATA` (dir / trailing slash → `shard-0.wal`; else the file), the two refusals (missing parent; not a regular file or directory), message names path and parent | ✅ `b31bd40` |
| 2 | `runtime/test`: pin that compaction and migration temps land beside an arbitrary log path and the parent of THAT path is synced (file-form path with a sibling directory as decoy) | ✅ `ccee2d0` |
| 3 | Contract + memory: `docs/plan/oop-vm/04-db-binding.md` file form; `database/src/CODE-LOGIC.md` or `runtime/src/CODE-LOGIC.md` note on path resolution | ✅ `f1985ba` |
| 4 | Gate: `scripts/residency-accept.sh` or a db-bench durability leg pointed at the file form — seed, restart, kill -9 battery, only-artifact check; refusal checks for the missing parent (codd-cyril) | ✅ `e274f4a` + `aaea6b2` |

## Proposed Solution

Small enough for a bounded slice: brainstorm the one fork in chat,
implement driver + db-bench file-form leg + docs in one pass, gates
green. No plan document needed unless it grows.

## History

- 2026-09-10 tasks 1–3 landed; 4 in flight.
- 2026-09-10 closed: task 4 landed, residency 32/0.
