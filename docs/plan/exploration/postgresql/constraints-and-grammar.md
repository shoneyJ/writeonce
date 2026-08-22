# Constraints & DDL grammar — PK / FK / reverse navigation

What Postgres' CREATE TABLE grammar and catalog do for PRIMARY KEY,
FOREIGN KEY/REFERENCES, and reverse lookup — and the `@table` grammar
writeonce should grow from it. Tree: post-18 master
(`REL_18_BETA1-2871`); paths into
[`reference/postgresql/`](../../../../.dev/reference/postgresql/).

## The Postgres side (facts, with paths)

**Grammar** (`src/backend/parser/gram.y`):

- Column constraints (`ColConstraintElem`, :4119): `UNIQUE` (:4140,
  with `NULLS [NOT] DISTINCT`), `PRIMARY KEY` (:4153), and
  `REFERENCES qualified_name opt_column_list key_match key_actions`
  (:4224).
- Table-level twins (`ConstraintElem`, :4376): multi-column
  `UNIQUE (...)` :4404, `PRIMARY KEY (...)` :4439 (or adopt an
  existing index, :4457), `FOREIGN KEY (...) REFERENCES ...` :4493.
- REFERENCES options: `key_match` = MATCH FULL | PARTIAL
  (unimplemented, errors) | SIMPLE (default) — :4620; `key_actions` =
  `ON UPDATE`/`ON DELETE` × { NO ACTION | RESTRICT | CASCADE |
  SET NULL | SET DEFAULT } — :4664–4733. Single-char codes in
  `src/include/nodes/parsenodes.h:2928`.

**Catalog** (`src/include/catalog/pg_constraint.h`):

- One row per constraint; `contype` `'p'`/`'f'`/`'u'` (:198). FK rows
  carry the FORWARD direction only: `conrelid`/`conkey[]` (referencing)
  → `confrelid`/`confkey[]` (referenced), plus the action/match chars
  (:98–130).
- **A PK/UNIQUE constraint IS an index**: `transformIndexConstraints`
  (`src/backend/parser/parse_utilcmd.c:2245`) rewrites the constraint
  into an `IndexStmt` (`index->primary`, `index->unique`) — the
  constraint and its unique index are one object (`conindid`,
  `index_constraint_create`, `catalog/index.c:1903`). FKs are
  transformed AFTER indexes deliberately (:3023).

**FK enforcement = trigger pairs** (`utils/adt/ri_triggers.c`):

- Referencing side: INSERT/UPDATE fire `RI_FKey_check` (:358) —
  `SELECT 1 FROM <pktable> WHERE pk = $1 FOR KEY SHARE` (a probe of the
  PK's unique index; :452 even has a direct-index fast path bypassing
  SPI).
- Referenced side: DELETE/UPDATE fire the action triggers —
  restrict/noaction = `SELECT 1 FROM <fktable> WHERE fk = $1` (:903),
  cascade = `DELETE FROM <fktable> WHERE fk = $1` (:1089), setnull/
  setdefault = the obvious UPDATEs. NO ACTION vs RESTRICT differ only
  in deferrability + a replacement-row re-check (:872) — "the only
  difference", per the source comment.

**Reverse navigation — the load-bearing negative:**

- Postgres stores NO backlink. A reverse lookup is a plain scan of the
  referencing table (`WHERE $1 = fkatt1`); it is fast iff an index on
  the FK column exists. That index is recommended, NOT auto-created
  (`doc/src/sgml/ddl.sgml:1390`), because indexing choices vary. The
  referenced side, by contrast, ALWAYS has an index — it must be a
  PK/unique.

## The writeonce translation

What exists today: every class is a table; the auto-assigned,
shard-interleaved `id` is the de-facto primary key (O(1) via the
table's open-addressing id hash); `@table(name:, index: [cols])`
declares secondary indexes; `@unique` on a column; `ref T` is a stored
FK (restrict-only, checked by `wo_row_has_referrers` — currently a
full scan); `backlink T.field` is a declared reverse view (currently
an O(table) scan too).

The grammar this study argues for (words, no code — an iteration's
brainstorm decides):

1. **Keep the id as THE primary key; add `@key` as a UNIQUE ALIAS, not
   a replacement.** Postgres' lesson: a PK is just a unique index the
   catalog blesses (`transformIndexConstraints`). writeonce already has
   the blessed unique id; a user-declared `@key` on a column should
   desugar to `@unique` + the natural-lookup index — never a second
   row-identity (slabs, WAL records, and refs all speak id).
2. **`ref T` grows an action option, defaulting to today's behavior:**
   `ref T` = restrict (current semantics, now named); optional
   `@on_delete(cascade)` / `@on_delete(set_nil)` — the `?ref T` shape
   is the precondition for set_nil, exactly as SET NULL requires a
   nullable column in Postgres. MATCH variants: skip — single-column
   refs only, MATCH SIMPLE semantics by construction.
3. **Backlink beats Postgres — if it implies the index.** Postgres
   makes reverse lookup fast only when the user remembers the FK-column
   index; writeonce's `backlink T.field` is a DECLARED intent, so the
   compiler should auto-require `index: [field]` on the referencing
   table (or inject it) — the study's one clear improvement over the
   reference. `wo_row_has_referrers` and backlink reads then become
   index probes, not scans (see the indexing card).
4. **Enforcement placement:** Postgres bolts FK checks on as triggers
   because constraints arrived after the executor; writeonce's choke
   points (`wo_row_insert`/`wo_row_remove`/`wo_row_update_field`) are
   the honest home — checks stay inline, no trigger machinery, same
   observable semantics (insert probes the referenced id's existence;
   delete probes the referencing index).

Non-goals this study records: composite keys (no driving workload),
deferrable constraints (need `transaction { }` = held iteration 18),
MATCH PARTIAL (Postgres never shipped it either).
