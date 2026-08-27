# employee — the database track's acceptance workload

> **Status: shipped — this is the database track's acceptance gate.** Run it
> with `just employee`. The sample was written *ahead of* the features it
> exercises, exactly as log-watcher was written ahead of iterations 5–7: the
> sample is the test, and the plans compiled toward it. Both landed — iteration
> 9 (engine: [`2026-08-01-db-engine-binding.md`](../../superpowers/plans/2026-08-01-db-engine-binding.md))
> and iteration 9b (query surface:
> [`2026-08-15-employee-relations-query.md`](../../plan/compiler/2026-08-15-employee-relations-query.md)).
> Normative semantics:
> [the 9b spec](../../superpowers/specs/2026-08-15-table-relations-query-design.md).
> One clause below is still ahead of the compiler and marked where it appears:
> `group … by … into` parses and is then refused by the typechecker.

Two `@table` classes and every 9b feature load-bearing:

| Mode | What it proves |
| --- | --- |
| `employee seed` | `insert` + WAL-before-ack; a second run catches the `departments.name` `@unique` trap (`SEED-DUP`, exit 3) |
| `employee report` | `group … by … into g` lowered as one hash pass; `count`/`avg`/`min`/`max` per department; `order by avg(g.salary) desc`; whole-query `sum` for payroll |
| `employee staff <dept>` | unique-name **index probe** (asserted via the engine's probe counter, not assumed), `backlink` scan one way, `e.dept.name` ref navigation the other |
| `employee raise <dept> <pct>` | update through a query result; a missing department takes the empty-query path |
| `employee drop <dept>` | delete **restrict**: a department with staff traps (exit 4); the trap code is the acceptance's assertion |

The engine lives in `database/` (statically linked into `wovm` — still one
binary). Rows are RAM-authoritative, WAL-durable; a kill -9 between `seed`
and `report` followed by an identical `report` is part of the acceptance
script (iteration 9's replay, proven on this workload).

Data shape: `Department { name @unique, staff: backlink Employee.dept }`,
`Employee { name, salary (cents), hired (epoch ms), dept: ref Department }`,
indexes `[name]`, `[dept]`, `[dept, salary]`. Salaries wrap like all language
arithmetic; `avg`/`min`/`max` are `?Int` because an empty group is data, not
a fault.
