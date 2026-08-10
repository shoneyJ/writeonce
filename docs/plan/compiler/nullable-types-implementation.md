# Nullable Types (`?T`) + Constrained `@gc` Implementation Plan

> **Rewritten 2026-08-10** as an honest status record plus a handoff, after an
> audit found this doc's own status table claiming `?T` was "Implemented
> with nullable handling" when the enforcement semantics do not exist. See
> `.dev/commit.md` (subject: `refactor(compiler): drop Money/SKU/Float and
> the abstract-type allowlist; correct the nullable-types plan doc`) for the
> change that produced this rewrite.

## Status

`?T` is **plumbed but not enforced**. Every stage that carries the syntax
through the pipeline shipped; the one stage that would give it meaning —
forced handling in the typechecker — did not.

| Component | Status |
|-----------|--------|
| **Lexer** (`lexer.ml`) | Shipped — has `Question` token (`?`) |
| **Token** (`token.ml`) | Shipped — has `Question` kind |
| **AST** (`ast.ml`) | Shipped — `field_ty` has a `Nullable` variant |
| **Parser** (`parser.ml`) | Shipped — parses the `?` prefix in all three positions: field types, return types, parameters |
| **Dump** (`dump.ml`) | Shipped — renders `?T` as `?` + inner type |
| **Typechecker semantics** (`types.ml`) | **NOT shipped.** `?T` round-trips through every stage above as inert syntax. The typechecker never enforces the `?T`/`T` boundary: no null-narrowing, no forced-handling diagnostic, no distinction in practice between a `?T` field and a `T` field. |

**Evidence (re-run 2026-08-10):**

```wo
class Box { v: ?Int }
fn take_it(b: Box) -> Int { return b.v; }
```

`woc` on this file exits **0** with **zero diagnostics**. `take_it` returns
`b.v` — a `?Int` — from a function declared to return `Int`, with no null
check anywhere. This is the entire point of `?T` (forced handling: you may
not use a possibly-nil value where a never-nil value is required), and it is
completely unenforced.

## Handoff

`?T` forced handling — null-narrowing control flow, the `WO-E211`/`WO-E212`/
`WO-E213` diagnostics below, and boxed scalar cells for nullable scalars — is
owned by `docs/plan/compiler/2026-08-01-haxe-parity-language.md` **Task 6**,
not this doc. That task is the **next work item** on the compiler track: it
blocks the log-watcher port (story iterations 5–6), which uses `?T`
throughout in place of the Haxe original's sentinel values. This doc stops
claiming ownership of that work.

## Dead-code register

Ten `WO-E2xx` codes are declared as named constants in `types.ml` with no
call site anywhere in the front end — `grep '~code:'` finds exactly five
sites (`WO-W201`, `WO-E202`, `WO-E206`, `WO-E207`, `WO-E225`); the other ten
declared constants are never referenced by a `Diag.error`/`Diag.warning`
call. `docs/plan/oop-vm/01-error-catalog.md`'s "Reserved, not yet emitted"
section already lists all ten; this table adds *why* each is dead and who,
if anyone, is expected to wire it:

| Code | Meaning | Why it's dead |
|------|---------|----------------|
| `WO-E201` `type_mismatch` | operand/assignment type mismatch | Named in `docs/plan/compiler/2026-08-01-woc-compiler-front.md` Task 6's own must-fail list ("type mismatch, unknown field, bad arity, unsatisfied interface, incomplete constructor") — a gap in already-shipped work, not blocked on any future feature. `Binary`/`Unary` in `types.ml` don't check operand types at all. |
| `WO-E203` `bad_arity` | wrong argument count at a call | Same Task 6 brief, same gap: `Call (_callee, args)` in `types.ml` type-checks each argument expression but never compares the count (or types) against the callee's signature. |
| `WO-E204` `unknown_fn` | call names a free function that doesn't resolve | `Call`'s callee is never looked up in `syms.free_fns` at all — not blocked on modules; even a same-file unresolved call goes unchecked today. Gap in shipped work. |
| `WO-E205` `unsatisfied_interface` | a class doesn't structurally satisfy an interface | **Called out explicitly**: this is the most consequential of the ten. `types.ml`'s own header comment (line 5) claims the pass "Produces typed AST + per-class field-kind table + interface satisfaction set," and the Task 6 brief names this as a required check ("calling an interface method on a non-satisfying class is an error naming the missing/mismatched signature") — but no code path ever calls `Diag.error ~code:unsatisfied_interface_code`. Structural interface satisfaction is entirely unenforced. Gap in shipped work, not deferred to a future plan. |
| `WO-E208` `non_exhaustive_switch` | a `switch` expression doesn't cover every case | Genuinely blocked: no `switch` keyword exists in `token.ml`/`lexer.ml`/`parser.ml` yet. Owned by haxe-parity Task 3 (`switch` as expression). |
| `WO-E209` `invalid_builtin` | a builtin call (`now`, `latest`, `count`, `words`, `print`, …) used with the wrong signature | Task 6's brief promises builtin-signature checking "matching the VM's builtin table," but `Call` has zero builtin special-casing — every callee is treated identically. Gap in shipped work. |
| `WO-E210` `module_not_imported` | a name used from a module that was never `use`d | Genuinely blocked: no `use`/module concept exists anywhere in the parser yet. Owned by haxe-parity Task 1 (Modules). |
| `WO-E211` `nullable_used_without_check` | a `?T` value used where `T` is required, unnarrowed | Genuinely blocked on the `?T`/`T` enforcement this doc hands off above. Owned by haxe-parity Task 6. |
| `WO-E212` `nullable_assign_mismatch` | assigning across the `?T`/`T` boundary without narrowing | Same as `WO-E211`. Owned by haxe-parity Task 6. |
| `WO-E213` `missing_nil_check` | narrowing control flow itself | Same as `WO-E211`/`WO-E212`. Owned by haxe-parity Task 6. |

Five of the ten (`E201`, `E203`, `E204`, `E205`, `E209`) need **no new
language feature** — they are checks the shipped Task 6 typechecker's own
brief already promised and never wired. The other five (`E208`, `E210`,
`E211`, `E212`, `E213`) are genuinely blocked on features that don't exist
yet, each owned by a specific haxe-parity task as noted above.

## Narrowing notes

- **`WO-W201` shipped a self-reference-only heuristic.** The "Heuristic for
  `has_recursive_structure`" section below lists four bullets; the actual
  `has_recursive_structure` in `types.ml` implements only the first two
  (a field of the class's own type, directly or through `ref`/`multi`/
  `map<_, Self>`). The other two — a cycle *through other classes*, and "used
  as `@gc` ref elsewhere in the same module" — are not implemented. The
  function only ever compares a field's referenced type name against the
  class's own name; it has no transitive/cross-class analysis.
- **`WO-E225` covers bare class fields only.** `check_field_types` walks
  `Ast.Class` fields exclusively (this does include `type X {}` declarations,
  which parse to `Ast.Class` too — but never method parameters, return
  types, or the element types of `multi T`/`map<K,V>`, because
  `scalar_name_of` returns `None` for `Ref | Multi | Map`). A method
  returning `Money`, a parameter typed `SKU`, or a field typed
  `map<SKU, Money>` never triggered `WO-E225` even before this change removed
  those names — and the same asymmetry holds for any future unknown type
  name today.
- **The "not typedef" clause is meaningless.** `is_known_type_name` has no
  "or a declared typedef" disjunct, and adding one would be a no-op:
  `typedefs` is declared in the `symbols` record, initialized to an empty
  map, and threaded from pass 1 into pass 2 — but nothing ever populates it.
  `type X { ... }` parses to `Ast.Class`, the same declaration form `class`
  uses, so typedef-shaped declarations are already resolved through the
  classes clause instead. There is no code path that could ever populate
  `typedefs`, so no fix is missing here — the field itself is dead weight.

## Additions this doc missed

- **`WO-E214`** (cross-file collision) exists and is emitted — from the
  *driver* (`compiler/bin/main.ml`), not `types.ml`, when the same class or
  interface name is declared in two files a directory discovers. It reuses
  the `WO-E2xx` range because it's a symbol-table concern, not a lexing,
  parsing, or ownership one. Shipped as part of driver polish
  (`docs/plan/compiler/2026-08-01-woc-compiler-front.md` Task 8); this doc
  never mentioned it.
- **A Task 7 ownership pass exists.** `compiler/src/owner.ml` (MVS flow
  analysis, `WO-E3xx` two-site diagnostics, the moves/drops/rc/residual
  tables behind `--dump-owner`) is fully implemented. This doc predates it
  and never referenced it.

---

## New Requirements

### 1. Diagnostic-Assisted `@gc` Inference (WO-W201)

**Goal:** Keep explicit `@gc` but add compiler diagnostic when borrow checker cannot prove safety.

**New Diagnostic:**
```
WO-W201: <ClassName> has recursive/shared structure that borrow checker cannot prove.
         Consider adding @gc if this is an ephemeral in-memory cache.
         If this maps to a database table, keep owned (default).
```

**When to emit:**
- Class has fields that form recursive structures (e.g., `map<K, V>`, `multi T` where T is the same class)
- Class has fields that are borrowed in ways the borrow checker cannot prove safe
- Class is used in patterns typical of caches/registries (stored in `map`, passed as `@gc` ref)

**When NOT to emit:**
- Class has `@table` annotation → must be owned (DB-backed)
- Class has `@unique` field → persistent identity
- Class is a simple data struct (no recursive/shared patterns)

Shipped, with the narrower heuristic recorded above under "Narrowing notes."

---

### 2. Scalar Type Corrections

**Current (Wrong):**
```ocaml
let builtin_scalars = ["Int"; "Bool"; "Text"; "Money"; "Timestamp"; "Id"; "SKU"]
```

> **Historical record — do not edit to match current reality.** This block
> documents the bug Task 6b found (`Money`/`SKU` as bare builtins, no
> `Float`), not the code as it exists today. Its correction below has since
> been corrected again — see the note that follows.

**Corrected (as of this change, 2026-08-10):**
```ocaml
let builtin_scalars = ["Int"; "Bool"; "Text"; "Timestamp"; "Id"]
```

**Changes, in order:**
1. Task 6b: removed `Money`, `SKU` (they had no `abstract` declaration to
   back them — magic strings) and added `Float` (documented as "IEEE 754
   double / f64").
2. This change (2026-08-10): removed `Float` too. It had the identical
   phantom-scalar defect from the opposite direction — `token.ml` has no
   float-literal kind and `wob.h` has no float representation, so `ratio:
   Float` typechecked while no `Float` value could ever be written or
   represented. `Money`/`SKU` also stay removed; the stopgap allowlist that
   let them resolve (`abstract_types`/`is_abstract_type`) is deleted
   outright, not emptied. `builtin_scalars` is now exactly the five names
   that work end to end: `Int`, `Bool`, `Text`, `Timestamp`, `Id`.

**The `abstract` feature itself is rejected**, not deferred. The
systems-track verdict table's `abstract` row flips **adopt → reject**
(`docs/superpowers/specs/2026-08-01-systems-track-design.md`): a distinct
scalar type adds a conversion surface without buying safety this language
needs, and the compiler's own `Money`/`SKU` stopgap allowlist is the
concrete proof the cost was real. Domain scalars are plain `Int`/`Text`.
Haxe-parity's abstract+`is` task was deleted outright (`is` cut, 0 uses). No allowlist has a future to be revived
into — re-adding `Float` requires float literals in the lexer *and* a float
kind in `.wob` landing together; re-adding `abstract` requires the keyword
itself to lex and parse, which plan 8 Task 8's reject-row enforcement now
actively blocks.

---

### Updated Built-in Scalar List (in `types.ml`)

```ocaml
let builtin_scalars = ["Int"; "Bool"; "Text"; "Timestamp"; "Id"]
```

---

### Updated Built-in Scalar Table

| Type | Description | Runtime Representation |
|------|-------------|------------------------|
| `Int` | 64-bit signed integer | i64 |
| `Bool` | Boolean | i64 (0/1) |
| `Text` | UTF-8 string | pointer + length |
| `Timestamp` | Milliseconds since epoch | i64 |
| `Id` | Opaque identifier | i64 |

(`Float`'s row is deleted — see the Scalar Type Corrections section above
for why.)

---

## Updated Plan

### New Tasks Added

#### Task A: Diagnostic WO-W201 for `@gc` Suggestion

**File:** `compiler/src/types.ml` (Pass 2 - typechecker)

**Implementation:**
```ocaml
(* In typechecker, after analyzing class structure *)
let suggest_gc_annotation (cls : class_info) : unit =
  if cls.is_gc then ()  (* Already @gc *)
  else if cls.table.is_some then ()  (* Has @table -> must be owned *)
  else if has_recursive_structure cls then
    Diag.Collector.add collector
      (Diag.warning ~code:"WO-W201" ~file:cls.pos.file ~line:cls.pos.line ~col:cls.pos.col
         ~message:(Printf.sprintf "%s has recursive/shared structure that borrow checker cannot prove. Consider adding @gc if this is an ephemeral in-memory cache. If this maps to a database table, keep owned (default)." cls.name) ())
```

**Heuristic for `has_recursive_structure`** (as originally scoped — see
"Narrowing notes" above for which two of these four actually shipped):
- Class has a field of its own type (direct recursion)
- Class has `multi Self` or `map<_, Self>` field
- Class fields form a cycle through other classes
- Class is used as `@gc` ref elsewhere in the same module

---

#### Task B: Fix Built-in Scalar List

**File:** `compiler/src/types.ml`

**Change:**
```ocaml
(* Before *)
let builtin_scalars = ["Int"; "Bool"; "Text"; "Money"; "Timestamp"; "Id"; "SKU"]

(* After *)
let builtin_scalars = ["Int"; "Bool"; "Text"; "Float"; "Timestamp"; "Id"]
```

> **Historical record — do not edit to match current reality.** This is
> Task 6b's own correction at the time it was made; `Float` has since been
> removed too (this change, 2026-08-10), for the reason given in the
> Scalar Type Corrections section above. The true current list is
> `["Int"; "Bool"; "Text"; "Timestamp"; "Id"]`.

`abstract` types are rejected outright (see above) — there is no allowlist
to add validation for, stopgap or otherwise. An unknown scalar name (not a
builtin, not a declared class, not a declared interface) is simply
`WO-E225`, unconditionally.

---

### Updated Typechecker Tasks

#### Task 2a (Updated): Typechecker with New Diagnostics

**File:** `compiler/src/types.ml`

**New WO-E Codes:**
| Code | Trigger |
|------|---------|
| WO-E225 | Unknown type: not a builtin, not a declared class, not a declared interface |
| WO-W201 | Class has recursive/shared structure, consider `@gc` |

**Implementation Order (as shipped):**
1. `builtin_scalars` correction (now the five that work — see above)
2. WO-W201 diagnostic in class analysis pass
3. WO-E225 for unknown scalar names

(The stopgap `is_abstract_type` check that used to sit between steps 1 and 2
is gone — see the Scalar Type Corrections section.)

---

### What Task 6b actually tested

No `test/golden/types/` directory exists, and `woc` has no dump flag for the
types stage at all — its dump flags are `--dump-tokens`, `--dump-ast`, and
`--dump-owner`, nothing more. None of `gc-suggestion.wo`, `sku-scalar.wo`,
`unknown-type.wo`, or `float-example.wo` (all named in an earlier version
of this doc) was ever created as a fixture file. Task 6b instead asserted
these behaviors directly in `compiler/test/runner.ml`, via `typecheck_str`
over inline `.wo` source strings:

- **WO-W201 (gc-suggestion):** checks named `"gc-suggestion: exactly one
  diagnostic (WO-W201)"`, `"gc-suggestion: code is WO-W201"`, etc., run over
  inline sources for a self-referential `Node`, an `@gc`-annotated `Cache`
  (must NOT fire), an `@table`-annotated `Node2` (must NOT fire), a
  `@unique`-fielded `Node3` (must NOT fire), a plain `Point` struct (must NOT
  fire), unrelated `multi`/`map` fields on `Calc`/`Bucket` (must NOT fire —
  the over-trigger risk), and a `multi Self`-fielded `Tree` (must fire).
- **Scalar list:** `"Money is no longer a builtin scalar"`, `"SKU is no
  longer a builtin scalar"`, `"Timestamp is a builtin scalar"`, and (as of
  this change) `"Float is not a builtin scalar"`.
- **WO-E225 (unknown type):** `"unknown-type: exactly one diagnostic
  (WO-E225)"` over `class BadExample { code: INVALID_TYPE }`, and (as of
  this change) `"unknown-type fields (SKU, Money): exactly two
  diagnostics"` over `class Product { id: Id; sku: SKU; price: Money }`.

### Updated Files

What actually changed, across both Task 6b and this change:

| File | Changes |
|------|---------|
| `compiler/src/types.ml` | `builtin_scalars` corrections (Task 6b: −Money/SKU +Float; this change: −Float too); WO-W201 diagnostic; WO-E225 unknown-type diagnostic; the `abstract_types`/`is_abstract_type` stopgap allowlist added by Task 6b, then deleted outright by this change |
| `compiler/src/owner.ml` | `oclass_of`'s builtin-scalar branch (this change: dropped the `is_abstract_type` disjunct — behavior-neutral, the `else Copy` fallthrough already caught unknown names) |
| `compiler/src/diag.ml` | `WO-W201` warning prefix; `WO-E225` (and the other reserved `WO-E2xx` codes) |
| `compiler/test/runner.ml` | Direct assertions for WO-W201 and WO-E225 (see "What Task 6b actually tested" above); no fixture files |
| `compiler/test/golden/ast/pricing-demo.{wo,expected}`, `compiler/test/golden/owner/pricing-demo.wo`, `compiler/test/golden/ast/two-error-recovery.wo` | This change: `Money` → `Int`, `SKU` → `Text` (pure rename; owner's golden `.expected` is byte-identical) |
| `docs/plan/compiler/nullable-types-implementation.md` | This document |

---

### Verifying today

There is no dump flag for the types stage to demonstrate any of this with.
What actually exists:

```bash
just woc-test                       # dune runtest: test_diag + runner goldens/assertions
compiler/_build/default/bin/woc <file.wo>   # real compiler, real exit code + diagnostics
```

To see the `?T` gap directly, run the probe under "Status" above through
`woc` — exit 0, no diagnostics, despite returning a nullable value from a
non-nullable-typed function.

---

### Updated `.wob` Format (Plan 3)

No changes needed - abstract types compile to their underlying representation at runtime.

---

## Abstract Data Types — the container roster (recorded 2026-08-08; FUTURE — post story iteration 11)

> **Scope note (2026-08-08):** the story's critical path is *compile and
> run log-watcher* (iterations 3–7). log-watcher needs only `multi`, `map`,
> and `Text`. Nothing in this roster is scheduled before story iteration 11
> completes; it is the recorded candidate pool, not work.

Two different "abstract" notions live in this plan; keep them apart:

- **Abstract newtypes** (`abstract Money = Int`) — zero-cost compile-time
  wrappers over a scalar representation. Covered above; verdict-table adopt
  row.
- **Abstract data types (ADTs)** — behavioral specifications of containers:
  an ADT says *what operations exist and their semantics*; a data structure
  says *how they are implemented*. A map is an ADT; a hash table and a
  red-black tree are two data structures implementing it.

Doctrine holds: containers are **runtime-provided native classes
implemented in C**, not user-definable generics (OOP spec §3). The VM picks
the backing data structure; the language exposes only the ADT's operations.
Milestone 1 ships `multi` (list) and `map`. The globally accepted ADT
roster below is the candidate pool for later milestones — names and
semantics only, implementation deliberately unspecified:

**Linear**

| ADT | Semantics |
|-----|-----------|
| List | ordered sequence, indexable, duplicates allowed — **shipped as `multi`** |
| Stack | LIFO: push, pop, peek |
| Queue | FIFO: enqueue, dequeue |
| Deque | insert/remove at both ends |
| Priority queue | retrieve highest-priority element first |

**Associative**

| ADT | Semantics |
|-----|-----------|
| Set | unordered collection of unique elements |
| Multiset (bag) | like a set, but counts duplicates |
| Map (dictionary) | key → value lookups — **shipped as `map`** |
| Multimap | one key maps to multiple values |

**Hierarchical / connected**

| ADT | Semantics |
|-----|-----------|
| Tree | nodes with parent-child relations |
| Binary search tree | ordered tree: search, insert, delete |
| Heap | partial ordering; the usual backing for a priority queue |
| Graph | vertices plus edges, directed or undirected |
| Trie | prefix tree for strings |

**Other**

| ADT | Semantics |
|-----|-----------|
| String | sequence of characters — **shipped as `Text`** |
| Matrix / array | fixed-dimension indexed storage |
| Union-find (disjoint set) | track partitions, merge groups |
| Stream / iterator | sequential access to a lazily produced sequence |

Selection rules when a later milestone adopts one:

1. Only globally accepted ADTs from this roster — no bespoke container
   inventions.
2. Adoption is demand-driven: a sample workload must need it first
   ("samples force the grammar", principles doc #8).
3. Each adopted ADT lands as a native class with the same machinery `multi`
   and `map` already use: header sentinel class id, kind-tagged elements,
   builtin-table operations, drop/GC integration via `wo_drop_kind`.
4. The ADT's operation set is normative in the format doc; the backing
   structure stays a VM implementation detail and may change without a
   language-surface change.

---

## Summary of Changes to Existing Plan

| Section | Change |
|---------|--------|
| `builtin_scalars` | Task 6b: remove `"Money"`, `"SKU"`; add `"Float"`. This change (2026-08-10): remove `"Float"` too. Final list: `["Int"; "Bool"; "Text"; "Timestamp"; "Id"]`. |
| Typechecker | Shipped: WO-W201 (`@gc` suggestion, self-reference-only heuristic) + WO-E225 (unknown type, bare class fields only). Still dead: ten reserved `WO-E2xx` codes — see "Dead-code register" above. |
| `abstract` types | **Rejected**, not adopted. Verdict-table row flips adopt → reject; haxe-parity's abstract+`is` task was deleted outright (`is` cut, 0 uses). No `Money`/`SKU`/any newtype re-declaration is coming. |
| `?T` semantics | **Not implemented.** Plumbed through lexer/token/AST/parser/dump; typechecker enforcement (narrowing, forced handling, `WO-E211`–`WO-E213`) owned by haxe-parity Task 6 — the next work item. |
| ADT roster | Globally accepted container ADTs recorded as the candidate pool for future native classes (section above); `multi`/`map`/`Text` mapped to List/Map/String |
| Test fixtures | None added under `test/golden/types/` — Task 6b asserted behavior directly in `runner.ml` instead (see "What Task 6b actually tested") |
| Documentation | This document, rewritten 2026-08-10 |

---

## References

- [Error catalog](../oop-vm/01-error-catalog.md) — every `WO-E`/`WO-W` code `woc` actually emits, plus the "Reserved, not yet emitted" section this doc's dead-code register expands on
- [Haxe-Parity Language plan](2026-08-01-haxe-parity-language.md) — Task 6 owns `?T` forced handling (the handoff above); the abstract+`is` task is deleted (`is` cut, 0 uses)
- [OOP Compiler VM Design](../../superpowers/specs/2026-08-01-oop-compiler-vm-design.md) - Section 3
- [Systems Track Design](../../superpowers/specs/2026-08-01-systems-track-design.md) - Part 1; the `abstract` row (adopt → reject)
