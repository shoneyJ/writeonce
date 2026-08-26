# The `woc` diagnostic catalog — normative reference

Every `WO-E###`/`WO-W###` code the `woc` front end (`compiler/`) actually
emits, as of plan 2 tasks 2–8, plan 3 tasks 1–2, and the iterations that
added codes afterwards — 9b (WO-E250), 15/17 (WO-E106–E109), the
language-surface-strictness branch (WO-E219/E220), the shard-fiber arc
(WO-E221/E222), 24 (WO-E226), 36 (WO-E223) and databasev2 2 (WO-E224). Re-swept against the source
2026-08-26; the eight codes that sweep found missing are now in the tables
below. Code ranges are reserved
per stage (`compiler/src/diag.ml`): `WO-E0xx` lexing, `WO-E1xx` parsing,
`WO-E2xx` types, `WO-E3xx` ownership, `WO-E4xx` the bytecode emitter,
`WO-W2xx` warnings from the types stage. This
is an enumeration of codes already in use, not an archaeology dig — see
"Completeness method" below for how that was verified, "Reserved,
not yet emitted" for codes the source declares but no check yet raises,
and (until 2026-08-18) "Reachable but unenforced" for the one code (WO-E205) whose check
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
| WO-E003 | haxe-parity Task 8 (build flags). Misuse of the token-level `#if`/`#else`/`#end` filter: a `#if` with no flag name, a second `#else` in one section, a stray `#else`/`#end` with nothing open, a `#if` left open at end of file, or an unknown directive word. `Eof` always survives the filter so the parser still terminates after a reported error. Flags are NAMES only — no expressions — and are set with `woc -D <name>`. | ``unknown directive `#unless` — the build-flag directives are #if <flag>, #else, #end`` |
| WO-E004 | a raw text literal (backtick-delimited) that runs off the end of the file. Reported at the OPENING backtick — unlike a plain `"..."` string this is never silent, because multi-line is this form's normal case and a missing close would swallow every remaining line. | `unterminated raw text literal` |
| WO-E005 | a raw newline inside a `"..."` or `'...'` string. The scan stops at the newline without consuming it, so the `Newline` token still terminates the statement and only one line is lost. Multi-line text is spelled with a raw literal instead. | ``newline in string literal (use a `...` raw text literal for multi-line text)`` |

## WO-E1xx — parsing (Tasks 4–5, `compiler/src/parser.ml`; WO-E103 haxe-parity Task 2)

| code | meaning | example message |
| --- | --- | --- |
| WO-E101 | generic syntax error: an unexpected token where the grammar expected something else, including running off the end of the file inside an unclosed block/type/interface body. Declaration-level recovery syncs to the next top-level keyword so one bad declaration yields one diagnostic, not a cascade. | `expected ')' or ',', got NEWLINE` |
| WO-E102 | an invalid `@table(...)` configuration. Original causes: `name` given twice, an `index` with no columns, or an unknown argument key. **databasev2 2 (2026-08-26) added the storage arguments and five more causes under the same code:** `durable` given twice; `resident` given twice; a `resident` value other than `all`/`keys`; `resident: index`, which is the pre-review spelling and gets a message naming its replacement; and a *retired* argument word (`mode`, `store`, `ram`, `cold`, `tiered`, `paged`, `mmap`, `buffer` — vocabulary the design brainstorm explored and rejected), which gets a message stating the two real keys rather than a generic "unknown argument". A non-boolean after `durable:` is WO-E101 from the generic token expectation, not this code. Also `durable: false` together with `resident: keys` — rows would be neither logged nor resident, checked after the whole argument list is known because it is a property of the combination; the loader refuses the same pair independently (`.wob` v7). | `` `cold` is not a @table argument — storage is declared with two keys: `durable: true\|false` and `resident: all\|keys` `` |
| WO-E103 | haxe-parity Task 2. `inline fn ...` — the haxe keyword verdict table's own reject half of the `inline` row (`const` values are the adopted half). The whole declaration is discarded by the usual top-level recovery, same as any other bad declaration. | `` `inline fn` is rejected — optimization is the compiler's job `` |
| WO-E106 | iteration 15 (deps, 2026-08-18). A dependency fetch/shape failure, driver-level: missing `git` binary, clone/checkout failure, a locked commit missing from the remote, cache/lock drift (a moved `rev` — the message names both SHAs and points at `woc --update-deps`), a fetched dep that is not a writeonce project, or a dep declaring its own `[deps]` (transitive — refused flat-only). One code; the message names the dependency and the failing step. | `` dependency `niceframework`: lock drift — wo.lock pins <sha> but .wo-deps has <sha> (a moved `rev`?); run `woc --update-deps` or remove .wo-deps/niceframework `` |
| WO-E107 | iteration 15 (deps). A `[deps]` name collides with a local top-level module directory of the same name — `use <name>` would be ambiguous, so the build refuses instead of silently picking one. | `` dependency `niceframework` collides with the local module directory `niceframework/` `` |
| WO-E108 | iteration 17 (library kind + `internal/`, 2026-08-20). A `use` path reaches a module under a dependency's `internal/` — Go's rule, enforced at the *consumer's* own `use` and only across the `[deps]` boundary: a library imports its own interior freely. Driver-level (`compiler/bin/main.ml`), reported at the offending `use`. | `` `serve/internal/parse` is internal to the dependency `serve` — a module under `internal/` is the library's own business and cannot be imported across the `[deps]` boundary `` |
| WO-E109 | iteration 17. An unknown `kind` value in `wo.toml`. Manifest-level: printed directly and exits 2 rather than joining the collector, the same as WO-E106/E107. `program` is the default, so every manifest written before the key existed stays byte-identical. | ``woc: wo.toml: error WO-E109: unknown `kind` value `lib` — expected "program" (the default) or "library"`` |
| WO-E105 | iteration 5 strictness (2026-08-18). A rejected Haxe keyword used where it would otherwise misparse — or, worst, compile clean (`return super.f()` used to): `extends`/`implements` after a class name, and `extends`/`implements`/`super`/`override`/`cast`/`Dynamic`/`untyped`/`macro`/`extern`/`operator` as an expression head or a top-level declaration head. Each cites the systems-track verdict table's doctrine reason. (`Dynamic`/`untyped` as a *type name* fire WO-E225 with the same doctrine message.) | `` `extends` is rejected: no inheritance, ever — is-a is a tagged union, has-a is composition, polymorphism is structural interfaces (principle 4) `` |
| WO-E104 | iteration 7b. `@gc` on a class — the annotation is gone: GC-ness is inferred (structural cycles + demand promotion; `woc --dump-gc`). The annotation is skipped for recovery and the class classifies by inference. | `` `@gc` is not a valid annotation: GC-ness is inferred by the compiler (run `woc --dump-gc`). Remove it. `` |

## WO-E2xx / WO-W2xx — types (Task 6, `compiler/src/types.ml`; WO-E214 Task 8, `compiler/bin/main.ml`; WO-E215 plan 3 Task 2, `compiler/src/types.ml`; WO-E210/E216–E218/W202 haxe-parity Task 1, `compiler/src/types.ml`; WO-E201 haxe-parity Task 2, `compiler/src/types.ml`; WO-E208 haxe-parity Task 3, `compiler/src/types.ml`; WO-E203 haxe-parity Task 4, `compiler/src/types.ml`)

| code | meaning | example message |
| --- | --- | --- |
| WO-E201 | haxe-parity Task 2. An `and`/`or` operand whose type is confidently known (the same narrow, "stay silent when underivable" deriver WO-E209 uses — `confident_typ`) and is not `Bool` — this language has no truthiness. Reserved since Task 6, its first real emission site. Haxe-parity Task 3 gave it two more sites: a `switch` expression's arms disagree on their yielded type (the switch's own type is fixed by the first arm — types.ml's "first wins" convention — every later arm is checked against it, via the regular `.typ` inference, not `confident_typ`; since haxe-parity Task 4 the comparison is *structural* — two typedef records with the same shape are the same type, `typ_equal`); and (review fix, Critical 2) a `case` value whose representation (`WO_K_TEXT` vs. `WO_K_SCALAR`) doesn't match the switch subject's — a real VM segfault if unchecked (a `Text` subject picks EQS, and EQS's `str_check` dereferences whatever sits in a mismatched `Int` case value's register), checked via `confident_typ`, silent when either side is unresolved. Haxe-parity Task 4 added the union-subject site: a `case` value over a confidently union-typed subject that does not name one of that union's variants (a misspelled variant, a variant of some other union, or a plain literal — union arms match variants, never values). Task 4's fix round 1 added three inverse/porosity sites, each a reviewer-reproduced silent-wrong-behavior hole: a variant-named `case` over a confidently **`?Union`** subject (it can never match — `switch` does not narrow `?T`; the message points at handling nil first, since forced handling is Task 6's), a variant-named `case` over a confidently **non-union** subject (`switch n { case Lo: }` over `n: Int` silently ordinal-matched), and a **cross-union `==`/`!=`** (`X == P` from two different bare unions was silently true whenever the ordinals matched; same-union comparison stays legal). Lexical scope wins at every one of these sites — a local sharing a variant's name is never misread as one. | `` `Wat` is not a variant of union `Kind` `` |
| WO-E203 | haxe-parity Task 4 (`typedef` records + enum payload variants). A payload variant's argument count doesn't match its declaration, at either of the two places payload fields are positional: a construction (`Failed("a", "b")` against `Failed(reason: Text)`) or a `switch` pattern (`case Failed(a, b):`). The pattern site also rejects a non-name argument (`case Failed("x"):` — payload fields are bound positionally, never matched by value) and a payload-binding pattern sharing its arm with other values (`case Failed(r), Pending:` — the binding would be meaningless on the other match). Reserved since plan 2 Task 6; these are its first real emission sites, scoped to variant payloads only — user `fn`/method call arity is still the emitter's WO-E403, unchanged (see "Reserved, not yet emitted" below for the history of that gap). | `` variant `Failed` of `Status` takes 1 payload argument(s), given 2 `` |
| WO-W201 *(retired, iteration 7b)* | Suggested `@gc` for a recursive/shared class. Retired: `@gc` no longer exists (WO-E104) and inference classifies exactly these classes as traced automatically, so the suggestion is obsolete. | *(no longer emitted)* |
| WO-E205 | iteration 5 strictness (2026-08-18). A confidently class-typed value used at an interface-typed position — a call argument, an annotated `let`, or a `return` — where the class does not structurally satisfy the interface (Go-style: an instance method of the same name and parameter count for every interface method; a `static fn` never satisfies). Provable statically, so it fails at compile time instead of `ICALL`'s no-vtable-entry `WO_T_BOUNDS` trap — the hybrid boundary restored. Silent when the value's type is underivable. | `` `Rock` does not satisfy interface `Priced`: no matching instance method `current_price` `` |
| WO-E202 | a `.field` access names a field that the base's class (a *declared* class — an unresolved/placeholder expression type never triggers this) doesn't have. | `unknown field \`price\` on \`Product\`` |
| WO-E206 | a constructor literal (`ClassName { ... }`) omits a field the class declares that is neither defaulted nor nullable. Haxe-parity Task 4 narrowed it from "omits any field with no default was already the rule, but defaults were unenforceable" to the real omittability rule: a field with a declared default is filled by the emitter (`TailState {}` — the sample's defaults-fill-in pattern), and a `?`-typed field omitted is nil (the zero word `NEW` already leaves), for classes and typedef records alike. | `missing field \`sku\` in constructor of \`Product\`` |
| WO-E207 | a constructor literal names a class that isn't declared anywhere in the (possibly multi-file) program. `typedef` records (haxe-parity Task 4) are classes to this check — a record name resolves here like any declared class. | `unknown type \`Widget\` in constructor` |
| WO-E208 | haxe-parity Task 3 (`switch` as expression); the union exhaustiveness rule is haxe-parity Task 4's. Two subject regimes: (1) a **union-typed** subject (derived via `confident_typ`, the "stay silent when underivable" deriver — an underivable subject falls to regime 2) needs no `default` exactly when every variant is covered by some arm; a gap fires this code and names the missing variants, in declaration order. A `default` always satisfies it. (2) every **other** subject (scalars, Text, and anything underivable) keeps Task 3's unconditional rule: no `default` arm is always this error. A `?Union` subject is deliberately regime 2 until Task 6's forced-handling work legalizes narrowing it. | `` switch over `Kind` has no `default` arm and does not cover: Mid, Hi `` |
| WO-E209 | hotfix (2026-08-11, round 2). A builtin call (`print`, `print_int`, `words`, `now`, `push`, `get`, `count`, `latest`, `set`, `has`, `multi_new`, `map_new`) given the wrong number of arguments, or an argument whose type is confidently known and does not match the builtin's signature (source of truth: [`08-builtin-surface.md`](08-builtin-surface.md)). Motivated by a real segfault: `print(7)` compiled clean and crashed `wovm` — `print` wants a `Text` (a heap-string pointer), and the VM's `str_check` dereferences whatever register it is handed as a `wo_str*` with no runtime tag to check first, so a bare `Int` was a wild pointer read. `types.ml`'s own `confident_typ` (deliberately narrower than the typechecker's regular `.typ` inference — see its doc comment) derives an argument's type from a literal, `self`, a parameter's declared type, a class field's declared type, a `Ctor` naming a real declared class, a `let` whose value was itself confidently typed, or (round 2 — round 1 missed this, a real second segfault repro: `print(takesSecret(box))` where `takesSecret` is declared `-> Int`) a `Call` whose declared signature is known: a free fn (resolved own-module-first-then-used-modules, never the flat whole-program symbol merge — the same Critical-1 bug shape haxe-parity Task 1 already fixed for the emitter), a class method off a confidently-typed receiver, an interface method's signature, or another builtin's own confident return type (`words`/`count` → `Int`, `now` → `Timestamp`, `has` → `Bool`, …). `ReqInt` accepts any non-`Text` builtin scalar (`Int`/`Bool`/`Timestamp`/`Id` all share the identical `WO_K_SCALAR` runtime representation — found as a real false positive against `print_int(has(...))` once builtin return-chasing went live). Anything still not chased (an unresolved name, an `Index`/`Binary` result, a qualified free-fn call through a `use` alias, an UNKNOWN-BUT-RESERVED stdlib call) is left unchecked rather than guessed at — see "Remaining unchecked surface" below. A user-declared free `fn` of the same name always wins over the builtin table (08-builtin-surface.md's shadowing rule; also resolved module-aware, not via the flat merge), so a shadowed name is never checked here either. Arity mismatches are also still caught later, at emission (`WO-E403`, unchanged) — this is an earlier, additional gate over the same contract, not a replacement. | `` builtin `print` expects Text, got `Int` `` |
| WO-E210 | haxe-parity Task 1 (modules). A `Ctor`/bare-call name resolves — it's declared, somewhere — but not in this file's own module and not through any `use` edge either; the module it actually lives in is named as a hint. Reserved by Task 6's own brief, genuinely blocked until a module concept existed at all (see the nullable-types-implementation.md handoff) — this is its first real emission site. | `` `helper` is declared in module `shared/util`, which is not `use`d here `` |
| WO-E211 | iteration 5 strictness (`?T` forced handling, 2026-08-18). A possibly-nil value (`?T`, or a literal `nil`) used where a plain value is required, un-narrowed: an arithmetic or `<`/`<=`/`>`/`>=` operand, an `and`/`or` operand (`?Bool`), an interpolation segment, a `for` iterable, or a `return` whose declared type is not nullable. Narrowing legalizes it: `if x != nil { … }` narrows the LOCAL `x` to `T` inside the branch; a diverging then-branch (`if x == nil { return … }`) narrows after the `if`; `x != nil and x.n > 3` narrows the right operand; `while x != nil` narrows the body. Field places (`a.next`) never narrow — bind to a local first. Env types here are declared or confidently inferred (the fallback placeholders are plain scalars, never `?T`), so this cannot false-positive off an underivable expression. | `` `x` is possibly nil (`?T`) and is used where a plain value is required — narrow it first (`if x != nil { ... }`) `` |
| WO-E212 | iteration 5 strictness. `nil` or a `?T` value stored across the boundary into a slot whose DECLARED type is not nullable: an annotated `let`, an assignment to a local whose type is confidently known (cenv, never the placeholder env) or to a resolvable class field. | `` `nil` cannot be stored in `x: Int` — the target is not nullable; declare it `?T` or narrow the value first `` |
| WO-E213 | iteration 5 strictness. Reaching through a possibly-nil value — a field read, an index — without a nil check. The deref twin of WO-E211: the base itself is `?T`. | `` field `next` is possibly nil (`?T`) — check it against `nil` before reaching through it `` |
| WO-E214 | a class or interface name is declared more than once across the files a directory discovers (one program, multiple files — Task 8). Reported at the *later*-discovered declaration (sorted by path), with the first declaration as the related site; the merged symbol table keeps the first one, so this is what stops that silent keep from also hiding a real shape conflict. Driver-level, not `types.ml` — reuses the `types_prefix` range because it's a symbol-table concern, not a lexing/parsing/ownership one. | `class \`Dup\` already declared in \`a_first.wo\`` |
| WO-E215 | a class, interface, free `fn`, or (haxe-parity Task 4) union name is declared more than once in the *same file* (`collect_declarations`'s own `StringMap.add` silently dropped the earlier one — Task 1 review, found while building the plan-3 emitter, fixed in Task 2). Task 4 also fires it for a *variant* name reused within a file's unions (inside one union or across two — variants share one flat value namespace, so a bare `Ok` reference could not otherwise pick a tag), reported at the reusing variant with the owning union as the related site. Reported at the later declaration, with the first as the related site — the same shape as WO-E214, one file instead of two; the symbol table keeps the first declaration. Class/interface names and free-fn names are separate namespaces, so a class and a fn sharing a name never collide here. | `class \`Dup\` already declared` |
| WO-E216 | haxe-parity Task 1 (modules). A `use <path>` names something that is neither one of the six reserved stdlib namespaces (`fs`, `proc`, `net`, `time`, `json`, `env`) nor a directory this program actually discovers. | `` unknown module `nosuchmodule` — not a discovered project module and not a reserved stdlib namespace `` |
| WO-E217 | haxe-parity Task 1 (modules). A qualified reference (`alias.name(...)`) names a real declaration in a real, `use`d module, but that declaration has no `pub` marker — private to its own module. | `` `hidden` is not `pub` in module `secret` `` |
| WO-E218 | haxe-parity Task 1 (modules). A bare (unqualified) name resolves as `pub` in *more than one* used module — "collisions diagnose rather than shadow silently" (the plan's own words): resolution never silently picks a winner among used modules, it fails loudly and names every alias that matched. | `` `thing` is ambiguous — exported `pub` by more than one used module (a, b) `` |
| WO-E225 | a field's declared type name isn't a builtin scalar, a declared class (typedef records included), a declared interface, or (haxe-parity Task 4) a declared union. Also checked, same shape, on a payload variant's field types. A dotted name whose head is a reserved stdlib namespace (`json.Value`) is accepted as UNKNOWN-BUT-RESERVED — the same plan-9 convention `fs.stat(...)` calls get; any other dotted name is as unknown as a misspelling. Checked once per field declaration, at the field's own position. | `unknown type \`Wdiget\`` |
| WO-E219 | language-surface-strictness branch. A `pub(read)` field written from outside its declaring class — the marker means readable anywhere, writable only inside. Reported at the write site. | `` field `total` of class `Cart` is pub(read) — readable anywhere, writable only inside `Cart` `` |
| WO-E220 | language-surface-strictness branch. A name is both a real method of the receiver's class and a `using` extension in scope — an extension never overrides a method, so the collision is refused rather than silently resolved one way. | `` `render` is both a real method of `Page` and a `using` extension — rename one; an extension never overrides a method `` |
| WO-E221 | the shard-fiber arc (iteration 8+11). A `spawn C { ... }` whose target class declares no `fn receive(msg: M)`, or declares one whose `M` is not a class, record or union. `receive` is an ordinary identifier, not a keyword — declaring it is what makes a class an actor. | `` `spawn Worker { ... }`: no `fn receive` — an actor is a class with `fn receive(msg: M)` where M is a class, record, or union `` |
| WO-E222 | the shard-fiber arc. A traced (inferred-GC) type, or a type containing one, used as an actor message or as actor state. Aliased object graphs cannot cross shard heaps, and `spawn` placement makes every actor potentially remote, so this is refused statically rather than trapped at the send. | `` message type `Graph` is traced (or contains a traced class) — aliased graphs cannot cross shard heaps; spawn placement makes every actor potentially remote `` |
| WO-E223 | iteration 36 (operators). A literal shift count outside `0..63` on `<<` or `>>`. A non-literal count is not caught here — the VM traps it at run time (`WOP_SHL`/`WOP_SHR`). | ``shift count is out of range 0..63 for `<<` `` |
| WO-E224 | databasev2 2 (2026-08-26). A **durable** table holds a `ref` into a **volatile** one (`durable: false`). The referencing row survives a restart and the referenced row does not, so the stored row id dangles — and FK restrict cannot catch it, because restrict asks "does a row reference this?" and after a restart the honest answer is no while the id is still sitting in a durable slot. Provable from the class table, so it fails at compile time instead of becoming a wrong query result. Reported once, at the field's own declaration. Only this direction is refused: **volatile → durable is legal** (the referencing row is the one that disappears, leaving nothing holding a stale id), and a `backlink` is never checked at all — it stores no column, so after a restart it resolves to an empty collection, a legal state indistinguishable from "nothing references me". Sees through a `?` wrapper. | `` durable table `Order` cannot hold `ref Session`: `Session` is declared `durable: false`, so its rows are gone after a restart and this stored row id would dangle — FK restrict cannot catch it `` |
| WO-E226 | iteration 24 (`call`). `call`'s reply type has to survive actor-`M` erasure, so every `fn receive(msg: M)` program-wide must declare the SAME return type, and in v1 that type must be a copyable scalar. Fires when two `receive` declarations disagree, or when the agreed type is not scalar. | `` `call` on `actor Msg` needs one reply type, but `Room` and `Registry` declare different `receive` returns `` |
| WO-E250 | iteration 9b (the query surface). Every diagnostic the language-integrated query grammar raises, one code: a `from v in C` whose `C` is not a declared table class, a navigation source that is not a `backlink`/`multi` of a table class, and the two not-yet-supported clauses — `group … by … into` on a table query and on a navigation query. The group-by rows are why the clause parses and still cannot run. | ``group-by aggregation is not supported yet`` |
| WO-W202 *(warning)* | haxe-parity Task 1 (modules). A file's own `use` clause is never actually referenced — neither a bare name resolving through it nor a qualified `alias.name(...)` call — anywhere in that file's surviving parse tree. | `` unused `use fs` `` |
| WO-W203 *(warning)* | haxe-parity Task 3 review fix (Critical 1). A `switch`'s `default` arm is not textually last — no longer a silent dead-code trap (`default` is lowered last regardless of source position, `Ast.switch_lowering_order`), but still surprising source; fired once per switch, at `default`'s own position. | `` `default` is not the last arm -- a `case` written after it still matches (this compiler evaluates `default` last regardless of source position), which reads as dead code `` |

### Reserved, not yet emitted

`unknown_fn_code` (WO-E204) is declared in `types.ml` — the range is
reserved — but nothing in the front end raises it yet. (WO-E211/E212/E213
left this list 2026-08-18: `?T` forced handling is enforced; see their rows
in the main table above.) `module_not_imported_code`
(WO-E210) — the one member of this list Task 6's brief named that a *later*
task, not a gap in Task 6's own shipped work, was blocking — has moved up
into the table above: haxe-parity Task 1 gave it its first real emission
site. `invalid_builtin_code` (WO-E209) has also since moved up into the
table above — a 2026-08-11 hotfix wired it (a real `print(7)` segfault
forced the issue; see its row above) — and `type_mismatch_code` (WO-E201)
has too: haxe-parity Task 2 wired it for `and`/`or`'s non-`Bool`-operand
check (see its row above). `non_exhaustive_switch_code` (WO-E208) has also
moved up: haxe-parity Task 3 (`switch` as expression) wired it — the exact
"genuinely blocked: no `switch` keyword exists yet" gap the nullable-types
dead-code register named is closed (see its row above). `bad_arity_code`
(WO-E203) has now moved up too: haxe-parity Task 4 wired it for variant
payload arity (construction and pattern sites — see its row above). That
wiring is deliberately narrower than the code's own name promises: a
user-declared `fn`/method call's arity is still ungated here and caught
only at emission (WO-E403), the same pre-existing gap the WO-E209 hotfix
note already disclosed for builtins. Four of the original ten remain dead
(WO-E204, WO-E211, WO-E212, WO-E213 — the last three are Task 6's own
forced-handling work). Listed here so a conformance fixture (plan 3) or a
future reader doesn't assume one of the remaining codes is reachable
today; move a code up into the table above in the same commit that wires
its first real emission site.

### Reachable but unenforced

**WO-E205 was wired 2026-08-18** (branch `type-enforcement`) — the owed gap
this section used to record is closed. Structural satisfaction (`types.ml`'s
`class_satisfies`, the same name+arity+non-static rule `emit.ml`'s
`satisfies` builds vtable rows from) is checked wherever a confidently
class-typed value flows into an interface-typed slot: a call argument
against the callee's declared parameter, an annotated `let`, and a `return`
against the declared return type. The canonical evidence program now fails
compile at the `quote(r)` call site, and the known-gap fixture moved to
`tests/corpus/compile-fail/unsatisfied-interface/` (`fixture.code WO-E205`)
in the same change, exactly as its own comment demanded. See the WO-E205 row
in the main table above.

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
| WO-E403 | a construct the v1 instruction set cannot express, or a call the emitter cannot lower correctly. The full source-surface contract is [`08-builtin-surface.md`](08-builtin-surface.md); the cases raised here are: an unresolved name; a call to something that is neither a declared `fn` nor a builtin; a wrong argument count (nothing upstream checks arity — WO-E203 is declared and never raised — and a mismatched call reserves a window the callee does not read, which the loader rejects); a field/method on a type that is not a declared class; `multi_new()`/`map_new()` with no destination of declared type (the element kinds are the container's runtime drop plan and cannot be guessed); an element write into a `multi` (v1 has `multi_push`/`multi_get`, no element store); a `for` over a `map` (v1 exposes no key enumeration); (haxe-parity Task 2) `break`/`continue` with no enclosing loop — nothing upstream tracks loop nesting to reject it earlier; (haxe-parity Task 2) a `"${expr}"` interpolant whose type is neither `Text` nor `Int`, or is unresolvable; (haxe-parity Task 4) a field default the emitter cannot lower when a constructor literal omits that field — defaults are opaque token spans by design (ast.ml), and the lowerable set is exactly the sample's own shapes: Int (optionally negated), Text, and Bool literals, `now()`, and `[]` (an empty container, kinds from the field's declared type). Several of these are cases the typechecker's placeholder types let through — the emitter is the first stage that must be exact. | `` `for` can only iterate a `multi` — the v1 builtins expose no key enumeration for a `map` `` |
| WO-E404 | an ownership-table entry the emitter could not honor: a residual borrow site whose operand has no register at the guarded region, a residual region no lowering wrapped at all (checked at the end of every compilation unit — owner.ml anchors regions on several different node kinds, and one nobody consumed would ship the aliasing check silently disabled), or a drop/rc site naming a local that has no register. Emitting such a region unguarded would drop the single enforcement a residual site exists for, so it fails instead. | `` residual borrow site in `shuffle` names an operand with no live register — the runtime guard cannot be placed `` |
| WO-E405 | the program entry (the zero-arg free fn `main`, selected by name) declares a return type other than `Int`. The systems-track spec (`docs/superpowers/specs/2026-08-01-systems-track-design.md:70`) makes the entry's return value the process exit code, so any other declared return type was never legal — this is the check that finally says so. `main` with no return annotation at all is unaffected (nothing declared to contradict `Int`); every other milestone-1 fixture uses that form. Reported once, at `main`'s own position. | `` entry `main` declares return type `Node` — the entry's return value is the process exit code, so it must return `Int` `` |
| WO-E406 | haxe-parity Task 1 (modules). A call through a reserved stdlib alias (`use fs`/`proc`/`net`/`time`/`json`/`env`) survived typechecking — `types.ml` accepts it as UNKNOWN-BUT-RESERVED, since the six namespaces' members arrive in plan 9 — and reached emission. There is nothing to lower it to yet; an unused `use fs` never reaches this code at all, only a call that actually goes through it. | `` stdlib module `fs` is not linked in this milestone (called as `fs.stat`) `` |

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

**The method is sound; re-running it is the maintenance.** The sweep was
not repeated between plan 3 and 2026-08-26, and eight codes accumulated
outside the table in that window (WO-E003, E108, E109, E219–E223, E226,
E250) — one of them, WO-E250, the only diagnostic the entire shipped query
surface raises. Note the extra indirection the re-sweep had to follow:
codes are built as `<stage>_prefix ^ "NN"`, so a grep for the literal
string `WO-E250` finds only the comment beside the constant, never the
emission. Sweep for `_prefix ^ "` and resolve each constant, plus the
handful of driver codes that `Printf.eprintf` a literal `WO-E1NN` and exit
2 without touching the collector at all (WO-E106/E107/E109). Any iteration
that adds a code adds its row here in the same change.
