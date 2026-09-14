---
track: databasev2
iteration: "14"
status: pending
readiness: refine
---

# databasev2 14 — the shop workload: what an order-taking web app needs from the store

> **Status:** ⬜ pending · design `refine` — see the
> [status board](../00-status.md). Part of
> [Story — databasev2: the database beyond RAM](00-story.md).
>
> **Inserted 2026-09-12** from a developer-facing question: "would I build an
> e-commerce site on writeonce?" The load side is a non-question — a hundred
> orders a minute is under two durable writes a second against an engine that
> group-commits thousands. What the developer hits is the SHAPE of the query
> and schema surface, measured against what the PostgreSQL tree under
> `.dev/reference/postgresql` gives them by habit. This iteration names those
> gaps as one workload and lets each land as its own slice.

## The user

A developer building a shop on porch: a `Product` catalogue, `Order` rows
with a `ref Product`, a stock count to decrement, a listing page, an order
history page, and a weekly revenue report. Their instinct is PostgreSQL. They
will keep writeonce only if the everyday statements are as short as SQL and
the mistakes SQL catches for them are caught here too. The driving workload is
[`docs/examples/shop`](../../examples/shop/README.md), extended as this
iteration lands; every criterion below is a page or a report that sample must
serve.

## Goals

- **Range queries and ordering through an index.** A product listing "under
  20 euros, cheapest first" and an order history "last 30 days, newest first"
  read an ordered index and stop when the range ends, instead of scanning the
  table and sorting the result. The PostgreSQL shape is `access/nbtree` behind
  `executor/nodeIndexscan.c`; writeonce's indexes today are hash buckets that
  answer equality only (`database/src/table.c`, `wo_idx_probe`).
- **Pagination.** `skip` joins `take` so a listing can show page three. The
  spec already names it
  ([`2026-08-15-table-relations-query-design.md`](../../superpowers/specs/2026-08-15-table-relations-query-design.md)
  section 3, clause 6); only `take` was built.
- **Reports through group-by aggregation.** "Revenue per day" and "top ten
  products" as one query with `group … by … into g` and `sum`, `count`,
  `avg`, `min`, `max`. Parked from language 9b on 2026-08-16: the parser
  accepts the clause, `types.ml` refuses it. This half belongs to the
  language track (compiler surface, anonymous projection records); the
  iteration lists it because the workload needs it, and names the owner.
- **Schema-level validity rules.** A composite unique key (`sku` together
  with `warehouse`), a check rule (`qty > 0`, `price >= 0`), and a declared
  on-delete policy for a `ref` (cascade the order lines, or set the reference
  to nil) beside today's FK restrict. Each is one line in the class today and
  a forgotten `if` in a handler tomorrow. PostgreSQL's shapes are the
  constraint machinery under `catalog/` and `commands/`.
- **Operations while the shop runs.** An export of the store to a text form
  a human can read and a fresh binary can import, and a read-only attach for
  a second process (the support tool, the report script) — the two habits
  `pg_dump` and a second `psql` session give a PostgreSQL user. Attach is
  [databasev2 9](09-cross-program-tables.md)'s territory and stays there;
  this iteration owns export/import and states the attach dependency.

## Acceptance Criteria

Met: none yet.

Outstanding:

- **Given** `Product` declared with an ordered index on `price`, **when** the
  listing asks for products under a price sorted ascending with `take`,
  **then** the query lowers to an index-range probe (visible in the
  disassembly as the range builtin, not `DB_SCAN`), returns exactly the rows a
  full scan plus sort returns, and its cost grows with the result size, not
  the table size — measured on `docs/examples/shop` at ten thousand products.
- **Given** an order history query with `order by created_at desc take 20
  skip 40`, **when** it runs, **then** it returns page three exactly as
  PostgreSQL's `OFFSET 40 LIMIT 20` would over the same rows, and `skip`
  without `order by` is a compile error (an unordered page is a bug).
- **Given** the shop's revenue report written as a group-by query over
  `Order` with `sum(g.total)` and `count(g)` per day, **when** compiled and
  run, **then** the numbers equal the hand-rolled loop the sample uses today,
  and referencing a dropped column after the projection is a compile error.
- **Given** a class with a composite unique key and a check rule, **when** a
  duplicate pair or a violating value is inserted or updated, **then** the
  statement traps by name before RAM moves, catchable like `@unique`, and the
  same violation on replay is refused as corruption — the engine never trusts
  a log a rule would have rejected.
- **Given** a `ref` declared with an on-delete policy, **when** the target
  row is deleted, **then** cascade removes the referencing rows in the same
  barrier and set-nil clears the references in the same barrier, both inside
  one implicit transaction, and the default stays restrict — byte-identical
  behaviour for every class declared before this landed.
- **Given** a running shop with a populated store, **when** the export
  command runs against a copy of its log, **then** it writes a text form that
  round-trips through import into a fresh store whose replayed rows are
  byte-identical, and a human can read a row in it without tooling.
- **Given** every standing gate, **when** re-run, **then** unchanged: the
  index kind, the clauses and the rules are opt-in, and a class or query
  written before this iteration compiles and runs byte-identically.

## Out Of Scope

- A second index type beyond one ordered kind (no GIN, GiST, BRIN, no
  full-text search) — the ordered index covers the shop; text search is its
  own question.
- Isolation levels and MVCC — the engine is single-writer on shard 0 and
  `transaction { }` (language 18) already gives one-barrier atomicity; there
  is nothing to isolate from.
- Replication, read replicas, high availability — the single-binary story
  stands.
- Money as a decimal type — `Int` cents is the doctrine; a `Float` price in
  the sample is the sample's problem and this iteration corrects it in the
  shop, not the language.
- Attach for a second process — databasev2 9, referenced not absorbed.

## Info — the forks (open, brainstorm before any slice starts)

1. **The ordered index's storage.** Options: a sorted array per table rebuilt
   on change (simple, O(n) insert), a skip list or B-tree in RAM rebuilt from
   the log at boot (PostgreSQL's nbtree is on-disk and page-based, which the
   log-is-the-store doctrine rejects), or a sorted run per slab merged at
   query time. Keys-resident tables (databasev2 2) hold no row bodies in RAM,
   so the index must carry the key itself. Decide after measuring the shop at
   ten thousand and one million products.
2. **Grammar for the index kind.** `@table(index: [price])` today means hash;
   options: a second argument key (`ordered: [price]`), a per-column marker,
   or making every index ordered and dropping hash (simpler surface, slower
   equality probes — measure).
3. **Range probe surface.** Whether `where` comparisons on an indexed column
   lower automatically to the probe (the 9b precedent for equality) or the
   query must name the index; and how `order by` on the indexed column
   proves it walks the index (the disassembly, as 9b did).
4. **Composite unique and check rules: where they live.** `@table(unique:
   [sku, warehouse])` beside `index:`, and a `check` as a class annotation
   over a boolean expression of fields — evaluated where? Compile-time
   constant folding is impossible for a row; the check must be a small
   engine-side interpreter or a compiled method the engine calls, and the
   latter crosses the two-memory-worlds boundary. The lightest honest option
   may be a compiled predicate method the STATEMENT calls before the builtin,
   which the engine cannot verify on replay — state the trade-off before
   picking.
5. **On-delete cascade inside the log.** A cascade deletes N rows for one
   statement: one kind-6 record with the members (language 18's block, opened
   implicitly), or N plain records under one barrier. The block is honest
   about atomicity; decide whether an implicit block may open while an
   explicit one is already open (fork 3c of language 18 says one per
   process).
6. **Export format.** Options: one JSON object per line with class name and
   fields (readable, the `json` builtins already exist), a `.wo` source file
   of `insert` statements (importable by compiling it — no import tool at
   all), or the WAL's own record dump. The `insert`-source form needs no new
   surface and is grep-able; ids would be reassigned on import, which breaks
   `ref`s unless the export orders classes by dependency.
7. **Group-by ownership.** Language track iteration, with the shop as a
   second acceptance workload beside `employee`; this iteration only carries
   the criterion. Confirm the number with the language track before writing
   the story.

## Progress

| # | Slice | Owner | Size | State |
| --- | --- | --- | --- | --- |
| A | `skip` clause (grammar, typing, lowering; `skip` without `order by` refused) | language track / codd-zack | S | ⬜ |
| B | ordered index: storage, grammar, range probe, `order by` walk, shop listing legs | codd-shoney brainstorm → codd-zack | L | ⬜ |
| C | composite unique + check rules + replay refusal | codd-shoney brainstorm → codd-zack | M | ⬜ |
| D | on-delete policy (cascade, set-nil) as an implicit block | after language 18 T7 | M | ⬜ |
| E | export / import | codd-shoney brainstorm → codd-zack | M | ⬜ |
| F | group-by aggregation | language track (own story) | M | ⬜ |
| G | shop sample grows the pages and the report; codd-cyril gate legs | codd-cyril | M | ⬜ |

## History

- 2026-09-12: story created from the "would I build a shop on this" analysis;
  seven forks open, `readiness: refine`. Attach stays databasev2 9; group-by
  stays the language track's.
