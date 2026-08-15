# `@table`, Relations, Query — Implementation Plan (employee sample)

> **Status: ⬜ pending** (story iteration 9b) — blocked on iteration 9's engine
> plan ([`2026-08-01-db-engine-binding.md`](../../superpowers/plans/2026-08-01-db-engine-binding.md)):
> Tasks 3–6 below consume its row storage, WAL, indexes and select subset.
> Board: [00-status.md](../../00-status.md)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
>
> **Style rule (user convention):** concept, reason, and required behavior in words only; the executor writes the code.

**Spec:** [`docs/superpowers/specs/2026-08-15-table-relations-query-design.md`](../../superpowers/specs/2026-08-15-table-relations-query-design.md) (normative: the settled forks, the clause grammar, the aggregate semantics table, the lowering model), amended by the systems-track spec's program-mode contract for the sample's CLI.

**Goal:** `@table` classes queried **in the language**: comprehension queries
with `where`/`join`/`group`/`order`/`take`/`select`, typed `ref`/`backlink`
navigation with restrict integrity, and the LINQ aggregate vocabulary
(`count`/`sum`/`avg`/`min`/`max`, `GroupBy` as group-and-reduce) — all lowered
to bytecode loops over engine cursor builtins, proven by a new
`docs/examples/employee` sample whose acceptance script is the gate.

**Architecture:** compiler front (`compiler/src/{lexer,parser,ast,types,owner,emit}.ml`)
for the query surface; the database engine (`database/src/` — its own
top-level directory per the 2026-08-15 decision, statically linked into wovm;
`table.c`/`db.c` from iteration 9, plus a new `query.c` for cursors and group
hashing). The compiler
is the planner: index selection happens at lowering, the VM never sees a plan
tree. Reference semantics: System.Linq for operator meaning, PostgreSQL's
nodeAgg/ri_triggers for execution and integrity vocabulary (both surveyed in
the spec, fork 3).

**Tech Stack:** OCaml stdlib (compiler), C11 libc (runtime). No new opcodes;
new builtin ids appended to the format doc.

## Global Constraints

- **The sample is the test.** `docs/examples/employee` plus its acceptance
  script is 9b's gate; the only corpus additions are the db-corpus fixtures
  iteration 9 already plans. `just oop-e2e`, `just woc-test`, `just wovm-test`
  stay green as regression after every task.
- **Index doctrine** (iteration 9, verbatim): secondary indexes are maintained
  only through the engine's row choke points; FK checks and backlink reads are
  index probes, never storage walks.
- **No SQL text anywhere** — the 9b acceptance's disassembly criterion.
- **No function values.** Every predicate/projection is expression syntax; a
  query value is never deferred or passed around.
- Plans/specs in words; commits local only, never push; docs under `docs/`;
  CODE-LOGIC.md updated beside changed code.

## File Structure

```
compiler/src/lexer.ml parser.ml ast.ml   query-expression grammar, clause AST (Task 1)
compiler/src/types.ml                    range/group scopes, navigation, projection synthesis (Task 2)
compiler/src/owner.ml emit.ml            query ownership + lowering to cursor builtins (Task 5)
database/src/query.c query.h             cursors, group hash, transition/finalize aggregates (Tasks 3, 4)
database/src/db.c table.c                FK restrict checks at the row choke points (Task 3)
docs/examples/employee/                  the acceptance workload (Task 6)
scripts/employee-accept.sh               the gate (Task 6)
docs/plan/oop-vm/00-wob-format.md        appended builtin ids (Tasks 3-5)
```

---

### Task 1: Query expressions parse

**Concept & reason:** the comprehension grammar from spec section 3 —
`from`/`where`/`join`/`group…into`/`order by`/`take`/`skip`/`select`, clause
order fixed, aggregates as clause functions — becomes lexer keywords (contextual,
so `from`/`group`/`order` stay legal identifiers outside a query), a clause AST,
and parse diagnostics in a new WO-E5xx range (clause out of order, missing
`select`, aggregate named outside a query). Fixed clause order is a parse rule,
not a type rule, so the error lands on the exact token.

- [ ] Grammar and AST for every clause; contextual keywords verified against
      the existing samples (no `.wo` file in the tree breaks).
- [ ] WO-E5xx diagnostics with golden coverage in the existing `woc-test`
      suite (dump-ast goldens for well-formed queries, diagnostic goldens for
      each malformed shape).
- [ ] Gates green; commit locally.

### Task 2: Queries typecheck

**Concept & reason:** the surface's whole promise is compile-time checking.
`from e in Employee` opens a scope where `e`'s fields are the class's; `ref`
navigation substitutes the target class's field set (`e.dept.name`);
`backlink` reads type as `multi` of the source class; `group … into g` closes
the range scope and opens the group scope, where `g.key` has the key's type
and `g.f` is legal **only** inside an aggregate call; `select { … }`
synthesizes an anonymous record type so later use of a dropped column is a
compile error. Aggregate result types follow the spec's table exactly —
`count` is Int, `sum` Int, `avg`/`min`/`max` are `?T` because an empty group
is data. Unknown table, unknown column (naming the class), navigation through
a non-`ref`, bare group-member column, join sides not separable: each is its
own WO-E5xx with a golden.

- [ ] Scope machinery for range/group variables; navigation typing both
      directions; projection record synthesis interned like other class
      shapes.
- [ ] Aggregate typing per the spec table; `?T` results force the existing
      nil-handling style at use sites.
- [ ] Diagnostic goldens for every error class named above; gates green;
      commit locally.

### Task 3: Cursors and integrity in the engine

**Concept & reason:** the runtime side queries need, built on iteration 9's
storage: a table-scan cursor (open by class id, advance, borrowed row view),
a primary-index point read (`ref` navigation), a secondary-index cursor with
longest-prefix probe (backlink reads, indexed `where`), all as builtins
appended to the format doc. Integrity lands at the row choke points where the
indexes already live: inserting/updating a non-nil `ref` probes the referenced
primary index and traps on a miss (foreign-key violation, sibling of the
unique trap); deleting a row still referenced traps (restrict) via the same
secondary index a `backlink` requires — one probe, no new structure. Nil
`ref` never probes (the MATCH SIMPLE rule); an update that leaves the key
unchanged skips the check (both from the PostgreSQL RI survey, mechanism
discarded — a check is an index probe, never query text).

- [ ] Cursor builtins + borrowed-row-view lifetime rules written into the
      binding doc (a row view never escapes the loop that opened the cursor —
      the ownership pass enforces it, mirror of the container-read borrow).
      **Rows have no borrow word** (they share the VM's field encoding, not
      its header), so unlike every VM-heap borrow there is no runtime trap
      behind this rule — the compile-time check is load-bearing alone, which
      is why it gets its own diagnostic and goldens rather than riding on
      E30x.
- [ ] Cursor stability per spec section 6: scans **materialize their id list**
      before the body runs and point-read per iteration; updates through the
      row view stay legal (exclusive row borrow, index maintenance at the row
      API); `insert`/`delete` targeting a table with an open cursor is a new
      WO-E5xx (the ownership pass carries the open-cursor table set through
      the loop body); read-only nested queries over the same table stay legal.
      The `raise` mode — updating an indexed column mid-scan — is the fixture
      that proves the materialized-id semantics.
- [ ] FK trap + restrict trap wired through the row API; a debug-build probe
      counter exposed for Task 5's index-selection proof.
- [ ] The `delete` statement (point delete of a row value) lands here too:
      iteration 9's subset is insert/select/update-point, and restrict has
      nothing to restrict without it — same typed-AST-plus-builtin shape as
      insert, WAL remove record already specified by iteration 9's Task 2.
- [ ] db-corpus fixtures from iteration 9's plan extended with one FK-violation
      and one restrict fixture (trap-code exact); ASan green; commit locally.

### Task 4: Group hash + aggregate execution

**Concept & reason:** the `AggregateBy` shape from the spec — group-and-reduce
in one pass, no group objects. A group hash table builtin set: create (keyed
by the group key's kind, nil legal), upsert-advance (locate-or-create the
group, advance each aggregate's transition slot), drain (iterate groups,
finalize, hand key + finals to the compiled projection loop). Transition/
finalize is PostgreSQL's split: `avg` carries sum+count in its transition
state and divides at finalize; the "no row seen yet" state is distinct from
"transition value is nil" so nil-skipping aggregates need no first-row special
case. Aggregate semantics are the spec's table: empty ⇒ nil (or 0 for
count/sum), nil elements skipped, sum wraps, avg truncates.

- [ ] Builtins implemented; the input projected to only the columns the
      aggregates read before hashing (the nodeAgg memory lesson).
- [ ] Fixture-level verification through iteration 9's db corpus (one grouped
      query, exact output) plus the sample in Task 6; ASan green; commit
      locally.

### Task 5: Lowering — the compiler is the planner

**Concept & reason:** a query desugars to the bytecode loops the language
already has. Scan or probe chosen at compile time: leading `where` equality
conjuncts matched against declared indexes longest-prefix-first, probe emitted
on a hit, scan otherwise — and the choice is **demonstrated** via Task 3's
probe counter in the acceptance, not assumed. `join` builds a transient hash
on the inner side and probes with the outer (nil never inserted, never
probes). `group` emits the two-phase hash-aggregation loop from Task 4.
`order by` materializes and stable-sorts (original-index tiebreak — the LINQ
stability guarantee); `take`/`skip` slice the result. Ownership: row views
stay loop-bound borrows; everything `select` emits is copied/built at the
boundary under the established Text-copy and fresh-value rules — queries add
no new ownership classes, and the ownership pass's existing drop machinery
covers the query's temporaries because the lowering IS ordinary loops.

- [ ] Desugar + lowering for every clause; disassembly of the sample's report
      mode shows loops and builtins, no plan tree, no text. The select
      boundary is the ownership bulkhead (spec section 6): everything a query
      returns is copied or freshly built, so no value anywhere points into a
      row slab after the query ends — asserted under ASan by mutating rows
      after a query and re-reading the query's results.
- [ ] Index selection proven: the acceptance asserts probe-counter deltas for
      the indexed `staff <department>` path versus a full-scan query.
- [ ] `oop-e2e`, `woc-test`, ASan-corpus gates green; commit locally.

### Task 6: The employee sample is the acceptance

**Concept & reason:** spec section 7 verbatim — `docs/examples/employee` with
`Department`/`Employee` (`@table`, `@unique` name, composite `[dept, salary]`
index, `ref`/`backlink` pair), wo.toml manifest so `woc .` builds it, a `just`
module beside it (the log-watcher convention), and `scripts/employee-accept.sh`
as the gate: `seed` (insert + WAL + duplicate-department trap on the second
run), `report` (headcount/avg/min/max by department, total payroll, ordered by
average salary — byte-exact lines), `staff` (both navigation directions +
index-probe proof), `raise` (update through a query; missing department ⇒ the
empty-query nil path), the restrict-trap demo, and a kill -9 between `seed`
and `report` proving replay on this workload.

- [ ] Sample source pre-authored 2026-08-15 (`docs/examples/employee/` —
      the target workload, sample-first like log-watcher was); this task makes
      `woc docs/examples/employee` compile it with zero diagnostics and the
      binary's modes run. The sample is authoritative: divergence between it
      and the spec is resolved in the spec's favor and committed.
- [ ] `scripts/employee-accept.sh` + `just employee` module: every mode
      checked with exact expectations, trap codes asserted, crash step
      included; soak-style RSS/fd sampling reused from the log-watcher
      script's pattern for the `report` loop.
- [ ] ASan run of the full acceptance: zero leaks (the log-watcher bar).
- [ ] Docs: README beside the sample, CODE-LOGIC.md updates beside changed
      compiler/runtime code, format-doc builtin table final, board + story
      rows updated with measured numbers; commit locally.

## Out of scope — deferred by name

- Everything spec section 8 lists: set operators, outer joins, subqueries,
  composite group keys, groups as values, FK cascade/set-nil, deferred
  checks, SQL text in any role, cross-shard queries, `LIVE`, migrations,
  cost-based planning.
- Sorted-grouping and partial-sort optimizations (recorded LINQ/postgres
  precedents; hash + full stable sort are this plan's only strategies).
- The ecommerce sample's query rewrite (9b's fifth acceptance criterion) —
  it lands as its own follow-up once the employee gate is green, so this
  plan's blast radius stays one new sample.
