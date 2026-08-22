# `compiler/src` — how `woc` is put together

Written 2026-08-14, when the front end grew the language surface that compiles
`docs/examples/log-watcher`. The normative contracts it emits against are
[`docs/plan/oop-vm/00-wob-format.md`](../../docs/plan/oop-vm/00-wob-format.md)
and [`08-builtin-surface.md`](../../docs/plan/oop-vm/08-builtin-surface.md);
the diagnostic codes are catalogued in
[`01-error-catalog.md`](../../docs/plan/oop-vm/01-error-catalog.md).

## The pipeline

```
lexer.ml   →  parser.ml  →  types.ml  →  gcinfer.ml  →  owner.ml   →  emit.ml  →  .wob
tokens        AST           symbols      traced set     move/drop     bytecode
                            typecheck      tables
```

`bin/main.ml` drives it: discover files (a directory is one program), parse each,
collect declarations per file, check module edges, merge symbols, typecheck,
run the owner pass per file, then emit one image from every unit. `diag.ml`
accumulates every stage's diagnostics and sorts them by (file, line, col), so
ordering never depends on discovery order. `dump.ml` renders the stable text
dumps the golden tests diff; `disasm.ml` reads an image back.

Four things are worth knowing before editing any of it.

### 1. Two type derivers, deliberately

`types.ml`'s `confident_typ` and `emit.ml`'s `ty_of_expr` both answer "what type
is this expression?", in different languages (`Types.typ` vs `Ast.field_ty`) and
for different purposes: the first gates diagnostics, the second picks
instructions (EQ vs EQS, a container's element kinds, whether a value is owned).
They are kept in sync by hand, and both follow one rule: **stay silent when
underivable**. `confident_typ` returns `None`; the emitter falls back to `Int`.
That is why a check built on `typecheck_expr`'s `.typ` (which reports `Int` for
anything unresolved) produces false positives, and every new check should read
`confident_typ` instead.

A third table pair follows the same discipline: `Types.builtin_confident_ret`
and `emit.ml`'s `builtin_ret` give each builtin's return type. An omission there
is not a lost type — it is a **leak**, because the owner pass classifies a
binding as owned from exactly that answer.

### 2. Contextual values need a destination

`[]`, `[a, b]`, `{}` and `nil` have no type of their own. They take it from,
in order: a written `let` annotation, the field/parameter they are built into,
the enclosing method's declared return type (`fstate.f_ret`), or — for a
non-empty list — their own first element. With none of those, emission is a
diagnostic, never guessed bytecode: a container's element kinds *are* its
runtime drop plan, so a wrong guess leaks or double-frees. `nil` is the zero
word for every `?T` (the format doc's own rule), which is also why a comparison
against `nil` must lower to `EQ` and never `EQS`.

### 3. The owner pass hands the emitter tables, not decisions

`owner.ml` computes moves, scope-end drops, branch-join drops and residual
borrow guards, keyed by **node id and label** (rc sites are gone since
iteration 7b — reference counting no longer exists; `gcinfer.ml` classifies
each class owned/traced first, structurally via SCC over the class-reference
graph plus demand promotion at escape sites, and `Types.is_gc_class` answers
from that set). `emit.ml` looks them up
by the same keys. When a construct has arms — `switch`, `if`, `try` — both files
must agree on the label strings and on the arm ORDER (`switch_lowering_order`
moves `default` last in both). A silent mismatch means a drop that never runs.

`try`'s shape: the catch arm is an alternate flow joining the try arm, so
`analyze_try` snapshots the entry state, walks the body, restores, walks the
handler with `e` declared as an owned local, and then makes each arm drop what
the other moved. The handler starts from the *entry* state on purpose — a trap
can be raised after any prefix of the body, and claiming the body's moves
happened would drop values the VM already released.

### 4. Statics, modules and the stdlib all arrive as `Ident.member` calls

A qualified call's head can be four things, resolved in this order: a value with
a type (an ordinary method call), a class with a static method
(`Flock.held(x)` — `static_method`), a reserved stdlib module
(`fs.stat(path)` — `Types.stdlib_members`), or a `use` alias for a project
module. Adding a fifth kind means extending that chain in both `emit_call` and
`ty_of_expr`, and `confident_typ` for the diagnostic side.

The stdlib table is data: module, member, source arity, builtin id, return
type, and the predeclared record whose class id gets appended as the call's last
argument. `json.encode`/`json.decode` are the two exceptions with bespoke
lowering — encode needs its argument's static kind, and decode has no type at
all until an `as` names one, which is why `json.decode(t) as T` is one
instruction and a bare `json.decode(t)` is an error.

## Predeclared records

`Error` (a catch arm's error), `Stat`, `TimeParts`, `Proc` (stdlib results) are
declared by `types.ml`, not by any source file. They join the **merged** symbol
table only — one copy per file would read as a cross-file duplicate — and they
enter the class table only when a program actually needs one, so images that
predate the surface keep their exact class tables. Their field ORDER is the
contract with the runtime, which writes those fields by index.

## Emitting the class table (a trap to remember)

Field-name constants must be interned **with every other constant**, before the
constant pool is serialized. Interning during class-table serialization appends
constants the pool has already been written past: the image then references
constants it does not contain, and the loader rejects every class. That bug cost
a debugging round; the interning now happens beside `class_name_k`.

## Register discipline in `emit.ml`

Locals live below `f_nlocals`, temporaries from `f_temp` upward, and a
statement resets `f_temp` to `f_nlocals`. Any construct that writes into a `dst`
which might itself be a temp (`switch`, `try`, a ctor, a container literal) must
reserve `dst` before allocating more temps, or an arm-local `let` can be handed
the same register and clobber a live value before its drop runs. `emit_switch`
carries the comment explaining the ASan-confirmed leak that taught this.

## Who owns a value nobody named

The drop tables (`owner.ml`) track **bindings**. Everything a statement builds
and never binds is the emitter's problem, and the workload found six of them:
an operand of a comparison (`if parse_expr(s) == nil`), an argument a callee
only borrows, a container read's copy (`c[i]` is the one place expression whose
register holds a **copy**, so it needs no second copy at a boundary and does
need a drop), a loop's iterable, the record a projection reads a field of, and
any of those escaped by a `return` from inside the statement that built them.

The soak (Task 6) widened the list with four more, all the same sentence:
a `!=`'s operands (its lowering is separate from `==`'s and missed the reap);
an Int-typed interpolation segment (`"${resp.status}"` LOOKS like a place
wrapped in Interp, but lowers to a fresh int_to_text — is_borrowed_value_t
asks the type); the argument of `json.encode` (its bespoke lowering bypassed
the stdlib-member drop); and a discarded expression statement (`pop(lines);`
REMOVES the element — the caller owns what it then ignores). The finding tool
was an arena size-class census plus a pointer trace, not ASan: an in-arena
leak is invisible to LeakSanitizer, because the arena is one allocation.

The framework-v1 slice (2026-08-20) found the copy-side mirror of the
Int-segment lesson: `copy_place_text` matched only bare `Ident/Field/Index`,
so a Text-typed SINGLE-SEGMENT interpolation of a place
(`allow = "${r.method}"` with `r` a loop borrow) passed the place's own
register through a `let`/assignment boundary uncopied — the binding aliased
the row's field and its overwrite freed it (release-build crash the arena
hid from ASan). It now asks `is_borrowed_value_t && not is_container_read`,
exactly `drop_fresh_text`'s place test. The RETURN boundary had the same
hole (`return "${p.content}"` handed the caller the part's own string —
the multipart slice's arena corruption, two requests removed from the
crash): emit_return's place test now sees through `Interp` the same way,
while bare Ident/Field/Index behavior there is unchanged. Both flavors
pinned by `tests/corpus/run/interp-borrowed-field`.

The iteration-5 strictness closeout (2026-08-20) added three seams worth
knowing: `pub(read)` rides the field annotation list as a synthetic
"pub_read" marker and is enforced in the Assign case that already resolves
the target's class (WO-E219, `current_self` names the checking class —
class-owned writes, sibling instances included); `using` extensions are a
TYPECHECK-TIME rewrite — `types.ml` records (file, call-id) → fn name in
`using_rewrites` and `apply_using_rewrites` rewrites `recv.ext(a)` to
`ext(recv, a)` before owner/emit, which therefore carry zero
using-awareness (collision with a real method is WO-E220 — never a silent
win either way); `#if` is a token-stream filter at the end of
`Lexer.tokenize` (`Lexer.defines` filled by `woc -D`, WO-E003 for misuse)
— the parser never sees a directive.

Two rules the measurements imposed, both easy to get backwards:

- **Never drop an argument register after a `CALL`.** The callee's frame
  overlaps those registers (vm.c's window overlap), so after it returns they
  hold the callee's leftovers. Copy the value into a stash slot allocated
  *below* the call window before the call — `call_window`'s `temp_idx` — and
  drop the stash.
- **A statement-owned temporary must live in a local slot, not a temp.** A
  statement that opens a scope resets `f_temp` to `f_nlocals` for its body, so
  a loop reuses the register; the end-of-statement `DROP` then releases a loop
  counter and the value leaks. `f_stmt_drops` holds locals; `f_esc_drops` is
  the same registers seen from a `return`.

## Verifying a change

- `just woc-test` — unit assertions plus the golden suite (token/AST/owner/bc
  dumps and an OCaml re-implementation of the loader's validation). `WOC_BLESS=1`
  regenerates goldens; read the diff before blessing, it is a contract change.
- `just oop-e2e` — the conformance corpus: `run/` byte-exact stdout,
  `compile-fail/` exact diagnostic code, `trap/` exact trap code, `gc/` exact
  collector trace, plus the single-binary smoke.
- `./compiler/_build/default/bin/woc --emit docs/examples/log-watcher -o /tmp/lw.wob`
  — the acceptance workload. It must compile with zero diagnostics, and
  `runtime/wovm /tmp/lw.wob watch <file> 2 1` must tail a live file and alert.

## Float and Bytes (iteration 19)

- **A digit run is an Int unless a fraction or an exponent follows.** The
  lexer requires a DIGIT after `.` before committing to a Float, which is what
  keeps `0..10` a range rather than `Float 0.` followed by `.10`, and checks
  the exponent form (`e`, optional sign, at least one digit) before consuming
  anything, so `2eggs` is still `Int 2` then an ident. `c` in that branch is
  PEEKED, not consumed — the scan loop reads it, and adding it to the buffer
  first double-counts the leading digit (a real bug this went through).
- **The no-mixing rule lives in the typechecker, not the emitter.** The
  emitter picks the arithmetic opcode from whether EITHER side is a Float, so
  an unreported `1 + 2.5` would lower to integer ADD over f64 bits and produce
  a plausible wrong number with no diagnostic. `check_numeric_mix` reports the
  mix (WO-E201) off confident types only, keeping this file's stay-silent-when-
  underivable contract; `%` on a Float is rejected outright.
- **`Float`/`Bytes` are builtin scalars but not Int-shaped.**
  `is_scalar_shaped` excludes both by name alongside `Text`, or
  `print_int(price)` prints f64 bits as a huge integer and `trunc(digest)`
  reinterprets a pointer — the representation mismatch that predicate exists
  for.
- **Bytes is a heap-owned scalar, so every ownership rule that named `Text` by
  string had to name a predicate instead.** `Types.is_heap_scalar` is that
  predicate (owner.ml's four sites) and `is_heap_kind` is its emitter twin
  (kind 3 or 7, six sites). Miss one and a Bytes temp never drops, or a Bytes
  stored into a container aliases where a Text would copy.
- **`?Float` needs its own nil constant.** `nil_const_for` picks it, and the
  bit pattern is emitted as a FLOAT pool constant because it is far outside
  OCaml's 63-bit native int — `const_int` cannot express it at all. Float
  constants dedupe on BITS, since `0.0` and `-0.0` are `=`-equal in OCaml but
  must stay distinct, and NaN is not `=`-equal to itself.
- **A Float `order by` key uses `float_cmp`, not `op_lt`.** Raw-bit ordering
  puts negatives backwards (the sign bit makes `-1.0` compare greater than
  `1.0` as an integer) and leaves NaN wherever the comparison sequence drops
  it. `float-table-column` in the corpus pins the ascending order that a
  bit compare gets wrong.
- **Three parallel builtin tables must agree**: `Types.builtin_signatures`
  (arity + arg kinds), `Types.builtin_confident_ret` and its emitter twin
  `builtin_ret` (a missing entry for a fresh-heap result is a LEAK, not just a
  lost type), and `is_builtin_name` plus the id mapping. The loader's arity
  table and the OCaml twin in `compiler/test/runner.ml` are a fourth and fifth.

## Library kind and the `internal/` boundary (iteration 17)

Every part of this lives in the driver (`compiler/bin/main.ml`). No lexer,
parser, typechecker, VM, `.wob`, or GC change — `internal` is a path shape, not
a keyword, and visibility is name resolution at compile time.

- **`kind` is declared, not inferred.** `wo.toml`'s top-level `kind` is
  `"program"` (the default, so every existing manifest is byte-identical) or
  `"library"`; anything else is WO-E109 at exit 2. Go infers library-ness from
  the absence of `main`, which makes "you forgot the entry" and "this is a
  library" the same error — the whole reason to spend a manifest key here.
- **Check mode reuses `compile_image` whole.** The library branch resolves
  `[deps]`, enforces the `[runtime]` constraint, runs the full pipeline, and
  discards the in-memory image; no `target/` is created and no file is written.
  An entry-less image was already legal on that path (the `--emit` precedent),
  so "checks clean" means what "builds clean" means.
- **`manifest_parse` was RELOCATED above `build_mode`** so the no-entry error
  can read the manifest and say "this project declares itself a library"
  instead of only "no `main`". OCaml has no forward reference across top-level
  `let`s; types.ml solved the same problem the same way. `woc build <dir> -o
  <out>` never goes through `manifest_build`, so reading it inside `build_mode`
  is the only placement that covers the explicit-build path.
- **WO-E108 is consumer-only, and keys on the FIRST segment naming a dep.**
  That single condition is what makes the root project's own `internal/`
  directories immune, and the dep-owned branch (which prefixes `use internal`
  to `<dep>/internal`) is untouched, so a library imports its own interior
  freely. The match is on a whole path SEGMENT — a module named `internals` is
  ordinary public surface.
- **The offending `use` is left in the AST, not dropped.** The collector's
  has-error path already stops emission; removing the use would replace one
  clear diagnostic with a cascade of unknown-type errors from the same file.
- **Exit-code bands stay split**: WO-E108 is a diagnostic through the normal
  collector path (exit 1); WO-E106/E107/E109 are manifest errors printed
  directly (exit 2).

## Operator parity (iteration 36 — `.wob` v6)

- **Precedence went INTO existing rungs, not new ones.** `|`/`^` joined
  `parse_additive`, `&`/`<<`/`>>` joined `parse_multiplicative` — exactly
  Go's table (`token.go` Precedence), which exists to fix C's trap:
  `x & mask == 0` groups the AND first here. The ladder doc in `parser.ml`
  carries the worked examples.
- **`not` is a keyword at the unary level (Lua placement).** `not a == b`
  groups `(not a) == b`. Chosen over Python's looser placement because the
  grammar's ordering is already anchored to Lua by name and because
  Bool-only typing turns almost every misread into a compile error. It
  lowers on the existing EQ against a zero constant — no new opcode, the
  same doctrine as and/or's JZ lowering.
- **Compound assigns are parse-time sugar via rewind-and-reparse.**
  `x += e` IS `x = x + e`, the documented contract — including an index
  expression evaluating twice, exactly as the written-out form would. The
  parser re-parses the place by resetting `st.pos` (no expression rung
  consumes a compound token, so the second parse stops where the first
  did); every re-parsed node draws a fresh id, so owner/emit see two
  honest reads, never one node in two roles. `+=`/`-=` had been lexed
  since haxe-parity Task 2 but no rule consumed them — dead tokens,
  `x += 1` died as a generic WO-E101 until this iteration.
- **Bitwise is Int-only on BOTH sides (WO-E201 family)** — no F-twin
  exists, so a Float operand would have become a garbage word operation
  with no diagnostic. A LITERAL shift count outside 0..63 is WO-E223 at
  the operand's position (a negative literal arrives as
  `Unary(Neg, IntLit)` — both shapes are caught); a variable count is the
  VM's WO_T_SHIFT.
- **Hex/binary literals accumulate in OCaml's native int (63-bit).** A
  full-width 64-bit literal like `0xFFFFFFFFFFFFFFFF` is out of reach —
  all-ones is spelled `-1` (and complement is `-1 ^ x`; there is no `~`).
  The `0x`/`0b` prefix commits only when a real base digit follows, so
  `0xg` stays `Int 0` + `Ident` — a parse error at its own position, no
  new lexer diagnostic. `_` separators are consumed only BETWEEN digits.
