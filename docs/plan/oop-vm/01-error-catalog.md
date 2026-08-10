# The `woc` diagnostic catalog — normative reference

Every `WO-E###`/`WO-W###` code the `woc` front end (`compiler/`) actually
emits, as of plan 2 tasks 2–8. Code ranges are reserved per stage
(`compiler/src/diag.ml`): `WO-E0xx` lexing, `WO-E1xx` parsing, `WO-E2xx`
types, `WO-E3xx` ownership, `WO-W2xx` warnings from the types stage. This
is an enumeration of codes already in use, not an archaeology dig — see
"Completeness method" below for how that was verified, "Reserved,
not yet emitted" for codes the source declares but no check yet raises,
and "Unreachable by design" for the one code (WO-E205) that isn't merely
unimplemented — it has no legal call site in the milestone grammar.
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

## WO-E2xx / WO-W2xx — types (Task 6, `compiler/src/types.ml`; WO-E214 Task 8, `compiler/bin/main.ml`)

| code | meaning | example message |
| --- | --- | --- |
| WO-W201 *(warning)* | a class has recursive/shared structure (a field, directly or through `ref`/`multi`/`map`/`?`, refers back to its own class) that the ownership pass cannot prove disjoint, has no `@table`, and has no `@unique` field — suggests `@gc`. | `Node has recursive/shared structure that borrow checker cannot prove. Consider adding @gc if this is an ephemeral in-memory cache. If this maps to a database table, keep owned (default).` |
| WO-E202 | a `.field` access names a field that the base's class (a *declared* class — an unresolved/placeholder expression type never triggers this) doesn't have. | `unknown field \`price\` on \`Product\`` |
| WO-E206 | a constructor literal (`ClassName { ... }`) omits a field the class declares (no default). | `missing field \`sku\` in constructor of \`Product\`` |
| WO-E207 | a constructor literal names a class that isn't declared anywhere in the (possibly multi-file) program. | `unknown type \`Widget\` in constructor` |
| WO-E214 | a class or interface name is declared more than once across the files a directory discovers (one program, multiple files — Task 8). Reported at the *later*-discovered declaration (sorted by path), with the first declaration as the related site; the merged symbol table keeps the first one, so this is what stops that silent keep from also hiding a real shape conflict. Driver-level, not `types.ml` — reuses the `types_prefix` range because it's a symbol-table concern, not a lexing/parsing/ownership one. | `class \`Dup\` already declared in \`a_first.wo\`` |
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

### Unreachable by design

`unsatisfied_interface_code` (WO-E205) is declared in `types.ml` but does not
belong in the list above — it is not a pending implementation, it is
unreachable by design given the milestone grammar. Structural interface
satisfaction has exactly one legal home: a site where a value is used at an
interface-typed position (a field, parameter, or return typed as an
interface). There is no `implements` keyword by doctrine — satisfaction is
structural, checked where the value is used, not declared — and the
milestone grammar declares no interfaces and exercises no interface-typed
positions, so the check has nowhere to fire. This is not a gap in shipped
work; it costs the milestone nothing. The check starts firing the moment a
future milestone introduces an interface-typed position — no interim
workaround is owed before then.

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
and `main.ml`'s `report_collision` are the only indirection layers
between a bare `~code:` argument and the `Diag.error` call — each was
read to confirm which named constant ultimately reaches the collector,
not just the arity-generic wrapper name. This is why the catalog is an
enumeration, not a dig: `diag.ml` already reserves the numeric ranges,
so the only open question per stage was *which* reserved codes actually
fire — answered by exhaustive grep, not inspection of a handful of
samples.
