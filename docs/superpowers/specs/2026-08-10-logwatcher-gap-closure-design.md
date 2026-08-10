# log-watcher gap closure — spec amendments + plan reconciliation

**Date:** 2026-08-10
**Status:** approved design, pre-implementation
**Scope:** amending the systems-track spec with four surface gaps the log-watcher
sample revealed, recording three scope cuts, correcting two false status claims,
and retiring a parallel roadmap
**Amends:** [`2026-08-01-systems-track-design.md`](2026-08-01-systems-track-design.md)
(Part 1 verdict table, Part 3 stdlib table)
**Supersedes:** `docs/00-code-review.md` (extracted, then reduced to a stub)
**Companion specs:** [`2026-08-01-oop-compiler-vm-design.md`](2026-08-01-oop-compiler-vm-design.md)
**Affected plans:** [plan 8](../../plan/compiler/2026-08-01-haxe-parity-language.md),
[plan 9](../plans/2026-08-01-program-mode-stdlib.md),
[plan 10](../plans/2026-08-01-log-watcher-sample.md)

## Motivation

`docs/00-code-review.md` was written as a gap analysis: what must exist before
the log-watcher sample compiles. Verified against the code, most of its
"missing" rows are correct — the front end genuinely cannot lex or parse roughly
200 constructs the sample uses. But it also carried three false claims, omitted
the single largest piece of work, and proposed a Phase 1–4 roadmap that competes
with the approved story iterations.

Its real contribution is four surface gaps that **no approved spec ever named**.
Those are the substance of this amendment. The competing roadmap is retired; the
false claims are corrected at their sources.

Measured baseline, `woc docs/examples/log-watcher` over 7 files:
**307 diagnostics** — 167 `WO-E101` (parse) + 140 `WO-E207` (unknown type).
Iteration 7 closes when that number is zero.

## Decisions locked during brainstorming

| Question | Decision |
| --- | --- |
| Status of `docs/00-code-review.md` | **Scratch input.** Findings extracted here; the file becomes a stub pointing at `docs/00-status.md`. Its Phase 1–4 roadmap is retired — story iterations 4→5→6→7 remain the only sequence. |
| Shape of the text/collection builtins | **Bare globals, no import.** `len`, `push`, `split_ws` … are always in scope, like the existing `print`/`now`/`count`/`latest`. Capability modules (`fs`, `proc`, `net`, `time`, `json`, `env`) stay `use`-imported and qualified. |
| `throw` | **Cut** from the critical path — 0 uses in the sample. |
| `time.mono` | **Cut** — 0 uses. |
| `is` | **Cut** — 0 uses. Empties plan 8 Task 7, whose `abstract` half was rejected 2026-08-10, so the task is deleted rather than deferred. |
| `#if` build flags | **Kept** in plan 8 Task 9 despite 0 uses. |
| Emitter sequencing | **Unchanged: iteration 4 before iteration 5.** Plan 8's tasks state that features "lower onto existing opcodes" and name exactly three fenced VM changes — wording that presupposes an emitter. Growing the surface first would force a much larger emitter later. |

## 1. Amendment — core builtins (new Part 3 section)

The sample calls **22 unqualified builtin names across ~176 sites**, none of
them in any spec: `len` ×55, `push` ×17, `byte_at` ×11, `starts_with` ×9,
`index_of` ×8, `has` ×7, `split` ×6, `split_ws` ×6, `join` ×5, `parse_int` ×5,
`trim` ×4, `slice` ×4, `substr` ×4, `pop` ×3, `ends_with` ×2,
`last_index_of` ×2, `to_lower` ×2, `sort` ×2, `char_of` ×1, `shift` ×1,
`remove` ×1, `reverse` ×1.

Part 3 gains a **core builtins** section, distinct from the capability modules:

- **Always in scope.** No `use` line, no namespace — the same status the
  existing `print`, `print_int`, `now`, `words`, `count`, `latest` builtins
  already have, and the same flat `WO_B_*` id space in the VM's builtin table.
- **Rationale.** The sample committed to this spelling at 176 sites before the
  spec had an opinion, and "samples force the grammar" (principle 8) makes that
  binding. Qualifying them (`text.len`) would add an import line to every file
  and buy nothing: these are language primitives, not capabilities — they touch
  no syscall, need no audit, and cannot be refused.
- **Grouping** (documentation only, not namespaces): text operations,
  collection operations, map operations.
- **Contract per builtin** is fixed arity with typed parameters, resolved at
  compile time like every other builtin; out-of-range indices trap
  (`T_BOUNDS`), never return a sentinel.
- `print_err` joins this set (Part 2 already named it; Part 3 never listed it).

**Deliberately not adopted:** iteration/closure builtins (`map`, `filter`,
`reduce`) — the sample uses explicit loops throughout, and adding higher-order
functions would require a function-value type the language does not have.

## 2. Amendment — `time` gains calendar surface

Part 3's `time` row lists `now()`, `mono()`, `sleep(ms)`. The sample also calls:

| Builtin | Sites | Why |
| --- | --- | --- |
| `time.iso(ms) -> Text` | 2 | JSONL detection timestamps and MCP response fields need a stable textual instant |
| `time.local(ms) -> {year, month, day, hour, minute, dow}` | 1 | cron next-fire computation needs broken-out calendar fields, including day-of-week |

Both are added. `mono()` is **cut** (0 uses) and returns when a workload needs
monotonic math. The record `time.local` returns is a plain value record, so it
needs no new type machinery beyond the `typedef` records plan 8 Task 4 already
lands.

## 3. Amendment — boolean operators (new verdict-table row)

The sample uses `and` at 35 sites and `or` at 20. Neither exists in the
language, and — the reason this went unnoticed — **neither the verdict table nor
any plan ever mentioned boolean operators at all**, in either spelling: there is
no `&&`/`||` row, and Haxe's own operators were never enumerated.

The verdict table gains one row: **`and` / `or` — adopt**, spelled as words
rather than `&&`/`||`.

- **Spelling rationale.** The sample chose words; the lexer has no `&` case at
  all (a bare `&` reports `WO-E001`), so words cost nothing to add as keywords,
  and they read better in the conditional-heavy code the sample is full of.
- **Precedence.** One new level **below** comparison and above assignment:
  `or` binds loosest, then `and`, then comparison, then the existing arithmetic
  ladder. This makes `if a == 1 and b == 2` parse as intended without
  parentheses — the sample's dominant shape.
- **Semantics.** Short-circuit, `Bool`-typed operands only, `Bool` result. No
  truthiness — a non-`Bool` operand is a type error, consistent with principle 13.
- **Lowering.** Compare-and-jump on existing opcodes (`JZ` plus a jump), no new
  opcode and no VM change.

## 4. Amendment — `env` is the sixth module

Part 2 specifies `env.args()`, `env.get(name)`, `env.exit(code)`,
`env.stopping()`; Part 3's table omits `env` while calling itself "five builtin
modules". The sample uses `env.get` ×1 and `env.stopping` ×4. Part 3's table
gains an `env` row and the count becomes six.

## 5. Corrections to false status claims

Both were asserted in the review doc; both are corrected at their real source
rather than in the retired file.

**Structural interface satisfaction is not implemented — and is unreachable by
design, not owed.** `WO-E205` is declared and never emitted. The review listed
satisfaction checking as *implemented*, which is false; but calling it a gap in
shipped work is equally wrong. Satisfaction is structural — there is no
`implements` keyword by doctrine — so the check has exactly one home: sites
where a value is used at an interface-typed position. The milestone grammar has
no such positions (no interface-typed fields, parameters, or returns are
exercised), so the check cannot fire yet and its absence costs nothing. The
error catalog re-files `WO-E205` as **unreachable until interface-typed
positions exist**, and `types.ml`'s module header stops claiming it produces a
satisfaction set. The log-watcher sample declares no interfaces, so this stays
off the critical path.

**`?T` is plumbed, not enforced.** The review listed "Nullable types `?T`" as
implemented — the same conflation already corrected in
`docs/plan/compiler/nullable-types-implementation.md`. No new action; noted here
so the two documents agree.

## 6. What the review omitted — the emitter

The review is titled "Compilation Requirements" and its Phase 4 promises
"end-to-end compile + run", but it never lists the bytecode emitter as work.
There is no `emit.ml`, no byte-writing anywhere in the compiler, no `--emit` or
`-o` flag, and `wob_kind_of_typ` exists but is never called. That is
[plan 3](../../plan/compiler/2026-08-01-wob-emit-e2e-single-binary.md) — story iteration 4,
an entire slice — and it is the current NEXT PLAN.

Consequence for iteration 4's scope, to be stated on the board: the emitter
proves the pipeline on the **milestone grammar only**. The sample's ~200
unparseable constructs are iterations 5–6 work; iteration 4 must not be judged
against the sample.

## 7. Plan updates

| Plan | Change |
| --- | --- |
| [plan 8](../../plan/compiler/2026-08-01-haxe-parity-language.md) | Task 2 gains `and`/`or` (keywords, new precedence level, compare-and-jump lowering, short-circuit, `Bool`-only). Task 5 loses `throw` — catch frames ship without the explicit-raise half, and the error-payload slot plus its `.wob` version note go with it. **Task 7 is deleted** (`is` cut, `abstract` rejected); later tasks renumber. The Goal line drops `is`. |
| [plan 9](../plans/2026-08-01-program-mode-stdlib.md) | Gains a core-builtins task covering the 22 bare globals plus `print_err`. `time` task gains `iso` and `local`, loses `mono`. `env` is named as a module rather than loose Part-2 prose. |
| [plan 3](../../plan/compiler/2026-08-01-wob-emit-e2e-single-binary.md) | Unchanged. |
| [plan 10](../plans/2026-08-01-log-watcher-sample.md) | Acceptance gains the diagnostic-count gate: 307 → 0. |
| [`01-error-catalog.md`](../../plan/oop-vm/01-error-catalog.md) | `WO-E205` re-filed as unreachable-by-design with its reason; `WO-E208`/`E210`/`E211`–`E213` keep their existing reserved entries. |
| [`docs/00-status.md`](../../00-status.md) | NEXT PLAN gains the milestone-grammar-only note; pending list gains the three cuts under the parked section. |
| `docs/00-code-review.md` | Reduced to a stub: one paragraph saying its findings landed here and in the plans, pointing at `docs/00-status.md`. |

## Error handling

Nothing in this amendment adds an error-handling mechanism. `and`/`or` produce
ordinary type errors on non-`Bool` operands (reusing `WO-E201` once that code is
wired). Core builtins trap on out-of-range access rather than returning
sentinels, matching the existing container builtins. Cutting `throw` leaves the
uncaught-trap surface exactly as it is today.

## Testing

- **Per amendment, corpus fixtures in the task that lands it:** `and`/`or` get
  precedence goldens (including `a == 1 and b == 2` without parens),
  short-circuit behavior, and a must-fail for a non-`Bool` operand. Each core
  builtin gets a golden exercising it plus a bounds-trap fixture where indices
  apply. `time.iso`/`time.local` get fixtures against a fixed injected clock so
  output is deterministic.
- **The sample is the integration test.** `woc docs/examples/log-watcher` is run
  at the end of every iteration from 5 onward and its diagnostic count recorded;
  the number must fall monotonically from 307 and reach 0 at iteration 7.
- **No new VM tests** from this amendment except the core builtins' own, since
  `and`/`or` add no opcode and `time`/`env` extend an existing module pattern.

## Success criteria

1. The systems-track spec's Part 1 has a boolean-operator row and its Part 3
   lists six modules plus a core-builtins section covering all 22 names.
2. Plans 8 and 9 reflect every addition and cut; plan 8 Task 7 is gone.
3. `WO-E205` is documented as unreachable-by-design, and `types.ml`'s header no
   longer claims a satisfaction set is produced.
4. `docs/00-code-review.md` is a stub; no second roadmap exists in the repo.
5. The 307-diagnostic baseline is recorded in plan 10 as its acceptance gate.

## Out of scope

`throw`, `time.mono`, `is`, higher-order/iteration builtins, and interface
satisfaction enforcement — each parked with its reason above. Implementing any
amendment is the plans' job, not this spec's.
