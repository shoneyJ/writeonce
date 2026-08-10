# Haxe-Parity Language Adoptions Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
>
> **Style rule (user convention):** concept, reason, and required behavior in words only; the executor writes the code.

**Goal:** Plan 8 — implement every **adopt** row of the systems-track spec's Haxe keyword verdict table: the language grows boolean operators (`and`/`or`), switch expressions, records, optionals, try/catch, enum payloads, static members, using-extensions, modules, `pub(read)`, build flags, and interpolation — with the reject rows enforced as diagnostics. (`abstract` was an adopt row until 2026-08-10; it is now a reject row. `is` was cut the same day — 0 uses in the driving workload, parked post-iteration-12 — emptying this plan's old Task 7, which is deleted rather than deferred.)

**Architecture:** Plan 8 of the roadmap. Depends on OOP plans 1–3 (woc + wovm + corpus). Overwhelmingly compiler work in `compiler/src/`; the VM changes are exactly three, called out in their tasks: catch frames (try/catch), variant objects (enum payloads), and boxed optionals for scalars. Everything else lowers onto existing opcodes. The spec's verdict table (`docs/superpowers/specs/2026-08-01-systems-track-design.md` Part 1) is normative — this plan sequences it.

**Tech Stack:** OCaml stdlib (compiler), C11 libc (the three VM changes), conformance corpus.

## Global Constraints

- All OOP-track constraints carry over (stdlib-only OCaml, libc-only C, no commits — drafts to `.dev/commit.md`, ASan gate, docs under `docs/`).
- **The verdict table is normative:** adopt rows land exactly as specified; reject rows produce diagnostics where they would parse (`extends`, `cast`, `Dynamic` as a type name) or stay absent where they would not.
- **Every adoption ships with corpus fixtures** (golden run + must-fail) and an error-catalog entry for its new `WO-E` codes, in the same task.
- **Doctrine unbroken:** no inheritance, no `Dynamic`, no macros — a task that finds itself needing one has found a plan defect; stop and ask.
- **Format doc governs VM changes:** catch frames, variant layout, and boxed optionals each update `docs/plan/oop-vm/00-wob-format.md` in the task that introduces them.

---

## File Structure

```
compiler/src/            lexer/parser/types/owner/emit extensions per task
runtime/src/vm.c         catch frames (Task 5)
runtime/src/obj.c        variant objects, boxed scalar optionals (Tasks 4, 6)
tests/corpus/lang/       one fixture directory per adoption
docs/plan/oop-vm/01-error-catalog.md   grows per task
docs/plan/oop-vm/00-wob-format.md      grows with the three VM changes
```

---

### Task 1: Modules — `use` + directory-as-module

**Concept & reason:** foundation first: later tasks and the whole stdlib import through it. A `.wo` file's module is its directory; `use fs` (stdlib namespace) or `use shared/util` (project-relative) brings a module's public names into scope. Symbol resolution goes file → module → used modules → stdlib; collisions diagnose rather than shadow silently. Public means `pub`-marked (this task adds the `pub` marker for declarations; field accessors come in Task 7). Unused `use` warns. The stdlib namespaces resolve even though their members arrive in plan 9 — the resolver knows reserved namespace names now so plan 9 slots in without resolver changes.

- [ ] Failing fixtures: cross-module call via `use`; collision diagnostic; private-name-access diagnostic; unused-use warning golden.
- [ ] Implement resolver + `pub`; corpus green; catalog entries.
- [ ] Record commit draft: `feat(compiler): module system — directory-as-module, use resolution with collision/privacy diagnostics, pub marker, reserved stdlib namespaces.`

### Task 2: Small control surface — `break`/`continue`, `do…while`, interpolation, `const`, `and`/`or`

**Concept & reason:** the low-risk parity gaps, batched because each is a lexer/parser/emit touch with no typing subtlety. Loop control lowers to jumps with correct drop-set handling at early exits (the owner pass already computes scope-end drops for `return`; `break`/`continue` reuse that machinery — the one non-trivial bit, and its fixture proves an owned value dropped on `break`). String interpolation desugars to concatenation at parse time. `const NAME = literal` declares compile-time values usable in expressions and `#if`-adjacent contexts; `inline`-function requests are rejected with the table's reason. `and`/`or` land here too: two new keywords (spelled as words, not `&&`/`||`), one new precedence level below comparison and above assignment (`or` binds loosest, then `and`, then comparison, then the arithmetic ladder), short-circuit evaluation, `Bool`-typed operands only — a non-`Bool` operand is a type error, no truthiness — lowering to compare-and-jump on existing opcodes (`JZ` plus a jump), no new opcode.

- [ ] Failing fixtures: loop-control goldens incl. the owned-drop-on-break ASan case; do-while; interpolation with expressions; const usage; `inline fn` must-fail; `and`/`or` precedence goldens (incl. `a == 1 and b == 2` parsing without parens), short-circuit behavior, non-`Bool` operand must-fail.
- [ ] Implement; green.
- [ ] Record commit draft: `feat(compiler): break/continue/do-while with drop-correct early exits, string interpolation desugar, const values, inline-fn rejection, and/or boolean operators (new precedence level, short-circuit, Bool-only, compare-and-jump lowering).`

### Task 3: `switch` as expression

**Concept & reason:** Haxe's strongest habit and log-watcher's core idiom. `switch subject { case pattern: expr … default: expr }` is an expression; arms yield values of one unified type. Subjects: scalars, texts, union values. Exhaustiveness: switching a union without `default` requires every variant covered (diagnostic names the missing ones); scalars/texts require `default`. Lowering: compare-and-jump chains on existing opcodes (EQ/EQS/JZ); each arm is its own drop scope. Statement-position switch is the expression with a discarded value — one construct, not two.

- [ ] Failing fixtures: value-yielding switch goldens over ints/texts/unions; missing-variant must-fail; missing-default-on-scalar must-fail; arm-type-mismatch must-fail.
- [ ] Implement; green.
- [ ] Record commit draft: `feat(compiler): switch expressions — exhaustive over unions (missing variants named), unified arm typing, EQ/JZ chain lowering with per-arm drop scopes.`

### Task 4: `typedef` records + enum payload variants

**Concept & reason:** the data-shape pair, together because both lower to the same VM notion (a class-table entry the source never declared as `class`). Records: `typedef Name = { field: Type, ?opt: Type }` — structural aliases; two typedefs with the same shape are the same type; record literals use the existing constructor-brace form; `?fields` type as optionals (Task 6 semantics — this task lands them as nullable-by-shape, Task 6 tightens handling). Enum payloads: union variants gain fields (`Pending | Failed(reason: Text)`); construction by variant name with arguments; a payload variant is a small heap object whose layout is a compiler-generated class entry with a variant tag the VM's existing header accommodates (format-doc update: the variant-tag convention). Switch (Task 3) binds payload fields in arms.

- [ ] Failing fixtures: record round-trips incl. optional-field omission; structural-equivalence golden (same shape interchangeable); payload construction + switch destructuring; wrong-payload-arity must-fail.
- [ ] Implement compiler + the variant-object VM piece; green; format doc updated.
- [ ] Record commit draft: `feat: typedef records (structural, ?fields) + enum payload variants (tagged variant objects, switch destructuring); format doc variant convention.`

### Task 5: `try` / `catch` over the trap system

**Concept & reason:** the biggest VM change of the plan: **catch frames**. A `try` region registers a handler; a trap raised inside unwinds frames — running drop maps exactly as today — but stops at the nearest handler instead of the entry boundary, delivering the structured error record `{code, method, line, msg}` bound to the catch variable. Uncaught behavior is byte-for-byte today's trap surface. `throw` (explicit raise) is cut from this task — 0 uses in the driving workload, parked post-iteration-12 — so the error record carries no optional payload slot and there is no `.wob` version-note coordination to make. Compiler side: `try expr catch (e) expr` expression form, both arms unified in type; the owner pass treats the catch arm as an alternate flow join.

- [ ] Failing fixtures: caught trap yields fallback (div0 probe pattern); drop maps still fire for frames skipped by the unwind (ASan big-class proof — the load-bearing test); uncaught still exits 1 with the plan-6 shaped error; nested try picks the nearest handler.
- [ ] Implement VM catch frames + compiler lowering; all prior trap fixtures re-run unchanged; green.
- [ ] Record commit draft: `feat: try/catch — VM catch frames (unwind stops at nearest handler, drop maps intact), expression-form catch with flow-join ownership; uncaught surface unchanged. throw cut (0 uses) — no error payload slot.`

### Task 6: `?T` optionals with forced handling

**Concept & reason:** the null-safety story. `?T` admits nil; `T` never does — the diagnostic-enforced boundary. Representation: heap kinds use the zero word (the VM's existing null checks already trap on it — optionals make those unreachable by typing); scalar optionals box into a one-field cell (the VM piece — obj.c gains the box; format doc notes the convention). Narrowing: comparing against `null` narrows in the branch (`if x != null` makes `x` a `T` inside — Haxe's exact idiom); using a `?T` un-narrowed where `T` is required diagnoses. Stdlib returns (plan 9) and record `?fields` (Task 4) type as `?T` from here on.

**First task of this plan:** iteration 4 (plan 3 — emitter, corpus, `woc build`) precedes plan 8; within plan 8 this task goes first — `?T` is plumbed (lexer/token/AST/parser/dump) but unenforced (E211/E212/E213 dead, probe exits 0 with zero diagnostics per `docs/plan/compiler/nullable-types-implementation.md`), and it blocks the log-watcher port (story iterations 5–6), which uses optionals throughout in place of the Haxe original's sentinel values.

- [ ] Failing fixtures: narrowing goldens; un-narrowed-use must-fail; nil propagation through record optional fields; boxed scalar optional round-trip; assignment of null to plain `T` must-fail.
- [ ] Implement; green.
- [ ] Record commit draft: `feat: ?T optionals — null-narrowing control flow, forced handling diagnostics, zero-word heap nil + boxed scalar cells; record ?fields and future stdlib returns typed ?T.`

### Task 7: `static` members, `using` extensions, `pub(read)` accessors

**Concept & reason:** the organization trio. Statics: `static fn`/`static const` on classes — namespaced calls (`Flock.held(path)`) with no instance, no `self`; lower as free fns with mangled names. Using: `using shared/textutil` makes that module's free fns whose first parameter matches a type callable as methods on it (`s.words()` for `words(s: Text)`) — resolution is compile-time only, no dispatch table, collisions with real methods diagnose (real method wins is a lie surface; error instead). Accessors: `pub(read) field` exports read access, writes stay owner-class-only — the Haxe `(default, null)` pattern; enforcement in the typechecker at field-write sites.

- [ ] Failing fixtures: static call goldens + self-in-static must-fail; using-extension call golden + collision must-fail; pub(read) external-write must-fail + internal-write golden.
- [ ] Implement; green.
- [ ] Record commit draft: `feat(compiler): static members (mangled free-fn lowering), using static-extensions (compile-time, collision-diagnosed), pub(read) accessor enforcement.`

### Task 8: `#if` build flags + reject-row enforcement + closeout

**Concept & reason:** last adoptions and the table's other half. Build flags: `woc -D name` defines flags; `#if name / #else / #end` sections include/exclude at the token stream level (flag names only, no expression language — the spec's limit); undefined flags are false; nesting allowed. Reject enforcement: the keywords that would otherwise parse get targeted diagnostics with the table's reasons — `extends`/`implements`/`super`/`override` on class declarations, `cast`, `Dynamic`/`untyped` as type/expression, `macro`, `extern`, `operator` — each cites the spec section. `abstract` joins this reject row too, but needs no diagnostic of its own: the keyword never lexes, so it is absent by construction, the same as `macro`/`extern`. Closeout: error catalog complete for the track, keyword table in the spec annotated with shipped status, `just oop-accept` runs the grown corpus, CLAUDE.md language notes synced.

- [ ] Failing fixtures: #if inclusion/exclusion goldens (portable-style flag), nesting, undefined-flag default; one must-fail per reject keyword with the doctrine message.
- [ ] Implement; full corpus green; docs synced.
- [ ] Record commit draft: `feat(compiler): #if build flags (token-level, flag names only) + reject-row diagnostics citing doctrine (extends/cast/Dynamic/macro/extern/operator...); systems-track language surface complete, catalog + docs synced.`

---

## Plan self-review notes

- **Spec coverage (Part 1 + success criterion 1):** every adopt row has a task (modules T1, control/const/interp/and-or T2, switch T3, typedef+enum T4, try/catch T5, optionals T6, static/using/pub(read) T7, #if T8); every reject row enforced in T8 or absent by construction — `abstract` moved from adopt to reject on 2026-08-10, and `is` was cut the same day (0 uses in the driving workload, parked post-iteration-12), so this plan's old Task 7 is deleted rather than deferred. Criterion 1's "corpus coverage per row" is each task's fixture requirement.
- **VM changes fenced:** exactly three (catch frames, variant objects, boxed scalar optionals), each with a format-doc update in its task; everything else is lowering.
- **Order rationale:** modules first (everything imports), data shapes before optionals (records carry ?fields), try/catch after switch (arms reuse unified-type machinery), rejects last when all parse paths exist to hang diagnostics on.
