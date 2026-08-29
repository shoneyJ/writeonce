---
track: databasev2
iteration: "8"
was_language_iteration: "27"
status: hold
readiness: refine
---

# databasev2 8 — query grammar, driven by real embedded-DB corpora

> **Moved 2026-08-26** from the language track, where this was iteration 27.
> Part of [Story — databasev2: the database beyond RAM](00-story.md). Content unchanged by
> the move; its dependencies are restated in that track index.

> Format: fiberloom `product/story-iteration-template`. Part of
> [Story — one language, one runtime, one database, one binary](../language-runtime-database/00-story.md)
> — the track this iteration was authored in before the 2026-08-26 move.
>
> **Inserted 2026-08-16.** A query-surface iteration in the 9b family: the
> language-integrated query grows to cover the grammar that *real
> applications backed by an embedded SQL database actually use* — measured,
> not guessed, by cataloguing a real app and adding only the constructs it
> depends on. The method is the postgres/System.Linq reference pattern
> applied to a whole application: **an embedded-SQLite app is a grammar
> corpus; each one analysed drives a grammar increment.**
>
> **Implemented 2026-08-16 (branch `query-grammar`).** The forks resolved
> empirically, and the result is the strongest possible one for the method:
> **corpus #1 (skillhost) forced NO new query grammar.** `count(<query>)` and
> `len(<query>)` already work (fork 2), and skillhost's correlated `NOT EXISTS`
> is a backlink emptiness — `where len(x.children) == 0` — using only 9b
> machinery (fork 1). So `exists`/`not exists` was **not** built: the method is
> "add only what a corpus uses," and this corpus uses nothing new. Deliverable
> is the parity sample (`docs/examples/skill-catalog`, all five skillhost
> statements translated 1:1, `scripts/skill-catalog-accept.sh` 7/0) and this
> recorded conclusion — not speculative subquery machinery.
>
> **Still open (no corpus yet):** the general `exists`/`not exists` construct,
> for a correlation a backlink cannot express (a correlation on a non-relation
> column). It enters when a corpus forces it, exactly as the method prescribes.

> **No spec exists yet.** The forks in *Info* are genuine decisions.

## Why this iteration exists

The 9b query surface ships scan / `where` / `select` / `order by` / `take`,
`ref`/`backlink` navigation, insert / update / delete, `@unique`, and FK
restrict (all running, `docs/examples/employee`). What it does NOT yet cover
is everything past that — and "everything" is unbounded, so the sensible way
to choose the *next* grammar is to point at a real program that uses an
embedded SQL database and add exactly what it needs.

**Corpus #1: `~/projects/skillhost`** (a C++ MCP host, embedded SQLite as its
in-memory skill catalog; surveyed 2026-08-16). Its entire SQL footprint is
one file (`src/catalog/catalog.cpp`, 172 lines): one table + index, a
single-row parameterized `INSERT`, and four `SELECT`s. Mapping each statement
to the writeonce query surface:

| skillhost statement | writeonce today |
| --- | --- |
| `INSERT INTO skills (…) VALUES (?,…)` | ✅ `insert Skill { … }` |
| `SELECT … WHERE name = ?` | ✅ `from s in Skill where s.name == n select s` (unique-index probe) |
| `SELECT … ORDER BY name` | ✅ `from s in Skill order by s.name select s` |
| `SELECT COUNT(*) FROM skills` | ❌ **whole-query `count`** |
| `SELECT … WHERE NOT EXISTS (SELECT 1 FROM skills c WHERE c.parent = s.name) ORDER BY name` | ❌ **correlated `not exists` subquery** |

So the real grammar gap this corpus demands is **two constructs**, and —
importantly — *neither is the parked full group-by/projection machinery*.
Everything else SQLite offers (JOIN, HAVING, LIMIT/OFFSET, DISTINCT, CTE,
window functions, UNION, UPSERT, RETURNING, JSON1, FTS5, triggers, generated
columns) skillhost does not touch, so none of it is in scope here.

## Goals

- **Whole-query `count`**: `count(from s in Table [where …] select …)` yields
  an `Int` — the trivial, group-free special case of aggregation (materialize
  the query, take its length). It is a stepping stone toward, and independent
  of, the parked group-by aggregation.
- **Existence subqueries**: `exists(<query>)` and `not exists(<query>)` as a
  boolean, usable in a `where` guard, where the inner query may reference the
  outer range variable (a *correlated* subquery — skillhost's roots-of-the-
  tree query). Short-circuits: existence needs only the first matching row.
- **Parity, proven by translation**: a new `docs/examples/skill-catalog`
  sample mirrors skillhost's schema and expresses all five of its statements
  in writeonce, producing results identical to what skillhost's SQLite
  returns for the same data.

## Acceptance Criteria

- What to achieve?
    - **Given** `count(from s in Skill select s)` and
      `count(from s in Skill where s.parent == nil select s)`,
    - **when** compiled and run,
    - **then** each yields the correct row count as an `Int`, lowered to a
      materialize-then-length over the existing scan/where loop — no group
      machinery, provable by disassembly.
- What to achieve?
    - **Given** `from s in Skill where not exists(from c in Skill where
      c.parent == s.name select c) order by s.name select s` — the roots of
      the skill tree,
    - **when** run over a catalog with parent/child skills,
    - **then** it returns exactly the childless skills in name order, and the
      inner query correctly sees the outer `s` (correlation), matching
      skillhost's `NOT EXISTS` result row-for-row.
- What to achieve?
    - **Given** the `skill-catalog` sample seeded with the same rows a
      skillhost session would load,
    - **when** each of skillhost's five catalog operations is run through the
      writeonce translation,
    - **then** every result matches, and the sample's README records the one
      translation choice made (see fork 1).

## Out Of Scope

- **Full group-by aggregation** (`group … by … into g … select { count(g),
  avg(g.f) }`) — still parked (9b's deferral). Whole-query `count` here is the
  degenerate case, not the general one; `sum`/`avg`/`min`/`max` as query
  aggregates ride with the group-by iteration.
- **Every SQL construct skillhost does not use**: JOIN, HAVING, LIMIT/OFFSET
  (writeonce has `take`; `skip`/offset waits for a workload), DISTINCT, CTE /
  `WITH RECURSIVE`, window functions, UNION/INTERSECT/EXCEPT, UPSERT /
  `ON CONFLICT`, `RETURNING`, multi-row `VALUES`, `INSERT … SELECT`, JSON1
  operators, FTS5, triggers, generated columns, explicit collation. Each
  enters only when a corpus demands it — that is this iteration's whole
  method.
- **A resident SQL parser** — the doctrine stands: skillhost is a grammar
  *corpus* to translate against, never a syntax writeonce adopts. No SQL text
  in a compiled image.
- **Subqueries in general** beyond correlated `exists`/`not exists` (e.g. a
  subquery producing a value, `IN (subquery)`, scalar subqueries) — add when
  a corpus uses them.

## Info

Forks the spec must settle:

**1. Does skillhost's `NOT EXISTS` even need a subquery, or does a `backlink`
express it?** skillhost's `skills` table is self-referential (`parent` → a
`name`), and its roots query is "skills no other skill names as parent." In
writeonce that is naturally a **backlink emptiness**: give `Skill` a
`children: backlink Skill.parent` and write `where len(s.children) == 0` — no
subquery at all, using machinery that already exists (backlink probe) plus a
`len` on the result. So the corpus may be fully expressible *today* once
`count`/`len` over a query lands, making `exists` strictly optional for
skillhost. Leaning: ship whole-query `count`/`len` (needed regardless), and
add `exists`/`not exists` as the general construct for correlations a backlink
cannot express (a correlation on a non-relation column) — but let the
`skill-catalog` sample use the idiomatic backlink form for its roots query and
record the subquery form as the alternative. This keeps the new surface
minimal and honest about what the corpus actually forces.

**2. `count` vs `len`.** writeonce already has `len`/`count` builtins on a
`multi`. A query yields a `multi`, so `len(from … select …)` may already work
with no new surface at all — the "gap" could be purely that a bare query in
argument position typechecks and lowers. Leaning: verify `len(<query>)` works
end to end first; if it does, whole-query `count` is a documentation/alias
matter, not new code, and the only real new construct in this iteration is the
existence subquery (fork 1's optional half). The spec must confirm this
against the running compiler before committing scope.

**3. Correlated-subquery execution.** If `exists` lands, the inner query
references the outer row, so it re-evaluates per outer row (a nested loop) or
uses the referenced index. Leaning: nested-loop for correctness first (the
data is small; skillhost's catalog is dozens of skills), index-backed probe as
the optimization the spec records — mirroring how 9b did scans before index
selection.

**Method note (the durable part):** this iteration establishes the pattern for
*all* future query-grammar growth — catalogue a real embedded-DB application,
add only the constructs it uses, translate its statements 1:1 as the
acceptance, and park the rest by name. skillhost is corpus #1 and, tellingly,
needs almost nothing beyond what 9b already shipped — which is itself the
strongest evidence that the 9b surface was scoped right.

## Proposed Solution

- **Brainstorm the spec** settling the three forks — especially forks 1/2,
  which may collapse the iteration to "confirm `len(<query>)` works + add
  `exists`," a very small increment.
- **Acceptance workload**: `docs/examples/skill-catalog` — `Skill { name:
  Text @unique, description: Text, location: Text, root: Text, parent: ?ref
  Skill, children: backlink Skill.parent }` and a CLI mirroring skillhost's
  catalog operations (add, get-by-name, list, list-roots, count), each a
  direct translation of the corresponding SQLite statement, with an
  acceptance script asserting the same results skillhost produces.
- Expected shape: small parser/type/emit additions for `exists`/`not exists`
  (a query in boolean position, correlated), whole-query `count`/`len`
  confirmed or wired, and the sample + script. No engine format change beyond
  what 9b already appended.
