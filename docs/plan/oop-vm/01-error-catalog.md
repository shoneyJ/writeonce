# The `woc` diagnostic catalog — normative reference

Every `WO-E###`/`WO-W###` code the `woc` front end (`compiler/`) actually
emits, as of plan 2 tasks 2–8 and plan 3 tasks 1–2. Code ranges are reserved
per stage (`compiler/src/diag.ml`): `WO-E0xx` lexing, `WO-E1xx` parsing,
`WO-E2xx` types, `WO-E3xx` ownership, `WO-E4xx` the bytecode emitter,
`WO-W2xx` warnings from the types stage. This
is an enumeration of codes already in use, not an archaeology dig — see
"Completeness method" below for how that was verified, "Reserved,
not yet emitted" for codes the source declares but no check yet raises,
and "Reachable but unenforced" for the one code (WO-E205) whose check
site the milestone grammar *does* exercise, unlike the codes above it —
see that section for why this is a live gap, not a scope boundary.
One code (WO-E214) is emitted by the driver (`compiler/bin/main.ml`),
not one of the four stage modules — a Task 8 review finding — see its
row in the types table below for why it still uses that range.

Every diagnostic renders as `file:line:col: <severity> <code>: <message>`,
the source line, and a caret under the column (`compiler/src/diag.ml`);
ownership errors (`WO-E3xx`) add a second, indented site for the other
half of the story ("moved here" / "borrowed here" / etc.).

## WO-E0xx — lexing (Task 3, `compiler/src/lexer.ml`)

| code | meaning | example message |
| --- | --- | --- |
| WO-E001 | an input byte the lexer doesn't recognize as the start of any token. Reported once per bad byte, which is then skipped — one bad byte never stops the whole file. | `unknown character '$'` |
| WO-E002 | a string literal's backslash escape is the last byte of the file, with no character left to escape (a plain unterminated string with no dangling backslash is *not* an error — rt parity). | `unterminated string escape` |

## WO-E1xx — parsing (Tasks 4–5, `compiler/src/parser.ml`)

| code | meaning | example message |
| --- | --- | --- |
| WO-E101 | generic syntax error: an unexpected token where the grammar expected something else, including running off the end of the file inside an unclosed block/type/interface body. Declaration-level recovery syncs to the next top-level keyword so one bad declaration yields one diagnostic, not a cascade. | `expected ')' or ',', got NEWLINE` |
| WO-E102 | an invalid `@table(...)` configuration: `name` given twice, an `index` with no columns, or an argument key other than `name`/`index`. | `@table(name: ...) given twice` |

## WO-E2xx / WO-W2xx — types (Task 6, `compiler/src/types.ml`; WO-E214 Task 8, `compiler/bin/main.ml`; WO-E215 plan 3 Task 2, `compiler/src/types.ml`)

| code | meaning | example message |
| --- | --- | --- |
| WO-W201 *(warning)* | a class has recursive/shared structure (a field, directly or through `ref`/`multi`/`map`/`?`, refers back to its own class) that the ownership pass cannot prove disjoint, has no `@table`, and has no `@unique` field — suggests `@gc`. | `Node has recursive/shared structure that borrow checker cannot prove. Consider adding @gc if this is an ephemeral in-memory cache. If this maps to a database table, keep owned (default).` |
| WO-E202 | a `.field` access names a field that the base's class (a *declared* class — an unresolved/placeholder expression type never triggers this) doesn't have. | `unknown field \`price\` on \`Product\`` |
| WO-E206 | a constructor literal (`ClassName { ... }`) omits a field the class declares (no default). | `missing field \`sku\` in constructor of \`Product\`` |
| WO-E207 | a constructor literal names a class that isn't declared anywhere in the (possibly multi-file) program. | `unknown type \`Widget\` in constructor` |
| WO-E214 | a class or interface name is declared more than once across the files a directory discovers (one program, multiple files — Task 8). Reported at the *later*-discovered declaration (sorted by path), with the first declaration as the related site; the merged symbol table keeps the first one, so this is what stops that silent keep from also hiding a real shape conflict. Driver-level, not `types.ml` — reuses the `types_prefix` range because it's a symbol-table concern, not a lexing/parsing/ownership one. | `class \`Dup\` already declared in \`a_first.wo\`` |
| WO-E215 | a class, interface, or free `fn` name is declared more than once in the *same file* (`collect_declarations`'s own `StringMap.add` silently dropped the earlier one — Task 1 review, found while building the plan-3 emitter, fixed in Task 2). Reported at the later declaration, with the first as the related site — the same shape as WO-E214, one file instead of two; the symbol table keeps the first declaration. Class/interface names and free-fn names are separate namespaces, so a class and a fn sharing a name never collide here. | `class \`Dup\` already declared` |
| WO-E225 | a field's declared type name isn't a builtin scalar, a declared class, or a declared interface. Checked once per field declaration, at the field's own position. | `unknown type \`Wdiget\`` |

### Reserved, not yet emitted

`type_mismatch_code` (WO-E201), `bad_arity_code` (WO-E203),
`unknown_fn_code` (WO-E204), `non_exhaustive_switch_code` (WO-E208),
`invalid_builtin_code` (WO-E209), `module_not_imported_code` (WO-E210),
`nullable_used_without_check_code` (WO-E211), `nullable_assign_mismatch_code`
(WO-E212), and `missing_nil_check_code` (WO-E213) are declared in `types.ml`
— the range is reserved — but as of Task 7 nothing in the front end ever
raises them; there is no call site and therefore no real example
message to catalog. They read like placeholders for checks Task 6's own
plan brief named (type mismatch, bad arity, unsatisfied interface, …)
that the shipped typechecker doesn't yet implement. Listed here so a
conformance fixture (plan 3) or a future reader doesn't assume one of
these codes is reachable today; move a code up into the table above in
the same commit that wires its first real emission site.

### Reachable but unenforced

`unsatisfied_interface_code` (WO-E205) is declared in `types.ml` but does not
belong in the "Reserved, not yet emitted" list above either. An earlier
revision of this doc claimed WO-E205 was *unreachable by design* — that
was wrong, caught and corrected in the plan-3 Task 4 review (2026-08-11).
Structural interface satisfaction's one legal check site is where a value
is used at an interface-typed position (a field, parameter, or return
typed as an interface) — and the milestone grammar does exercise that
position today. This compiles with exit 0 and zero diagnostics:

```wo
interface Priced { fn current_price() -> Int }
class Rock { n: Int }
fn quote(p: Priced) -> Int { return p.current_price() }
fn main() { let r = Rock { n: 1 }
  print_int(quote(r)) }
```

`Rock` has no `current_price` method, so it does not structurally satisfy
`Priced` passed to `quote`'s interface-typed parameter — and `Rock`'s
method set is fully known at compile time, so this is a *statically
provable* violation, exactly the shape WO-E205 exists to catch. `woc
--emit` accepts it anyway. The unchecked call reaches `wovm` as an
`ICALL` with no matching vtable slot, which traps `WO_T_BOUNDS` (6, "no
vtable entry for receiver class") at runtime instead of failing to
compile. That inverts the hybrid boundary WO-E3xx pins elsewhere
(provable violation → compile-time diagnostic, unprovable → runtime
trap): here a provable violation resolves as a trap. This is an owed
gap, not a design decision, currently pinned as the known-gap fixture
`tests/corpus/trap/unsatisfied-interface/` (plan 3, Task 4) — its own
comment says it must move to `compile-fail/` with `fixture.code
WO-E205` in the same change that implements this check, rather than
silently going stale.

## WO-E3xx — ownership / MVS (Task 7, `compiler/src/owner.ml`)

Every ownership diagnostic carries a second site (the module doc's
"two-site errors are the product") — the example messages below are the
primary message only; the related site's label (e.g. "`b` moved here")
renders indented beneath it.

| code | meaning | example message |
| --- | --- | --- |
| WO-E301 | a place is read (or moved again) after its value was already moved — by assignment, `take` argument passing, constructor field init, or `return`. | `use of \`b\` after it was moved` |
| WO-E302 | a place is moved while a live borrow of it, or of an overlapping place, still exists. | `cannot move \`bag.items\` while \`r\` is borrowed` |
| WO-E303 | two exclusive (`mut`) accesses of the same place, or two accesses the analysis can *prove* overlap, conflict in one region (e.g. two `mut` element accesses through the same provable index, or the same place borrowed and then mutated). Cases the analysis can't prove either way become a residual site for the VM to guard at runtime, not this diagnostic. | `cannot borrow \`bag.items[i]\` as \`mut\` twice in the same call` |
| WO-E304 | a borrow is returned or stored somewhere that outlives the scope it borrowed from. `@gc`-typed values are exempt (freely aliased by design). | `borrow of \`x\` returned — borrows cannot outlive their scope` |

## WO-E4xx — emitter (plan 3 Task 1, `compiler/src/emit.ml`)

The emitter's range covers the two boundaries nothing upstream can see:
the `.wob` format's own encoding limits, and the milestone-1 instruction
set's edge — surface the front end accepts but the VM has no operation
for. Both are reported, never worked around: an over-budget method is a
diagnostic rather than a truncated frame, and a construct with no
lowering is a diagnostic rather than invented bytecode. No image is
written when any of these fire (`woc --emit` writes nothing on exit 1).

| code | meaning | example message |
| --- | --- | --- |
| WO-E401 | the method needs more than 64 registers — the VM's register window (`runtime/src/wob.h` `WO_MAX_REGS`, enforced by the loader). Reported once per method, at the method's own position. | `` `wide` needs more than 64 registers — the VM's register window is 64 slots; split the method or reduce the number of live locals `` |
| WO-E402 | a value that does not fit an instruction field: a constant/class/method/interface-slot index above 65535 (`LOADK`/`NEW`/`CALL`/`ICALL` carry a 16-bit operand), a field index above 255 (`GETF`/`SETF` carry a byte), or a jump farther than the signed 16-bit displacement. | `field index 300 exceeds the 8-bit GETF/SETF field` |
| WO-E403 | a construct the v1 instruction set cannot express, or a call the emitter cannot lower correctly. The full source-surface contract is [`08-builtin-surface.md`](08-builtin-surface.md); the cases raised here are: an unresolved name; a call to something that is neither a declared `fn` nor a builtin; a wrong argument count (nothing upstream checks arity — WO-E203 is declared and never raised — and a mismatched call reserves a window the callee does not read, which the loader rejects); a field/method on a type that is not a declared class; `multi_new()`/`map_new()` with no destination of declared type (the element kinds are the container's runtime drop plan and cannot be guessed); an element write into a `multi` (v1 has `multi_push`/`multi_get`, no element store); a `for` over a `map` (v1 exposes no key enumeration). Several of these are cases the typechecker's placeholder types let through — the emitter is the first stage that must be exact. | `` `for` can only iterate a `multi` — the v1 builtins expose no key enumeration for a `map` `` |
| WO-E404 | an ownership-table entry the emitter could not honor: a residual borrow site whose operand has no register at the guarded region, a residual region no lowering wrapped at all (checked at the end of every compilation unit — owner.ml anchors regions on several different node kinds, and one nobody consumed would ship the aliasing check silently disabled), or a drop/rc site naming a local that has no register. Emitting such a region unguarded would drop the single enforcement a residual site exists for, so it fails instead. | `` residual borrow site in `shuffle` names an operand with no live register — the runtime guard cannot be placed `` |
| WO-E405 | the program entry (the zero-arg free fn `main`, selected by name) declares a return type other than `Int`. The systems-track spec (`docs/superpowers/specs/2026-08-01-systems-track-design.md:70`) makes the entry's return value the process exit code, so any other declared return type was never legal — this is the check that finally says so. `main` with no return annotation at all is unaffected (nothing declared to contradict `Int`); every other milestone-1 fixture uses that form. Reported once, at `main`'s own position. | `` entry `main` declares return type `Node` — the entry's return value is the process exit code, so it must return `Int` `` |

## Completeness method

Every code in this catalog was found the same way: grep every
`Diag.error`/`Diag.warning` call site across `compiler/src/*.ml` (lexer,
parser, types, owner — dump.ml never constructs a diagnostic) and
`compiler/bin/main.ml` (the driver — added after a Task 8 review found
WO-E214 living there, outside the original src/*.ml-only sweep), then
cross-reference each `~code:` argument back to the `let <name>_code = ...`
constant it names. Every constant with at least one such call site is
in the table above; every constant with zero call sites is listed under
"Reserved, not yet emitted" instead of silently omitted. `parser.ml`'s
`fail`/`unexpected`, `owner.ml`'s `report`/`escape`/`check_against_borrows`,
`emit.ml`'s `err`/`over_budget`/`check_bx`/`check_field_idx`, `main.ml`'s
`report_collision`, and `types.ml`'s `report_duplicate_decl` (plan 3 Task 2)
are the only indirection layers
between a bare `~code:` argument and the `Diag.error` call — each was
read to confirm which named constant ultimately reaches the collector,
not just the arity-generic wrapper name. This is why the catalog is an
enumeration, not a dig: `diag.ml` already reserves the numeric ranges,
so the only open question per stage was *which* reserved codes actually
fire — answered by exhaustive grep, not inspection of a handful of
samples.
