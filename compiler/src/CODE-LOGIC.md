# `compiler/src` — how `woc` is put together

Written 2026-08-14, when the front end grew the language surface that compiles
`docs/examples/log-watcher`. The normative contracts it emits against are
[`docs/plan/oop-vm/00-wob-format.md`](../../docs/plan/oop-vm/00-wob-format.md)
and [`08-builtin-surface.md`](../../docs/plan/oop-vm/08-builtin-surface.md);
the diagnostic codes are catalogued in
[`01-error-catalog.md`](../../docs/plan/oop-vm/01-error-catalog.md).

## The pipeline

```
lexer.ml   →  parser.ml  →  types.ml    →  owner.ml     →  emit.ml   →  .wob
tokens        AST           symbols +      move/drop/rc     bytecode
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

`owner.ml` computes moves, scope-end drops, branch-join drops, rc sites and
residual borrow guards, keyed by **node id and label**. `emit.ml` looks them up
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
