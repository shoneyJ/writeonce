---
track: databasev2
iteration: "2"
status: refine
---

# databasev2 2 — `@table` storage modes: durability becomes a language decision

> Part of [Story — databasev2: the database beyond RAM](00-story.md).
> Needs [1](01-ram-ceiling-measurement.md) — a mode's default should follow from
> a measurement, not a preference.
>
> **The developer's ask, and the language enrichment this track exists for.**
> Today durability is one environment variable for a whole process: `WO_DATA` is
> set and every `@table` is WAL-logged, or it is not and none are
> (`runtime/src/main.c`). Real applications are not uniform. A session table, a
> rate-limit counter and a page cache are resident and disposable; an orders
> table is resident and precious; an audit log is precious and rarely read. One
> global switch forces "everything is precious" or "nothing is", and the
> developer pays for the wrong one either way.

> **⚠ Superseded in part, 2026-08-26.** The brainstorm settled on a different
> shape than this document describes: one log-structured engine where the WAL
> *is* the row store, with residency declared per table (`resident: all` /
> `resident: keys`) rather than a three-valued `mode:` enum including `cold`.
> Principle 7 was amended accordingly — the log is authoritative, residency is
> the declaration. This file is rewritten once the grammar is approved; read the
> [track index](00-story.md) and `docs/00-principles.md` §7 as current.

## Goals

- **Move the storage decision to the declaration site.** `@table(mode: ...)`,
  chosen per table, in the source, where the person who knows what the data is
  worth is already writing. An operations runbook is the wrong place for a fact
  the compiler could hold.
- **Three modes, each earning its existence.** `ram` — resident, never logged,
  gone on restart. `durable` — today's behaviour, resident and ack-after-fsync,
  and the **default** so every existing program is byte-identical. `cold` —
  durable and not required to be resident, declared here and *implemented* in
  [6](06-cold-tiering.md), because a mode with no engine behind it is a promise.
- **Make the compiler enforce what the mode means.** This is the part that makes
  it a language feature rather than a config key. A program that inserts into a
  `ram` table and expects the row after a restart is stating a contradiction, and
  the compiler is the right place to say so — at minimum for the cases it can
  see statically, with the diagnostic catalogued like every other.
- **Let the engine act on it.** `ram` tables skip the WAL write entirely, which
  is not merely a saving — it is the 66× gap iteration 22 measured (4.5k durable
  vs 297k RAM inserts/s) becoming available per table instead of per process.
  They also become the first candidates to shed under pressure, which is what
  [5](05-bounded-tables-eviction.md) builds on.
- **Keep principle 7 intact and say why.** RAM stays authoritative for every
  table that says so. `cold` is a declared, per-table exception a developer opts
  into with the trade visible at the declaration.

## Phases

### Phase A — the grammar

- Extend the `@table` argument parser with `mode:`. The path is already cut:
  the argument loop matches `name` and `index` and rejects anything else with a
  catalogued diagnostic (`unknown @table argument ... (supported: name, index)`),
  so this is one more arm plus an updated message.
- Extend `Ast.table_cfg` — today `{ table_name; indexes }` — with the mode, and
  give it the default so every existing `@table` keeps its meaning.
- Reject the incoherent cases at parse time: `mode` given twice, an unknown mode
  name. Both belong in the same diagnostic family as the existing `@table`
  errors, and both go in the error catalog **in this change**, not later — that
  catalog went stale once by exactly that omission.
- Verify: golden AST fixtures for each mode; `woc-test` green; every existing
  `@table` in every sample parses unchanged.

### Phase B — the mode reaches the image and the engine

- Carry the mode through the class descriptor into the `.wob` image so the
  runtime knows it without re-deriving anything. This is a format change, so it
  moves `WOB_VERSION` and the format contract in the same commit as the code.
- The engine consults it at the one choke point that already exists: the
  `INDEX HOOK` / WAL staging site in `wo_row_insert` / `wo_row_remove`, which
  `database/src/CODE-LOGIC.md` names as the only place storage may be mutated.
  A `ram` table stages nothing.
- Replay must skip records for tables that are now `ram` — a WAL written when a
  table was `durable` and replayed after the source changed is a real
  migration case, and silently resurrecting rows into a `ram` table would be
  worse than refusing.
- Verify: a `ram` table's inserts produce no WAL growth (measured, not assumed);
  a `durable` table is byte-identical to today; a mode change across a restart
  is handled explicitly rather than by accident.

### Phase C — the compiler's enforcement

- Decide how far static checking goes (fork 2) and implement that much. The
  floor: `WO_DATA` set with every table `ram` is a program that asked for a data
  directory it will never write to — worth a warning at least.
- The interesting case is a `ram` table participating in a `ref`/`backlink`
  relation with a `durable` one. A durable row holding a foreign key into a
  table that evaporates on restart is a dangling reference by construction, and
  FK-restrict cannot save it. This is the check most worth having, and it is
  statically visible from the class table.
- Verify: corpus `compile-fail` fixtures for each refusal; each carries the
  exact `WO-E###` the catalog now documents.

### Phase D — prove it on a real workload

- Give `docs/examples/employee` or the `db-bench` sample a mixed schema — at
  least one `ram` table and one `durable` — and gate the distinction: after a
  restart, the durable rows are present and the ram rows are gone. That single
  assertion is the whole feature.
- Extend the durability legs of `just db-bench` so the per-table write-path
  saving appears as a baseline number, not a claim.
- Verify: `just employee`, `just db-actor`, `just db-bench`, `oop-accept` green.

### Phase E — document the contract

- The `@table` mode surface in the language-surface guide and the db-binding
  contract; the new diagnostics in the error catalog; the mode's effect on
  replay in `database/src/CODE-LOGIC.md`.
- Verify: `just linkcheck` clean; the language-surface guide's `@table` row
  matches what the parser actually accepts.

## Acceptance Criteria

- **Given** every existing `@table` declaration in the repository, **when** it is
  compiled after this change, **then** behaviour is byte-identical — `durable`
  is the default and nothing opts in silently.
- **Given** a table declared `mode: ram`, **when** rows are inserted with
  `WO_DATA` set, **then** the WAL does not grow, and after a restart the table is
  empty while `durable` tables in the same program replay intact.
- **Given** `mode: ram` and a measured insert workload, **when** it runs against
  the same shape as a `durable` table, **then** the write-path saving is visible
  in `bench/baseline.json` — the per-table half of iteration 22's 66× gap.
- **Given** an unknown mode name or `mode:` given twice, **when** it is compiled,
  **then** it fails with the catalogued diagnostic naming the legal modes.
- **Given** a `durable` table holding a `ref` into a `ram` table, **when** it is
  compiled, **then** the compiler refuses (or warns, per fork 2) — a persistent
  row cannot reference one that evaporates.
- **Given** a WAL containing records for a table whose source now says `ram`,
  **when** the program starts, **then** the situation is handled explicitly
  (refuse, or skip and report) and never by silently loading rows into a table
  declared not to have any.
- **Given** the `.wob` format change, **when** an image from the previous version
  is loaded, **then** the loader refuses it clearly on the version rather than
  misreading a descriptor.

## Out Of Scope

- **Implementing `cold`.** Declared here so the mode set is settled and the
  format carries it; the engine behaviour is [6](06-cold-tiering.md). Until then
  a `cold` declaration must be refused rather than silently treated as
  `durable` — accepting a mode that does nothing is how a feature becomes a lie.
- **Per-table capacity limits and eviction** — [5](05-bounded-tables-eviction.md).
  This iteration says what a table *is*; that one says how much of it there may
  be.
- **`@table` feature flags and `transaction { }`** — language
  [iteration 18](../language-runtime-database/18-memory-db-features.md), whose
  spec is approved and deliberately left whole.
- **Per-table WAL files.** One log, one writer, shard 0 — the invariant stage 3
  established and [7](07-single-file-db.md) depends on. Modes decide *whether* a
  table logs, never *where*.
- **Migrating an existing dataset between modes.** A schema-change story, and
  the repo already records destructive migrations as a recorded future.
- **Encryption at rest, compression of the WAL.** Neither has a consumer.

## Info

The grammar surface this touches, read from the source: the parser's `@table`
argument loop and its `unknown @table argument` failure; `Ast.table_cfg` as
`{ table_name : string option; indexes : string list list }`; and the class
descriptor in `runtime/src/wob.h` that the loader validates. Adding a key is
genuinely small — the semantics are the iteration.

Forks the spec must settle:

1. **What are the modes called?** `ram` / `durable` / `cold` is descriptive of
   mechanism. `scratch` / `persistent` / `archived` is descriptive of intent and
   is what a developer reasons about. The names are the API and are hard to
   change later; leaning the intent-shaped set for the first two if a
   short-enough pair can be found, since a developer choosing a mode is thinking
   about what the data is *for*, not about where it sits.
2. **How hard does the compiler push?** Three levels: warn on the suspicious
   cases; refuse the provably-broken ones (a `durable`→`ram` `ref`); or a full
   dataflow check that an insert into a `ram` table is never expected to persist.
   The third is not statically decidable in general. Leaning: refuse the
   relation case (provable, high value), warn on the `WO_DATA`-with-no-durable-
   table case, and stop there.
3. **What is the default, and does it depend on iteration 1?** `durable` keeps
   every existing program identical, which is nearly decisive. But if iteration
   1's numbers show the WAL write dominating a workload nobody wanted durable,
   there is an argument for making the choice mandatory — no default, every
   `@table` states its mode. That is a bigger source change and a better
   language; the fork is whether the churn is worth it now or at 1.0.
4. **Does `mode: ram` imply anything about the actor/DB-actor path?** Stage 3
   marshals worker-shard statements to shard 0 because the owner shard holds the
   store and the WAL. A `ram` table has no WAL — so does it still need to live on
   the owner? A per-shard `ram` table would be dramatically faster and a
   different consistency story. Tempting, out of scope here, and worth recording
   as a candidate rather than deciding in passing.
