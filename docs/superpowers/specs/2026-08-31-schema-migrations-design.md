# Schema migrations v1 — add + delete, declarative, auto on boot

> Brainstormed and approved 2026-08-31. Story:
> [databasev2 12](../../stories/databasev2/12-schema-migrations.md).
> Scope settled during brainstorm: declarative model, add/delete only,
> zero-value fill, data/seed migrations deferred to v2.

## The problem

A `@table` class is the schema and `WO_DATA`'s log is the database, but the
two are never compared. A record names its class by `cid` — the class's
declaration index — and replay decodes every record against the *compiled*
shape. So today, all verified:

- Adding a field makes every old record under-run its decode and replay
  refuses the whole log as "corruption".
- Deleting a field leaves trailing bytes — the same refusal.
- Reordering `@table` declarations renumbers cids and rows decode into the
  wrong class, silently when shapes happen to match.
- Nothing records which schema wrote the log. The durable/resident *mode*
  mismatch is checked at boot (databasev2 2); the shape is not.

Changing a `@table` class is a database migration. The language performs it.

## The model, and why

**Declarative, automatic, at boot.** The binary already carries the schema —
`wo_classdesc` has the class name and per-field names (v2 metadata), so no
`.wob` format change is needed. At startup the runtime compares the shape
recorded in the log against the compiled shape and either replays (match),
migrates (add/delete), or refuses (everything else). No migration files, no
offline step: writeonce's doctrine is one binary and no ops choreography, and
a migration artifact would be a second source of truth for a schema the class
declaration already states.

**v1 verbs: add and delete only.** They are the two changes whose data
consequence is unambiguous: an added field has no stored values and takes the
kind's zero value (Int 0, Float 0.0, Bool false, Text empty, optional nil);
a deleted field's stored values are freed and physically dropped when the
rewritten log is swapped in. Zero-fill is chosen over a default-value syntax
because the grammar has no field defaults today and v1 refuses to grow
compiler surface for a value the program can backfill itself.

**Everything else refuses, precisely.** Refusal messages follow the
`resident: keys` house style: name the class, the field, the stored shape,
the compiled shape, and what to do about it.

- A same-named field with a different kind: refused — no implicit
  conversions.
- A deleted field and an added field of the same kind in one step: refused —
  byte-for-byte indistinguishable from a rename, and one interpretation
  destroys data the other preserves. The message says to deploy the delete
  and the add as two separate steps when both are genuinely meant.
  (`@renamed_from` is the v2 answer.)
- A stored class absent from the compiled program: refused — its rows demand
  a verdict the runtime must not guess. (The existing durable-to-volatile
  refusal from databasev2 2 stays as-is.)
- Index or storage-flag changes: refused in v1.

## Mechanics

**The schema record.** A new WAL record kind, `WO_WAL_SCHEMA`, CRC-framed
like every other record, describing every durable class: class name, storage
flags, and per field its name and kind plus the container/reference metadata,
then the secondary-index layout. It is written as the first record of a fresh
log and as the first record compaction writes, so the head of a log always
states the shape of everything after it.

**The boot diff.** Before replay, the runtime peeks the log head.

- No schema record — a legacy log. Replay proceeds against the compiled
  shape exactly as today; the next compaction writes the record and the log
  is self-describing from then on. A legacy log whose shape already changed
  still fails as today: there is nothing recorded to diff against.
- Record present and equal to the compiled shape — normal replay. Equality
  is by class NAME and field NAME + kind, so pure declaration reordering is
  a match, which closes the silent cid-renumbering hole as a side effect.
- Record present and different — classify per class into migrate or refuse,
  as above.

**The migration pass.** Replay decodes each record with the OLD shape,
reconstructed from the schema record — old kinds drive the value decoder,
old names drive the mapping. Rows are built directly in the new shape
through a per-class field map: surviving fields move to their new slot,
deleted fields' values are freed, added fields take the zero value. Old cids
map to new cids by class name. A delta record's field index is an old-shape
index; the fold happens in old shape and maps afterward, so a delta on a
deleted field folds to a no-op. The pass then writes a fresh log the way
compaction does — schema record, one full-row record per live row, fsync,
rename — and boot continues on the new log. Keys-resident tables get their
offset maps rebuilt from the new log by the machinery compaction already
carries. One stderr line per migrated class reports what changed, rows
rewritten, and values dropped.

**Crash safety is inherited, not added.** The rename swap is atomic: a crash
anywhere in the pass leaves the old log intact and the next boot re-migrates
from it. This is databasev2 3's mutation-proven design, reused.

## Acceptance criteria

- Given a log written with shape A and a binary compiled with shape A plus a
  new field, when the program boots, then it runs, the field reads as the
  kind's zero value, and the log head carries the new schema.
- Given shape A minus a field, when it boots, then the dropped values are
  freed (leak-clean under ASan) and no record of the field remains in the
  rewritten log.
- Given a simultaneous delete and add of the same kind, when it boots, then
  it refuses naming both fields and the two-step alternative.
- Given a retype or a vanished class with stored rows, when it boots, then it
  refuses naming the class, field and both shapes.
- Given pure declaration reordering, when it boots, then no migration runs
  and every row lands in its right class.
- Given a keys-resident table across a migration, when read after boot, then
  folds resolve through the new offsets and a delta on a deleted field is
  gone.
- Given a legacy log with an unchanged shape, when it boots, then replay is
  byte-for-byte today's behaviour and the next compaction writes the schema
  record.
- Given a kill between the new log's write and the rename, when the next
  boot runs, then it re-migrates from the intact old log.

## Out of scope (v2 candidates, each with its reason)

- **Rename** — needs declared intent (`@renamed_from`); v1 refuses the
  ambiguous diff instead of guessing.
- **Retype / conversions** — even lossless ones invite silent surprises;
  refusal costs one explicit backfill program.
- **Dropped-class data verdicts** — refusing keeps the data; deleting it is
  a decision, not a migration.
- **Data/seed migrations** (`@seed(n)`) — deferred by decision; the site's
  trap 1 stays documented in the deploy guide.
- **Field-default syntax** — new grammar for what zero-fill plus app code
  already covers.
- **Index migrations** — indexes are rebuilt at boot anyway; the refusal is
  about flags, not data, and can be relaxed later with evidence.
