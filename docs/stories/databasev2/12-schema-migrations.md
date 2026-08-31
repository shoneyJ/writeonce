---
track: databasev2
iteration: "12"
status: in-progress
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
| 1 | `WO_WAL_SCHEMA` record: encode/decode, emitted by fresh-log open and by compaction | ⬜ |
| 2 | boot diff: peek head, match by class/field NAME, classify migrate / refuse / legacy | ⬜ |
| 3 | migration pass: old-shape replay through a field map, compaction-style rewrite, rename swap | ⬜ |
| 4 | refusals: retype, same-kind delete+add, vanished class — house-style messages | ⬜ |
| 5 | tests: the acceptance list, incl. keys-resident, legacy log, crash injection | ⬜ |
| 6 | close out: story, status board, deploy-guide note | ⬜ |

## Acceptance criteria

- **Given** a log written with shape A and a binary with one added field,
  **when** it boots, **then** it runs, the field reads zero-valued, and the
  log head carries the new schema.
- **Given** one deleted field, **when** it boots, **then** dropped values are
  freed (ASan-clean) and absent from the rewritten log.
- **Given** delete+add of the same kind, **when** it boots, **then** refusal
  naming both fields and the two-step alternative.
- **Given** a retype or a vanished class with rows, **when** it boots,
  **then** refusal naming class, field and both shapes.
- **Given** pure declaration reordering, **when** it boots, **then** no
  migration and every row in its right class *(closes the silent
  cid-renumbering hole)*.
- **Given** a keys-resident table across a migration, **when** read,
  **then** folds resolve through new offsets; a delta on a deleted field is
  gone.
- **Given** a legacy log (no schema record) with an unchanged shape,
  **when** it boots, **then** today's behaviour byte-for-byte, and the next
  compaction writes the record.
- **Given** a kill between new-log write and rename, **when** the next boot
  runs, **then** it re-migrates from the intact old log.

## Out of scope

- **Rename** — v2, via `@renamed_from`; v1 refuses the ambiguous diff.
- **Retype/conversions** — refusal costs one explicit backfill program.
- **Dropped-class verdicts** — refusing keeps the data; deletion is a
  decision, not a migration.
- **`@seed(n)` data migrations** — deferred by decision.
- **Field-default syntax** — zero-fill plus app code covers it.
- **Index migrations** — indexes rebuild at boot; refusal is about flags.
