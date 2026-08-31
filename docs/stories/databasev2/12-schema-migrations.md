---
track: databasev2
iteration: "12"
status: done
readiness: ready
---

# databasev2 12 — schema migrations: add + delete, declarative, auto on boot

> Part of [Story — databasev2: the database beyond RAM](00-story.md).
> Spec: [`2026-08-31-schema-migrations-design.md`](../../superpowers/specs/2026-08-31-schema-migrations-design.md).
>
> A `@table` class is the schema and the log is the database, but nothing
> compares them: today an added or deleted field turns a healthy `WO_DATA`
> into "corruption" at boot, and reordering declarations decodes rows into
> the wrong class without a sound. Changing the class IS a migration — this
> iteration makes the runtime perform the two unambiguous ones and refuse
> the rest by name.

## Decisions (locked in brainstorm, 2026-08-31)

- **Declarative, automatic, at boot** — the binary carries the schema; no
  migration files, no offline tool. Chosen over migration-code-in-language
  and an offline `--migrate` step.
- **v1 verbs: add + delete only.** Added fields take the kind's zero value —
  the grammar has no field-default syntax and v1 does not grow compiler
  surface. Deleted fields' values are freed and physically dropped at the
  rename swap.
- **Same-kind delete+add in one step refuses** — indistinguishable from a
  rename, and the wrong guess destroys data. Two-step deploy or v2's
  `@renamed_from`.
- **Data/seed migrations are v2** — the deploy guide's trap 1 (seed only
  fills an empty table) stays documented, not fixed.

## Tasks

| # | Task | State |
| --- | --- | --- |
| 1 | `WO_WAL_SCHEMA` record: encode/decode, written LAZILY ahead of the first record and by compaction | ✅ `ba8519f` |
| 2 | boot diff: peek head, match by class/field NAME, classify migrate / refuse / legacy | ✅ `63a063b`, wired `b21943a` |
| 3 | migration pass: a record-level TRANSCODE (no replay, no db state), rename swap | ✅ `b69092a` |
| 4 | refusals: per-class poisons that bite only when a record of the class is met | ✅ `63a063b` + `b21943a` |
| 5 | tests: 21 new across three tiers, incl. delta splice, owned cid fixup, valid-temp crash | ✅ `test_wal` 5966/0 |
| 6 | close out: story, status board, deploy-guide note, CODE-LOGIC | ✅ this commit |

## What changed against the plan, and why

- **The migration pass became a record-level transcode**, not an old-shape
  replay: the log is rewritten record by record (cids remapped by name,
  fields moved/dropped/zero-filled, delta back-pointers rewritten through an
  offset map) and the result replays through machinery that already exists
  and is already tested. No id maps, no indexes, no keys-resident logic in
  the migration itself.
- **The head record is written lazily**, ahead of the first real record. The
  eager version broke `durable: false`'s documented zero-bytes contract by
  75 bytes; `residency-accept` caught it.
- **A poison forces the transcode instead of being skipped.** The first
  end-to-end retype fell through to replay's generic "corruption" because a
  poisoned class did not break plan identity — the exact message this
  iteration exists to replace. Caught by the language-level smoke test.

## Acceptance criteria

All met. Tests in `runtime/test/test_wal.c` unless noted.

- ✅ added field zero-valued, head carries the new schema —
  `test_migrate_add_field`, and end-to-end at the language level
  (`migrating \`Note\`: +flag`, row reads `flag=0`).
- ✅ deleted field's values freed and absent — `test_migrate_delete_field`
  under ASan.
- ✅ same-kind delete+add refuses naming the two-step alternative —
  `test_schema_diff_verdicts` case 5; bites-only-with-records proven by
  `test_migrate_poison_needs_records`.
- ✅ retype / vanished class refuse by name — verdict cases 6–7; end-to-end
  the retype refusal names `val` and exits 2, and the previous binary still
  boots the refused log untouched.
- ✅ pure reordering migrates cids only — `test_migrate_reorder_owned`, which
  also proves the cid INSIDE a stored owned value is renumbered.
- ✅ keys-resident across a migration — `test_migrate_delta_splice`: a chain
  with a delta on the deleted field folds to the surviving field's latest
  value after replay.
- ✅ legacy log unchanged-shape byte-for-byte — schema unset changes nothing
  (all 5700 prior assertions), `test_schema_compaction_adopts_legacy` proves
  head adoption.
- ✅ kill between write and rename — `test_migrate_crash_before_rename`
  plants a COMPLETE valid migrated temp beside the untouched original; the
  next boot discards it and re-migrates.

## Out of scope

- **Rename** — v2, via `@renamed_from`; v1 refuses the ambiguous diff.
- **Retype/conversions** — refusal costs one explicit backfill program.
- **Dropped-class verdicts** — refusing keeps the data; deletion is a
  decision, not a migration.
- **`@seed(n)` data migrations** — deferred by decision.
- **Field-default syntax** — zero-fill plus app code covers it.
- **Index migrations** — indexes rebuild at boot; refusal is about flags.
