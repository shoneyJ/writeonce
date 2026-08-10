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
  | Ident of string
  | Field of expr * string
  | Index of expr * expr
  | Call of expr * expr list
  | Unary of unop * expr
  | Binary of binop * expr * expr
  | Ctor of string * (string * expr) list
  | DbStub of Token.t list

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
type stmt = {
  s_id : int;
  s_pos : pos;
  s_kind : stmt_kind;
}

and stmt_kind =
  | Let of {
      name : string;
      ty : string option;
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
      iter : expr;
      body : stmt list;
    }
  | Return of expr option
  | ExprStmt of expr

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
  is_gc : bool; (* @gc — reference semantics, spec section 3/4 *)
  table : table_cfg option; (* @table(...) — absent unless annotated *)
  fields : field list;
  methods : method_decl list;
}

type interface_decl = {
  id : int;
  pos : pos;
  name : string;
  methods : method_sig list; (* signatures only — no fields, no bodies *)
}

type decl =
  | Class of class_decl
  | Interface of interface_decl
  | Fn of method_decl (* free (non-method) top-level function *)

type program = { decls : decl list }
