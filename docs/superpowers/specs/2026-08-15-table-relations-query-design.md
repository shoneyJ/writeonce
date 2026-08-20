# `@table`, Relations, and Language-Integrated Query — Design

> **Status: proposed** (story iteration 9b). Settles the three forks recorded in
> [`09b-table-relations-query.md`](../../stories/language-runtime-database/done/09b-table-relations-query.md).
> Plan: [`docs/plan/compiler/2026-08-15-employee-relations-query.md`](../../plan/compiler/2026-08-15-employee-relations-query.md).
> Depends on iteration 9's engine plan
> ([`2026-08-01-db-engine-binding.md`](../plans/2026-08-01-db-engine-binding.md))
> for storage, WAL, insert and the select subset.

**One sentence:** queries are written in the language, checked by the compiler,
and lowered to bytecode loops over a handful of engine cursor builtins — no SQL
text exists anywhere in a compiled program — and the acceptance workload is a
new `docs/examples/employee` sample whose report mode is one `GROUP BY` after
another.

---

## 1. The three forks, settled

### Fork 1 — the SQL/Cypher layer is superseded as the program surface

`docs/runtime/database/02-wo-language.md` specifies a query layer of literal
SQL and Cypher with five fixed-glue rules. That layer is **not built on the C
stack**. The language-integrated surface below is the only way a `.wo` program
queries its tables.

Reasons, in order of weight:

- The iteration's own acceptance forbids the alternative: a query must lower to
  engine operations "not a string handed to a parser at runtime — provable by
  disassembly." A resident SQL parser and a compile-checked surface would be
  two grammars for one meaning, and the second grammar would re-introduce at
  runtime every error class the first one eliminated at compile time.
- The language's identity is compile-time checking (the WO-E diagnostic
  catalogue). "A typo in a field name is a compile error" cannot be delivered
  by a text layer.
- A binary that ships a SQL parser it uses only for its own programs pays
  image size and attack surface for nothing.

What survives: `02-wo-language.md` stays as design history and as the
specification the `prototypes/wo-db` C++ prototype implements; that prototype
remains the **engine-semantics** reference (what an index probe returns, what
unique violation means), not a syntax reference. SQL text remains a candidate
*export/interop* format for much later (external tools speaking to a writeonce
service), explicitly not part of this iteration.

### Fork 2 — comprehension syntax, desugared at compile time

writeonce has no function values, so LINQ's method-chains-taking-lambdas are
unwritable. The surface is a **query expression** — a comprehension the
compiler desugars — where every predicate and projection is ordinary
expression syntax with the range variable in scope:

```wo
let seniors = from e in Employee
              where e.salary > 90000
              order by e.salary desc
              select e;

let by_dept = from e in Employee
              group e by e.dept into g
              select { dept: g.key.name, headcount: count(g), avg_salary: avg(g.salary) };
```

(Illustrative; the grammar is normative in prose, section 3.)

Why comprehension and not chaining: a chained `.where(e.salary > 100)` has no
binding site for `e` — the comprehension's `from e in` clause is what
introduces the variable, which is exactly the property that makes every later
clause an ordinary typed expression the existing typechecker can check. C# had
to add query expressions *on top of* lambdas for the same readability reason;
we get to skip the lambda layer entirely. The predicate is compile-time
syntax, never a runtime closure — which is also what lets the whole query
lower to plain bytecode.

### Fork 3 — what each reference contributes

**System.Linq** (surveyed 2026-08-15) contributes the operator vocabulary and
its semantic edge cases, not machinery:

- The lowering target for `group … select aggregate` is the shape of
  `AggregateBy`/`CountBy` and the `GroupBy(key, resultSelector)` overload:
  **group-and-reduce as one node, no intermediate group object materialized**
  (`Grouping.cs:63`, `AggregateBy.cs`). The `IGrouping`-returning overloads
  exist to hand groups around as values; we have no delegates to hand them to,
  so groups surface only as the `into g` binding inside the query itself.
- Empty-source rules per aggregate (section 4's table) adapt LINQ's — where
  C# throws `InvalidOperationException`, writeonce answers `nil` through a
  `?T` result, because an empty group is data, not a fault.
- Ordering is **stable**, guaranteed — LINQ enforces it with an original-index
  tiebreak (`OrderedEnumerable.cs:432`); we adopt the same guarantee and the
  same escape hatch (an unstable sort is legal when the key is a scalar and
  rows are not identity-bearing, `OrderBy.cs:144-163` precedent).
- Join semantics: build a hash on the inner side, probe with the outer, and
  **nil never joins** — LINQ achieves SQL's NULL-never-matches by refusing to
  insert null keys into the build side (`Lookup.cs:119-122`); we do the same.
  In grouping, by contrast, nil **is** a legitimate key (`Lookup.cs:207`).

**PostgreSQL** (surveyed 2026-08-15) contributes execution vocabulary for the
engine side:

- Aggregate execution is the transition/finalize split from `nodeAgg.c`: a
  per-group transition value advanced once per row, a finalize step converting
  state to result (`avg` carries sum+count without the executor knowing). The
  `noTransValue` vs `transValueIsNull` distinction — "no row seen yet" is not
  "the value is nil" — is adopted verbatim; it is what makes nil-skipping
  aggregates correct without special-casing the first row.
- Grouping strategy: hash aggregation (`AGG_HASHED`) is the only strategy this
  iteration builds; sorted grouping is an optimization for later. Project the
  input to only the columns the aggregate needs before hashing
  (`find_hash_columns` precedent).
- Referential integrity semantics from `ri_triggers.c`, mechanism discarded:
  an FK check is a **direct probe of the referenced table's primary index**
  (never query text); a nil `ref` passes the check (MATCH SIMPLE rule); an
  update that does not change the key skips the check
  (`RI_FKey_fk_upd_check_required` precedent). Enforcement action is
  **restrict only** — deleting a Department that still has Employees traps;
  cascade/set-nil are out of scope.
- Everything MVCC, buffer-manager, lock-manager and cost-planner shaped is
  explicitly non-transferable: single-writer-per-shard RAM-authoritative
  storage designed those problems away (iteration 9's doctrine).

---

## 2. Relations become typed navigation

The declaration vocabulary already exists in the samples and partially in the
compiler; this iteration makes it mean something:

- `dept: ref Department` — a foreign key. Stored as the target's row id (a
  scalar column, already the compiler's classification). **Navigating** it in
  an expression (`e.dept.name`) typechecks with `Department`'s field set in
  scope and lowers to a primary-index point read.
- `staff: backlink Employee.dept` — the declared inverse. Not a stored column;
  reading it (`d.staff`) is a secondary-index scan of `Employee`'s `dept`
  column and yields `multi Employee`. Declaring a `backlink` whose target
  field is not a `ref` to this class is a compile error.
- `?ref Department` — an optional relation; nil stores as id 0, never probes,
  never joins.

Integrity, enforced at the engine's row choke points (iteration 9's index
doctrine — nothing touches storage except the row API):

- Insert/update of a non-nil `ref`: probe the referenced primary index; a miss
  traps with a foreign-key violation code (sibling of iteration 9's
  unique-violation trap).
- Delete of a row that a non-nil `ref` still points at: trap (restrict). The
  check is a probe of the secondary index that the `backlink` already
  requires, so restrict costs one lookup and no new structure.

---

## 3. The query surface (normative, in prose)

A **query expression** is an expression. Its clauses, in the only order they
may appear:

1. `from <var> in <source>` — required, first. Source is a table (class name),
   a `backlink` navigation, or a `multi` value. Introduces the range variable.
2. `where <bool-expr>` — optional, repeatable. Ordinary boolean expression
   over range variables; each `where` is a filter.
3. `join <var2> in <source2> on <expr> == <expr2>` — optional. Equi-join only;
   one side references only the outer variable, the other only the joined one.
   Most joins in practice are spelled as `ref` navigation instead; explicit
   `join` exists for joining on non-relation columns.
4. `group <expr> by <key-expr> into <g>` — optional. Ends the scope of the
   range variables and opens the scope of `g`. `g.key` is the key's value.
   For any field `f` of the grouped element, `g.f` names the **column of
   members** — legal only inside an aggregate call. Nil is a legitimate key.
5. `order by <expr> [desc]` — optional, comma-repeatable keys. Stable.
6. `take <int-expr>` / `skip <int-expr>` — optional.
7. `select <expr>` — required, last. The result element: a whole row, a single
   field, or a projection literal `{ name: expr, … }` whose type the compiler
   synthesizes as an anonymous record. Using a column the projection dropped
   is a compile error thereafter (the 9b acceptance's projection criterion).

A query's value is `multi <element>`; a query wrapped directly in a whole-
query aggregate (`count(from …)`) is that aggregate's scalar. Execution is
eager at the point the expression is evaluated — no deferred queries, no query
values passed around (that would be a function value in disguise).

Aggregates are compiler-recognized **clause functions**, legal over a group
binding or a whole query: `count(g)`, `count(from …)`, `sum(g.f)`, `avg(g.f)`,
`min(g.f)`, `max(g.f)`. They are not general functions; naming one outside a
query is the existing unknown-identifier error.

Diagnostics this surface owns (new WO-E5xx range): unknown table, unknown
column (naming the class), relation navigated through a non-`ref` field,
aggregate outside a query, group member column used bare outside an aggregate,
join sides not separable, projection field name collision.

---

## 4. Aggregate semantics (normative table)

| Aggregate | Input | Result type | Empty source | Nil elements (`?T` column) |
|---|---|---|---|---|
| `count(g)` | group/query | `Int` | `0` | counted (row exists) |
| `sum(g.f)` | `Int` column | `Int` | `0` | skipped |
| `avg(g.f)` | `Int` column | `?Int` | `nil` | skipped; all-nil ⇒ `nil` |
| `min(g.f)` / `max(g.f)` | `Int` or `Text` column | `?T` | `nil` | skipped; all-nil ⇒ `nil` |

Decisions behind the table:

- **Empty is data, not a fault.** LINQ throws on empty `Average`/`Min`/`Max`
  over non-nullables; writeonce has `?T` and a nil-forcing style already, so
  the nullable-column LINQ behavior (`null` on empty, skip nils, all-nil ⇒
  `null` — `Average.cs:165`, `Min.cs:47-56`) is the behavior for everyone.
  Traps stay reserved for integrity violations.
- **`sum` wraps.** The VM's ADD wraps two's-complement; `sum` is a loop of
  ADDs and inherits that. Documented, consistent with language arithmetic,
  and the alternative (checked overflow, LINQ's `Sum.cs:40`) would make `sum`
  the only trapping arithmetic in the language.
- **`avg` truncates** toward zero (integer division), same as `/`.
- Execution is the transition/finalize ABI (section 1, fork 3): one opaque
  transition slot per (group, aggregate), advanced per row, finalized per
  group when the hash table drains.

---

## 5. Lowering and execution model

A query compiles to an **ordinary bytecode loop** — the same opcodes every
`for` loop uses — over a small set of new engine cursor builtins. There is no
plan-tree interpreter in the VM: the compiler *is* the planner, at the only
scale this iteration promises (index selection, no cost model).

- **Scan**: table cursor builtins (open by class id, advance, read row into a
  register as a borrowed row view). `where` clauses are ordinary compiled
  boolean expressions guarding the loop body — the flattened-expression
  lesson of PostgreSQL's executor (`execExpr.c`) is in our case simply "the
  bytecode we already generate."
- **Index selection**: if the leading `where` conjuncts equality-match a
  declared index's prefix (longest-prefix rule, iteration 9's `find_by`), the
  compiler emits an index-probe cursor instead of a scan, and the acceptance
  demonstrates the difference (a probe counter the engine exposes in debug
  builds — "demonstrated, not assumed").
- **`ref` navigation** lowers to a primary-index point-read builtin.
  **`backlink`** lowers to a secondary-index cursor over the ref column.
- **`group … into g … select`** lowers to the two-phase hash-aggregation
  shape: phase one iterates the source, keying a group hash table and
  advancing transition slots (builtins: group-table create / upsert-advance);
  phase two drains the table, runs finalize, evaluates the `select`
  projection per group. One node, no intermediate group objects — the
  `AggregateBy` shape.
- **`join`** lowers to build-inner/probe-outer against a transient hash
  keyed on the join column; nil keys are never inserted and never probe.
- **`order by`** materializes the result `multi` and stable-sorts it; `take`/
  `skip` slice afterwards (a partial-sort fast path is recorded as a later
  optimization, LINQ `SpeedOpt` precedent).
- **Ownership**: rows read from a table are engine-owned; anything a query
  *returns* is copied out at the select boundary under Task-2's established
  rule — a Text crossing an ownership boundary is copied, a projection record
  is freshly built and caller-owned. Queries introduce no new ownership
  classes.

Format consequences: new builtin ids appended to the format doc's table (the
cursor/group/probe set), no new opcodes, no version bump beyond iteration 9's.

---

## 6. Ownership, borrows, and GC across the engine boundary

The engine and the VM heap are two memory worlds, and the whole safety story
is that values only ever CROSS between them by copy. Analysis recorded here
because both iterations' correctness hangs on it (2026-08-15).

### The bulkhead: two one-way gates, both already doctrine

- **Into storage:** a row stores no VM pointer — scalars copy, Texts copy,
  owned objects flatten by value, containers copy element-wise, `ref` is an
  id, and a GC-managed value in a `@table` field is a **compile error**
  (iteration 9's field-encoding rules). So no row ever points at a GC object.
- **Out of storage:** everything a `select` emits is copied or freshly built
  at the boundary — Texts via the established copy rule, projections as new
  records. So no GC root, no local, and no container ever points into a row
  slab once the query ends.

Consequence: **the collector never traces engine memory and the engine never
touches reference counts.** Iteration 7b (inferred GC, mark-sweep) does not
change this — it changes only *when* the "into" gate's error fires: GC-ness
becomes inferred, so inference must classify every class **before** table-field
validation runs, and the diagnostic reads "class X is garbage-collected
(inferred via Y) and cannot be stored in a table field." A class stored in a
table is thereby constrained to ownership-expressible shapes; that is a
feature, not a limitation — tables are the language's answer to shared
long-lived data, which is most of what `@gc` exists for.

### Row views are borrows without a runtime net

A cursor yields a **row view**: a borrow of engine-owned memory, valid until
the cursor advances or closes. Two things make this different from every
borrow the language has today:

- VM-heap borrows have a runtime defense (the object header's borrow word,
  `WO_T_BORROW` traps). Rows share the VM's field *encoding* but not its
  header — there is no borrow word in a row slab, so **the compile-time rule
  is load-bearing alone**. The ownership pass enforces: a row view never
  escapes the query loop that produced it (the container-read-borrow mirror),
  and anything that leaves does so as a copy through `select`.
- The program can mutate the table it is iterating — single-writer per shard
  removes concurrent writers, not the program's own hand.

### Cursor stability: materialize ids, allow row updates, forbid structural

The `raise` mode is the honest case: it updates `salary` — an **indexed**
column — while iterating an index scan. Naive cursor-over-index breaks here
(entries move mid-scan). The semantics, chosen for KISS and enforceability:

- **A scan materializes its matching id list before the body runs**, then
  point-reads each row per iteration. O(matches) ids of memory, recorded as
  the cost; index-order iteration falls out for free.
- **Updates through the row view are allowed** — the view is an exclusive
  borrow of that row for the iteration (the `mut` analog); index maintenance
  for the changed column happens at the row API as always, and cannot disturb
  the already-collected id list.
- **`insert` into or `delete` from a table with an open cursor is a compile
  error** (new WO-E5xx): a materialized id list cannot defend a point-read
  against a row deleted mid-loop, and silently skipping a vanished id is the
  kind of quiet wrongness this language exists to refuse. The ownership pass
  carries an open-cursor table set through the loop body, statically — insert
  and delete name their target class at compile time. Read-only nested
  queries over the same table remain legal (shared borrows).

### Query temporaries and GC pressure

Group hash tables and join build sides are engine-side C allocations scoped
to the statement — freed when the query ends, invisible to both the drop
tables and the collector. Query results are ordinary owned VM values, freed
by the existing drop machinery. Nothing on the query path allocates a GC
object or an RC operation. One accepted interaction: the budgeted collector
runs between statements, so a long full-table scan delays GC slices for its
duration — acceptable at this scale, recorded so nobody rediscovers it as a
latency mystery.

## 7. Acceptance workload — `docs/examples/employee`

A new sample, structured like `log-watcher` (wo.toml manifest, program mode,
`just` module, acceptance script), small enough to read in one sitting and
shaped so every 9b feature is load-bearing:

- **Types**: `Department` (`@table(name: "departments", index: [name])`,
  `name: Text @unique`) and `Employee` (`@table(name: "employees",
  index: [dept], index: [dept, salary])`, `name: Text`, `salary: Int` cents,
  `hired: Int` ms, `dept: ref Department`); `Department.staff: backlink
  Employee.dept`.
- **Modes** (CLI, exit codes per the program-mode contract):
  - `seed` — inserts departments and employees; proves insert + WAL + unique
    trap (second `seed` run reports the duplicate-department trap caught).
  - `report` — the GroupBy showcase: headcount by department, average /
    min / max salary by department, total payroll, departments ordered by
    average salary — each line's expected output is byte-exact in the
    acceptance script.
  - `staff <department>` — relation navigation both directions: finds the
    department by unique name (index probe), lists its `staff` backlink
    ordered by salary; also prints each employee's `e.dept.name` to prove
    forward navigation.
  - `raise <department> <pct>` — update through a query result; re-running
    `report` shows the moved averages; a `raise` for a missing department
    exercises the empty-query path (`avg` ⇒ nil).
- **Integrity demo**: deleting a department with staff traps (restrict);
  the acceptance asserts the trap code.
- **Crash step**: kill -9 between `seed` and `report`, re-run `report` —
  iteration 9's WAL replay proven on this workload too.

The sample is 9b's acceptance the way log-watcher was iterations 1–7's: no new
corpus fixtures beyond the db corpus iteration 9 already plans; the sample is
the test.

## 8. Out of scope (inherited and new)

- Everything 9b's story already excludes: cross-shard queries and distributed
  joins, `LIVE` subscriptions, migrations, cost-based planning.
- The `IGrouping`-as-value surface (groups escaping their query), `Distinct`/
  set operators, outer joins (`LeftJoin` family), subqueries in `where`, and
  `group by` composite keys beyond a single expression — each waits for a
  workload that demands it.
- FK actions other than restrict (cascade, set-nil), deferred constraint
  checking (the after-trigger queue pattern is recorded for when transactions
  span statements).
- SQL text in any runtime role.
