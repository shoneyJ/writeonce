---
iteration: "33"
status: refine
---

# Iteration 33 — `WO_DATA=<path>.db`: the persistent store as one file

> Format: fiberloom `product/story-iteration-template`. Part of
> [Story — one language, one runtime, one database, one binary](00-story.md).
>
> **Inserted 2026-08-22** (developer ask: "can the persistent db be in
> file.db form?"). The truth is already almost there: `WO_DATA=<dir>`
> holds exactly ONE file (`shard-0.wal`) — the entire persistent state,
> since RAM is authoritative and no data pages exist. This iteration
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
  file form, and documents honestly that the file is an append-only log
  that grows until iteration 32 lands.

## Acceptance Criteria

- **Given** `WO_DATA=/tmp/app.db` (no such directory), **when** a
  program seeds, restarts, and verifies, **then** replay is byte-true
  and `/tmp/app.db` is the only artifact on disk.
- **Given** `WO_DATA=<dir>` (existing directory), **when** the same
  program runs, **then** behavior is byte-identical to today —
  `<dir>/shard-0.wal`, every standing gate unchanged.
- **Given** the db-bench durability legs pointed at the file form,
  **when** the restart proof and kill -9 battery run, **then** every
  guarantee holds identically (the file IS the same WAL, only named by
  the operator).

## Out Of Scope

- A paged database file (SQLite's shape) — RAM is authoritative; the
  disk story is the WAL, full stop.
- Checkpoint/compaction — [iteration 32](32-wal-checkpoint.md)'s; its
  rename-swap (write snapshot+tail to a NEW file, fsync, `rename()`
  over the old) is exactly what keeps the single-file promise crash-safe
  when it lands. 33 before or after 32 works; landing 33 first means
  32's spec inherits the file form as a stated constraint.
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

## Proposed Solution

Small enough for a bounded slice: brainstorm the one fork in chat,
implement driver + db-bench file-form leg + docs in one pass, gates
green. No plan document needed unless it grows.
