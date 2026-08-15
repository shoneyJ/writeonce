(* ast.ml — AST for `.wo` OOP source (milestone 1).

   Declarations (Task 4 of compiler/plan/2026-08-01-woc-compiler-front.md):
   `interface` (method signatures, no bodies), `class` and `type`
   (identical field grammar — Task 4 brief's own words: they differ
   only in the `is_class` flag, exactly mirroring crates/rt/src/ast.rs's
   `TypeDecl { is_class: bool, .. }` design), and `fn` (both as
   class/type methods and as free top-level functions — Task 6's brief
   mentions "free-fn tables", so free `fn` is real grammar here, not a
   rt carry-over). Statements and expressions (Task 5, below) live
   inside a method/fn's `body`, which Task 4 captured as a verbatim
   token span and Task 5 parses for real.

   Every declaration-shaped node (class/type, interface, method/fn,
   field, param) carries a unique `id` (monotonic per parse — Tasks 6/7
   key side tables, e.g. field-kind and ownership-state tables, on these
   ids) and a `pos` — the node's own starting source position. This
   codebase has no existing notion of a source *range* (Token.t and
   diag.ml's `site` are both single points), so `pos` follows that same
   single-point convention rather than inventing a new range type.

   `field_ty` and `default_expr` are plain payload, not "declarations" —
   they don't get their own `id`/`pos`; nothing downstream needs to key
   a side table on "this specific field's type expression" independent
   of the field that owns it.

   Task 5 adds real statement/expression ASTs (`stmt`/`expr` below) and
   retires the body-as-token-span placeholder: `method_decl.body` is now
   `stmt list`, not a verbatim span. Every `stmt` and every `expr` gets
   its own `id`/`pos` too — unlike `field_ty`/`default_expr`, Tasks 6/7
   name concrete per-node consumers (every expression gets a type; move
   sites, rc sites, and residual sites are individual call-argument and
   indexing expressions), so these *are* the "declaration-shaped" case
   the paragraph above describes, not the opaque-payload case.

   One disclosed gap in the parent-id-<-child-id invariant Task 4 set up
   for the declaration skeleton (a container's id is minted before its
   children's): a `stmt`'s id is still minted before its own
   sub-expressions/sub-blocks are parsed, so that half holds exactly as
   before. It does NOT extend to expr-inside-expr — left-recursive
   binary/postfix parsing (`a + b`, `a.b`, `a(b)`) parses the left/base
   operand first (smaller id) and only decides to wrap it in
   `Binary`/`Field`/`Call` after seeing the next token, so that
   wrapper's id is necessarily minted *after* its own child's. Every id
   is still unique and monotonic in mint order; only the strict
   parent-<-child direction is given up, and only for expr-in-expr
   nesting. Forcing it there would mean pre-reserving ids speculatively
   before knowing a wrapper is even needed — not worth the complexity
   for side-table keys that only need uniqueness, not order. *)

type pos = {
  line : int;
  col : int;
}

(* `ref`/`multi`/`map` are not lexer keywords (Task 3 deliberately
   dropped rt's schema-keyword zoo) — they're recognized positionally,
   by name, only at the start of a field's type, exactly like rt's own
   `insert`/`select` statement-keyword convention. Scalar also covers a
   bare class name used as an owned-embed field type (spec section 4:
   "Fields hold owned values, ref T ids, ... or @gc references") —
   Task 6 decides whether a given Scalar name is a builtin scalar or a
   user class. `?T` nullable wrapper (adopted from Haxe, systems-track
   spec Part 1) wraps any field_ty; milestone-1 has no array (`[T]`)
   or tagged unions — those are rt schema-layer features not named in
   this task's grammar, so they're deliberately absent here. *)
type field_ty =
  | Scalar of string
  | Ref of string
  | Multi of string
  | Map of string * string (* key type, value type: map<K, V> *)
  | Nullable of field_ty   (* ?T wrapper *)

(* Parameter passing convention (spec section 3, rule 2): default is an
   immutable borrow; `mut` is an exclusive borrow; `take` moves
   ownership in. The owner pass (Task 7) is the eventual consumer. *)
type param_conv =
  | Borrow
  | Mut
  | Take

type param = {
  id : int;
  pos : pos;
  name : string;
  conv : param_conv;
  ty : field_ty; (* declared type; Task 6 resolves it for real *)
}

(* `= now()` is recognized explicitly (mirrors rt's DefaultExpr::Now).
   Everything else is kept as its raw token span rather than eagerly
   turned into a string (rt's DefaultExpr::Opaque(String) precedent) —
   "opaque token span" per the Task 4 brief, so a later stage could in
   principle re-lex/interpret it without having thrown information
   away. Task 4 itself never inspects the contents. *)
type default_expr =
  | DefaultNow
  | DefaultOpaque of Token.t list

type field = {
  id : int;
  pos : pos;
  name : string;
  ty : field_ty;
  default : default_expr option;
  (* Annotation *names* only (e.g. ["unique"]) — the brief: "names
     recorded, unknown names fine at parse level". Any `(...)` argument
     list on a field annotation is consumed and discarded, matching
     rt's own field-annotation handling; nothing downstream needs those
     arguments in Task 4. *)
  annotations : string list;
  (* haxe-parity Task 7: `pub(read) name: T` — the field's value is
     readable from outside the declaring class, but writable only from
     inside it (Haxe's `(default, null)` property pattern). Reads need no
     check at all; the write side is types.ml's, at every assignment
     whose target is a field of another class's instance. *)
  pub_read : bool;
}

(* ---- expressions (Task 5) ------------------------------------------

   `unop`/`binop` name their operators the way rt's ast.rs BinOp/UnOp
   does for the operators this grammar shares with it (Add/Sub/Mul/
   Div/Mod, Eq/Ne/Lt/Le/Gt/Ge). Two deliberate differences from rt: no
   `And`/`Or` (this grammar's lexer, Task 3, has no `&&`/`||` tokens —
   there is no boolean-logic sublanguage here) and one new operator,
   `Concat`.

   `Concat` (source syntax `..`, `Token.DotDot`) is this task's own
   design decision, not a straight rt port: the brief's precedence
   ladder lists "text concatenation" as its own tier, distinct from
   arithmetic, and the VM design spec's instruction table
   (docs/superpowers/specs/2026-08-01-oop-compiler-vm-design.md §5)
   lists a dedicated `CONCAT` op alongside (not folded into) `ADD` —
   so the source grammar needs its own operator token to compile down
   to that, not an overload of `+` disambiguated by operand type later.
   `Token.DotDot` is lexed (Task 3) but was never given a grammar rule
   before now, so this claims it. Flagged as an inference, not a
   spec-literal instruction, because no fixture upstream of this task
   spells out the token; it is the only unclaimed binary-shaped token
   left, and Lua's `..` is the same design (concat binds looser than
   `+`/`-`, tighter than comparison — this ladder's ordering). *)
type unop = Neg

(* `And`/`Or` (haxe-parity Task 2): real keywords, spelled as words, not
   `&&`/`||` — the spec amendment's own wording. `Bool`-typed operands
   only (types.ml wires this through, no truthiness); short-circuit,
   lowered to compare-and-jump on the existing JZ/JMP opcodes (emit.ml),
   no new opcode. Own precedence level, looser than every comparison —
   see parser.ml's ladder doc for the exact ordering (`or` loosest, then
   `and`, then comparison). *)
type binop =
  | Add
  | Sub
  | Mul
  | Div
  | Mod
  | Concat
  | Eq
  | Ne
  | Lt
  | Le
  | Gt
  | Ge
  | And
  | Or

type expr = {
  id : int;
  pos : pos;
  kind : expr_kind;
}

(* `Call`'s callee is a general `expr`, not a name: a free call has an
   `Ident` callee, a method call (interface-typed or not — dispatch is
   Task 6's job, not the parser's) has a `Field` callee. Brief: "free,
   method, and interface-typed method calls share one call node" — this
   is that sharing; the parser never distinguishes the three, it only
   ever builds `Call (callee, args)`.

   `Ctor` (`ClassName { field: expr, ... }`) is recognized in primary-
   expression position by the two-token shape identifier-then-brace
   (parser.ml's `looks_like_ctor`) — milestone-1 has no `new` keyword,
   this brace literal is the only construction syntax.

   `DbStub` is the SQL sublanguage's one opaque node (`insert`/`select`,
   lowercase-Ident or uppercase-keyword form alike): its token list is
   never re-parsed as this grammar, only captured verbatim, spec
   section 3's "parses but traps" contract. Lowercase `insert` is a
   *statement*-only trigger (parser.ml's `is_insert_trigger`, checked
   before general expression parsing); lowercase `select` is legal in
   general expression position too (`is_select_trigger`, reachable from
   `parse_primary`) — e.g. `let rows = select ...` — matching the brief:
   "statement-position lowercase insert/select (and expression-position
   select)". *)
and expr_kind =
  | IntLit of int
  | StrLit of string
  | BoolLit of bool
  (* haxe-parity Task 6: `nil`, the absent value of a `?T`. One
     representation for every T: the zero word — "a nullable field stores
     exactly what T stores and spells nil as 0", docs/plan/oop-vm/
     08-builtin-surface.md. Nothing to allocate, nothing to unbox, and
     every per-kind drop plan already ignores a zero slot. *)
  | NilLit
  | Ident of string
  | Field of expr * string
  | Index of expr * expr
  | Call of expr * expr list
  | Unary of unop * expr
  | Binary of binop * expr * expr
  | Ctor of string * (string * expr) list
  | DbStub of Token.t list
  (* `insert Class { field: expr, ... }` — the FIRST DB statement to leave
     the stub behind (iteration 9, Task 3). Typed like a constructor
     literal, returns the new row's id (Int), legal in statement and
     expression position both. `select` stays a DbStub until Task 5. *)
  | Insert of string * (string * expr) list
  (* haxe-parity Task 2: one `${expr}` interpolation site, produced only
     by the string-interpolation desugar (parser.ml) — never written
     directly by a parse rule the way every other expr_kind is. Its
     *textification* (pass through if already Text, `int_to_text` if
     Int, a diagnostic for anything else) is a type-directed decision
     deferred to emit.ml, since the parser has no type information yet;
     "desugars at parse time to concatenation" covers the chain SHAPE
     (a `Binary(Concat, ...)` of StrLit/Interp segments), not this one
     leaf's textification. *)
  | Interp of expr
  (* haxe-parity Task 3: `switch subject { case v1, v2: <stmts> ...
     default: <stmts> }`. One construct for both positions (the brief's
     own words: "statement position is the expression with a discarded
     value") — `stmt` has no separate switch node; a bare `switch {...}`
     statement is simply this same node wrapped in `ExprStmt`, exactly
     like a bare `select ...` call already is. Arms carry a `stmt list`
     body (not a single `expr`) because the sample's own sites do —
     `case "tail_log": if args == nil { return err(...); } ... return
     self.tools.tail_log(...);` is not reducible to one expression — so
     `expr`/`stmt` must be mutually recursive from here down (this is
     the one place `expr_kind` reaches into `stmt`; every other node
     above predates this task and never needed to). `values = []` means
     `default` (`is_default = true`); a `case` always has at least one
     value, and — the sample's own `alias_of`/cron.wo shape,
     `case "@daily", "@midnight": ...` — may have more than one,
     matching on any of them. No guards, no ranges: the sample never
     uses either, so neither is grammar here (YAGNI, recorded in the
     task report). *)
  | Switch of expr * switch_arm list
  (* Container literals, the driving workload's own spelling for a fresh
     container: `[]` / `[a, b, c]` for a `multi T`, `{}` for an empty
     `map<K, V>`. They lower to exactly what `multi_new()`/`map_new()`
     already lower to (the element kinds come from the destination's
     declared type — docs/plan/oop-vm/08-builtin-surface.md's
     "a fresh container needs a destination of declared type"), plus one
     `push` per element for a non-empty list. A literal with no typed
     destination is WO-E403, the same as a bare `let m = map_new()`.
     Non-empty map literals are not grammar: the workload has none, and
     `{ k: v }` in expression position cannot be told from a constructor
     literal without lookahead nothing else needs. *)
  | ListLit of expr list
  | MapLit
  (* `expr as Type` — a CHECKED conversion, not a reinterpretation: its only
     meaning in this language is "decode this JSON text into that type",
     yielding `?Type` (nil when the text does not fit). Anything else is a
     WO-E403 at emission: there is no reinterpret-cast in the doctrine (the
     systems-track spec's reject table lists `cast`), and this form exists
     only because a decode's result type cannot be inferred. *)
  | As of expr * field_ty
  (* haxe-parity Task 5: `try body catch (ename) handler` — an expression,
     like `switch`. `body` is an expression (the workload's only form);
     `handler` is a `stmt list` so both arm spellings share one shape,
     exactly as a switch arm does: `catch (e) nil` parses as a single
     ExprStmt, `catch (e) { ... }` as its statements, and the arm's value
     is its trailing ExprStmt (an arm with no trailing expression yields
     nothing, which is legal in statement position). The error record the
     handler binds is the structured trap error {code, line, method, msg}
     — the `Error` record type, predeclared by types.ml. *)
  | Try of {
      body : expr;
      ename : string;
      handler : stmt list;
    }
  (* iteration 9b: a language-integrated query. `from <var> in <source>
     where <e>* [group <e> by <k> into <g>] [order by <e> [desc]] [take <e>]
     select <e>` — lowered to a bytecode loop over engine cursor builtins,
     never SQL text. A table-class value is its row id at runtime, so field
     access on a range variable reads through the engine. Slice scope today:
     from/where/order/take/select and group-by aggregation; join is later. *)
  | Query of query

and query_source =
  | QTable of string (* a table class by name: `from e in Employee` *)
  | QNav of expr (* a backlink/multi navigation: `from s in d.staff` *)

and query = {
  q_var : string;
  q_src : query_source;
  q_wheres : expr list;
  (* group <key_expr> by ... into <gvar>: present iff this is an aggregating
     query. q_group_key is the whole grouped element (`e`), q_group_by the
     key, q_gvar the group binding whose `.f` columns feed aggregates. *)
  q_group : (string * expr) option; (* (gvar, key_expr) *)
  q_order : (expr * bool) option;    (* (key, desc?) *)
  q_take : expr option;
  q_select : expr;
  q_pos : pos;
}

(* ---- statements (Task 5) ---------------------------------------------

   Statement nodes use `s_id`/`s_pos`/`s_kind` rather than `id`/`pos`/
   `kind` (which `expr` already claims): both records would otherwise
   share the exact same field set, and OCaml's type-directed field
   disambiguation needs at least one label difference to tell a bare
   `{ id; pos; kind = ... }` literal apart from the other type — every
   other id/pos reuse in this file (param/field/method_sig/...) is safe
   because each of those already has a distinct full field set.

   `If.else_body` pairs the `else` keyword's own position with its
   block so dump.ml has something to print an "ELSE" line's LINE:COL
   from — every other dumped line in this codebase starts with a real
   position (Task 4 convention), and there is no other node to hang
   that position on. `else if ...` is desugared here at parse time into
   `else_body = Some (else_pos, [ <nested If stmt> ])` — a one-statement
   else-block whose sole statement is itself an `If` — rather than a
   third `else_body` shape, so dump.ml's block-rendering code (already
   written once, for `then_body`) renders the chain for free. *)
and stmt = {
  s_id : int;
  s_pos : pos;
  s_kind : stmt_kind;
}

and stmt_kind =
  | Let of {
      name : string;
      (* The full annotation grammar, not just a bare name: the driving
         workload writes `let rest: multi Text = []`, `let headers:
         map<Text, Text> = {}` and `let port: ?Int = nil`, all of which
         parse_field_ty already understood for fields and parameters. *)
      ty : field_ty option;
      value : expr;
    }
  | Assign of {
      target : expr;
      value : expr;
    }
  | If of {
      cond : expr;
      then_body : stmt list;
      else_body : (pos * stmt list) option;
    }
  | While of {
      cond : expr;
      body : stmt list;
    }
  | For of {
      var : string;
      (* `for k, v in m` over a `map<K, V>`: the second name binds the value
         for that key. None is the one-name form, over a `multi`. Map
         enumeration is slot-ordered (runtime/src/cont.h's parallel arrays),
         which is insertion order. *)
      var2 : string option;
      iter : expr;
      body : stmt list;
    }
  | Return of expr option
  | ExprStmt of expr
  (* haxe-parity Task 2: loop control. Both reuse owner.ml's scope-end
     drop machinery (see Owner.DBreak/DContinue) so an owned value still
     alive in the loop body is dropped at the jump, not left to leak;
     emit.ml refuses to lower either one outside a loop (WO-E403 —
     "cannot lower", the same convention as every other construct with
     no legal target, since nothing upstream tracks loop nesting as a
     parse- or type-error). *)
  | Break
  | Continue
  (* `do { body } while cond` — body runs at least once, then the
     condition gates repeating it. Lowered onto the same JZ/JMP pair
     `while`/`for` already use, just reordered (parser.ml/emit.ml). *)
  | DoWhile of {
      body : stmt list;
      cond : expr;
    }

(* haxe-parity Task 3: one arm of a `switch`. `values = []` iff
   `is_default`; a `case` arm's `values` is never empty (parser
   contract, mirrored — not re-checked — by every later stage). No
   per-arm `id`: nothing downstream keys a side table on "this specific
   arm" independent of the `Switch` expr that owns it (the shared
   `Switch.id` is the drop-scope/branch-join node for every arm, one
   per-arm string label telling them apart — exactly how `If`'s THEN/
   ELSE already share `s_id` and differ only by label). *)
and switch_arm = {
  arm_pos : pos;
  values : expr list;
  is_default : bool;
  body : stmt list;
}

(* haxe-parity Task 3 (review fix, Critical 1): the arm order a
   switch's own lowering actually walks — `default` moved to the end,
   regardless of where it sits in the source. `default` has no
   comparison of its own (it matches unconditionally); lowering the
   arms in raw *source* order therefore made any `case` arm written
   after a `default` permanently unreachable dead code (nothing ever
   jumps into it, and `default`'s own body jumps straight to the
   switch's exit, never falling through) — a real, reviewer-reproduced
   bug, not a theoretical one. Both `owner.ml` (`analyze_switch`,
   whose drop-scope/JOIN-DROP tables are keyed "ARM<i>" by this order)
   and `emit.ml` (`emit_switch`, the compare-and-jump chain itself)
   call this SAME function rather than each re-deriving the reorder
   independently — the two-file fix the review flagged, done once so
   the "ARM<i>" indices the two files hand each other can never drift
   apart. `List.partition` is stable (documented in the stdlib): every
   `case` arm keeps its own relative order, and — malformed, not
   otherwise rejected — more than one `default` would too, all pushed
   after every `case`. *)
let switch_lowering_order (arms : switch_arm list) : switch_arm list =
  let cases, defaults = List.partition (fun (a : switch_arm) -> not a.is_default) arms in
  cases @ defaults

(* haxe-parity Task 2: `const NAME = <literal>` — a compile-time value,
   substituted for every unshadowed `Ident NAME` reference by a
   dedicated post-parse pass (parser.ml's own const-substitution step,
   run at the end of `parse`) rather than threaded through
   typecheck/owner/emit as a new resolvable name: after substitution a
   const reference simply *is* the literal expr it names, so every later
   stage needs zero const-specific code. `value` is restricted by the
   parser to a literal (`IntLit`/`StrLit`/`BoolLit`, optionally
   `Unary(Neg, IntLit)`) — never a general expression, matching the
   brief's own "= literal", not "= expr". *)
type const_decl = {
  id : int;
  pos : pos;
  name : string;
  value : expr;
}

(* A signature shared shape (name/params/ret) appears twice: as an
   interface method (no body) and as a class/type/free-fn method (body
   captured as a span). Kept as two separate flat records rather than
   one record nesting the other — avoids an awkward `sig` field name
   (`sig` is an OCaml keyword) and keeps `m.name`/`m.params` uniform
   instead of `m.msig.name`. *)
type method_sig = {
  id : int;
  pos : pos;
  name : string;
  params : param list;
  ret : field_ty option;
}

type method_decl = {
  id : int;
  pos : pos;
  name : string;
  params : param list;
  ret : field_ty option;
  (* Task 4 captured this as a verbatim token span (brace-depth counter
     only); Task 5 parses it for real. *)
  body : stmt list;
  (* haxe-parity Task 1 (modules): true only for a top-level free `fn`
     parsed with a leading `pub` marker. method_decl is shared with
     class methods (Task 4's own design — see this file's module doc),
     but `pub` is a Task-1-scoped, top-level-declaration-only marker
     (classes, interfaces, free fns); method-level visibility is a
     different, not-yet-designed question, so parse_method always
     passes `pub = false` for a class body's own methods — this field
     is meaningful only when the surrounding decl is `Fn`. *)
  pub : bool;
  (* haxe-parity Task 7: `static fn` on a class — no instance, no `self`,
     called as `Flock.held(path)`. Lowered as an ordinary method record
     with no receiver slot (emit.ml), so its `arg_cnt` counts parameters
     only, and it can never satisfy an interface method (nothing to
     dispatch on). Always false for a free `fn` and for an interface
     signature. *)
  is_static : bool;
}

(* `@table(name: "...", index: [a, b], index: [c])` — optional storage
   configuration, ported from rt's TableCfg. `name`/`index` are the only
   known keys; anything else inside `@table(...)` is a parse error
   (WO-E1xx), not a silent skip — unlike an unrecognized annotation
   *name*, which does skip silently (rt convention, see parser.ml). *)
type table_cfg = {
  table_name : string option;
  indexes : string list list;
}

type class_decl = {
  id : int;
  pos : pos;
  name : string;
  (* true for `class`, false for `type` — Task 4 brief: identical field
     grammar either way; `fn` methods parse for real inside both (a
     deliberate divergence from rt's plan-13 asymmetry, where a plain
     `type`'s `fn` was skip-discarded — see parser.ml's module doc). *)
  is_class : bool;
  (* haxe-parity Task 4: true for `typedef Name = { ... }` — a
     STRUCTURAL record alias, reusing this same node (same field
     grammar, same downstream ctor/field machinery) rather than a
     parallel decl kind. What the flag changes downstream: two records
     with the same shape are the SAME type (emit.ml dedups them onto one
     class-table entry; types.ml's arm unification compares shapes, not
     names). A record body is fields only — the parser never puts a
     method or const inside one, so `methods`/`consts` are always []
     here. `is_class` is false whenever this is true. *)
  is_record : bool;
  is_gc : bool; (* @gc — reference semantics, spec section 3/4 *)
  table : table_cfg option; (* @table(...) — absent unless annotated *)
  fields : field list;
  methods : method_decl list;
  (* haxe-parity Task 2: class-level `const NAME = literal` (bare, no
     `static` — `static const` is Task 7's syntax, deliberately not
     handled here so it falls through to a clean parse error, counted
     against the gap until Task 7 lands). Scoped to this class's own
     methods only by the same post-parse substitution pass that handles
     top-level consts — see const_decl's own doc comment. *)
  consts : const_decl list;
  (* haxe-parity Task 1 (modules): `pub` marker — false (private to the
     declaring module) unless the declaration was written `pub class`/
     `pub type`. *)
  pub : bool;
}

type interface_decl = {
  id : int;
  pos : pos;
  name : string;
  methods : method_sig list; (* signatures only — no fields, no bodies *)
  pub : bool; (* haxe-parity Task 1 — see class_decl.pub *)
}

(* haxe-parity Task 1 (modules): `use fs` (a reserved stdlib namespace)
   or `use shared/util` (project-relative, slash-separated path
   segments naming another discovered module's directory). `segments`
   is never empty — the parser requires at least one identifier. *)
type use_decl = {
  id : int;
  pos : pos;
  segments : string list;
}

(* haxe-parity Task 4: one variant of a union declaration
   (`type Name = A | B | C(field: Type, ...)`). `v_fields` is the
   payload, in declaration order — [] for a bare variant. No per-variant
   `id`: like switch_arm, nothing downstream keys a side table on "this
   specific variant" independent of the union that owns it (a variant's
   identity downstream is (union, ordinal) — its tag). *)
type variant_decl = {
  v_pos : pos;
  v_name : string;
  v_fields : (string * field_ty) list;
}

(* haxe-parity Task 4: `type Name = V1 | V2 | ...` — a tagged union.
   All-bare unions (every `v_fields` empty) lower to plain integer tags
   (the variant's ordinal), no heap object and no class-table entry;
   a union with at least one payload variant lowers every variant to a
   small heap object whose class-table entry the compiler generates
   (docs/plan/oop-vm/00-wob-format.md, "enum payload variants"). *)
type union_decl = {
  id : int;
  pos : pos;
  name : string;
  variants : variant_decl list;
  pub : bool;
}

type decl =
  | Class of class_decl
  | Interface of interface_decl
  | Fn of method_decl (* free (non-method) top-level function *)
  | Use of use_decl
  | Const of const_decl (* haxe-parity Task 2: top-level `const NAME = literal` *)
  | Union of union_decl (* haxe-parity Task 4: `type Name = A | B | ...` *)

type program = { decls : decl list }
