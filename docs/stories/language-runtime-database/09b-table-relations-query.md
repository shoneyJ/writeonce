---
iteration: "9b"
status: done
readiness: ready
---

# Iteration 9b — `@table`, relations, and language-integrated query

> Format: fiberloom `product/story-iteration-template`. Part of
> [Story — one language, one runtime, one database, one binary](00-story.md).
>
> **Inserted 2026-08-11**, hence `9b` rather than a renumber. It follows
> iteration 9 because a query surface needs tables that actually execute, and
> precedes iteration 25 because `service` blocks will want to return query
> results.
>
> **Spec exists (2026-08-15):**
> [`2026-08-15-table-relations-query-design.md`](../../superpowers/specs/2026-08-15-table-relations-query-design.md)
> settles the three forks recorded in *Info* below (kept as the decision
> record): the SQL/Cypher layer is superseded as the program surface,
> the syntax is a compiler-desugared comprehension, and the references
> contribute vocabulary + semantics (System.Linq) and execution + integrity
> vocabulary (PostgreSQL, surveyed with the spec). Plan:
> [`2026-08-15-employee-relations-query.md`](../../plan/compiler/2026-08-15-employee-relations-query.md).

## Goals

- `@table` graduates from a parsed-but-inert annotation into the declaration
  that makes a class persistent: named storage, declared indexes, and a
  primary identity.
- Relations become first-class and typed — `ref T` foreign keys, `backlink`
  inverses, and `multi` collections — so a developer navigates their data by
  following fields rather than by hand-writing joins.
- Queries are **written in the language, checked by the compiler**: a
  LINQ-shaped operator vocabulary (filter, project, join, group, order,
  aggregate) over tables and relations, with the result's type inferred and
  every column reference resolved at compile time. A typo in a field name is
  a compile error, not a runtime one.

## Acceptance Criteria

- What to achieve?
    - **Given** a class annotated `@table` with a declared index,
    - **when** the program is compiled and run,
    - **then** its instances persist through the engine, the index is built,
      and a query that could use the index does use it — demonstrated, not
      assumed.
- What to achieve?
    - **Given** two classes related by `ref` with a `backlink` inverse,
    - **when** a query navigates the relation in either direction,
    - **then** it typechecks with the related class's field set in scope, and
      navigating a field that does not exist is a compile error naming it.
- What to achieve?
    - **Given** a query whose result shape is a projection rather than a whole
      row,
    - **when** it is assigned or returned,
    - **then** its type is the projected shape — so a later use of a column
      the projection dropped is a compile error.
- What to achieve?
    - **Given** a query written against tables,
    - **when** the compiler lowers it,
    - **then** it becomes engine operations, **not** a string handed to a
      parser at runtime — provable by disassembly, and by the absence of any
      SQL-text construction in the emitted image.
- What to achieve?
    - **Given** the ecommerce sample's existing relational shapes
      (`Order.user: ref User`, `User.orders: backlink Order.user`, line-item
      collections),
    - **when** they are expressed as queries in this surface,
    - **then** each produces the same results as the equivalent hand-written
      query, and the sample's README records anything that could not be
      expressed.

## Out Of Scope

- Cross-shard queries and distributed joins — iteration 8 owns ownership
  movement, and a query spanning shards is a 2PC concern recorded with the
  database track.
- `LIVE` subscriptions over queries. The subscription registry is the
  HTTP/UI track's; a query that pushes updates is a later composition of the
  two.
- Migrations. Changing a `@table` class's shape is the blue-green spec's
  additive-only differ (iteration 26), not this iteration's problem.
- Query optimisation beyond index selection. A cost-based planner is a
  separate, much later concern; this iteration must only prove that declared
  indexes are used.

## Info

Three open forks the spec has to settle. Each is a real decision, and I have a
leaning on all three but no mandate.

**1. Where does this leave the existing SQL + Cypher query layer?**
`docs/runtime/database/02-wo-language.md` specifies a two-layer design — a
schema layer plus a query layer of literal SQL and Cypher with five
"fixed-glue" rules. A language-integrated surface either replaces that layer,
sits beside it, or becomes the only surface with SQL retained purely as an
export format. Replacing it is the coherent choice and also the most
disruptive, because that document is normative and the `wo-db` C++ prototype
implements the SQL/Cypher grammar it describes.

**2. There are no function values, so what is the syntax?**
LINQ-to-Objects is built on delegates: `.Where(x => x.Age > 18)` passes a
lambda. writeonce has **no function-value type**, and the systems-track spec
deliberately rejected closure builtins (`map`/`filter`/`reduce`) for exactly
this reason. So the surface cannot be method-chaining-with-lambdas as written
in C#. The realistic options are a comprehension syntax the compiler desugars
(`from o in orders where o.total > 100 select o.id`), or method chaining whose
"lambda" argument is a compiler-recognised expression form rather than a
value. Either way the predicate is **compile-time syntax, never a runtime
closure** — which is also what lets the whole query lower to engine ops.

**3. What does the reference actually contribute?**
`.dev/reference/dotnet-runtime/src/libraries/System.Linq/src/System/Linq/`
(sparse checkout, added with this iteration) is the operator catalogue: read
`Where.cs`, `Select.cs`, `Join.cs`, `GroupBy.cs`, `OrderBy.cs` for what each
operator means and which edge cases it has to answer, and the `*.SpeedOpt.cs`
files for how LINQ specialises when the source's shape is known statically —
directly relevant, since writeonce knows every shape statically. What does
**not** transfer is the machinery: `IEnumerable` iterator composition,
delegates, and `IQueryable`'s runtime expression trees, the last of which
depends on reflection that principle 13 forbids outright. Take the vocabulary
and the semantics; leave the plumbing.

**4. (Settled with the spec, 2026-08-15) How do queries interact with the
borrow checker and the GC?** Row views are borrows of engine memory with **no
runtime borrow word behind them** — the compile-time escape rule is
load-bearing alone. Scans materialize their id list up front, so updating a
row (even an indexed column) inside the loop is sound, while `insert`/`delete`
on a table with an open cursor is a compile error. The GC never meets the
engine at all: both directions across the boundary are copies, and GC-managed
values cannot be stored — spec section 6 is the full analysis, including the
iteration-7b ordering constraint (inference before table-field validation).

Also relevant: `@table(name:, index:)` already parses today with known-key
validation (`WO-E102`), the Rust runtime already ships secondary indexes and
`find_by` behind that annotation, and `ref T` already classifies as a scalar
id rather than a pointer — so the relational vocabulary partly exists and this
iteration makes it mean something in the C stack.

## Query surface landed (2026-08-16, branch `query-surface`)

The compiler-checked query surface runs end to end, proven by
`docs/examples/employee` (8-check acceptance, `scripts/employee-accept.sh`):

- **Queries**: `from <v> in <table|nav> where* [order by <k> [desc]] [take n]
  select <v|v.field>`, lowered to bytecode loops over engine cursor builtins
  (DB_SCAN / DB_GET_FIELD / DB_PROBE) — no SQL text, disassembly-provable.
- **Relations**: `ref C` forward navigation (`e.dept.name`, a point read),
  `backlink C.f` reverse navigation (`d.staff`, an index probe); backlink
  fields are virtual (no stored column).
- **Mutation**: update-through-row (`e.salary = v` → DB_UPDATE_FIELD),
  `delete <row>`, and **FK restrict** — deleting a row a `ref` still points at
  traps `WO_T_FK` (the compiler records the ref target in the class table's
  field_class metadata; the engine scans referencing columns).
- **`@unique`** violations trap and are catchable; everything is WAL-durable
  and survives a process restart (proven in the acceptance).

**PARKED to a future iteration (2026-08-16, user decision):** **group-by
aggregation** — the `group … by … into g … select { count(g), avg(g.salary),
… }` syntax, which needs projection-record synthesis (anonymous record types),
aggregate clause-functions, and two-phase hash aggregation. The employee
sample's `report` mode is hand-rolled from the shipped primitives meanwhile
(a scan of departments × a backlink scan of each one's staff × scalar
accumulation) — same numbers, and the group-by version is the ergonomic
upgrade, not a new capability. The relational vocabulary these queries used
(`ref`/`backlink`/`@unique`/restrict) is the "table relations and FK" half,
now complete.

## Proposed Solution

- ~~Brainstorm a spec first~~ — **done 2026-08-15**; the spec settles all
  three forks and the plan exists (pointers in the header note). The fork-1
  outcome for the record: language-integrated query is the only program
  surface; `docs/runtime/database/02-wo-language.md`'s SQL/Cypher layer stays
  as design history and as the `wo-db` prototype's engine-semantics
  reference, never as syntax.
- **The acceptance workload is a new sample**: `docs/examples/employee` —
  `Department`/`Employee` with `@unique`, a composite index, a `ref`/
  `backlink` pair, and a report mode that is one `GROUP BY` after another
  (headcount, avg/min/max salary by department). It is 9b's acceptance the
  way log-watcher was iterations 1–7's; the ecommerce query rewrite (the
  fifth criterion below) follows as its own step once employee is green.
- Study `.dev/reference/dotnet-runtime`'s `System.Linq` operator set for the
  vocabulary, and `docs/runtime/database/02-wo-language.md` plus
  `prototypes/wo-db/` for the semantics already committed to.
- Expect the work to span the front end (query syntax, relation typing,
  projection types), the emitter (lowering to engine operations rather than
  text), and the engine (index selection, relation traversal) — which is why
  it follows iteration 9 rather than preceding it.
