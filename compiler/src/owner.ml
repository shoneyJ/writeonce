(* owner.ml — the ownership pass (mutable value semantics), Task 7.

   This is the novel core of the design (spec section 3 rules 1-6 and
   section 4's "borrow enforcement split"). One forward dataflow walk per
   function body decides two things at once:

     1. the four MVS *rules*, each reported as a two-site diagnostic
        (WO-E301..WO-E304 — the codes are documented at their
        definitions below);
     2. the four *tables* plan 3's emitter consumes — moves, drops,
        rc sites, residual sites — rendered by Dump.dump_owner and
        pinned by goldens under compiler/test/golden/owner/.

   ---- what a "borrow" is in this grammar --------------------------------

   There is no borrow *expression* syntax: `mut`/`take` are parameter
   conventions only (parser.ml's parse_param). So a borrow is created in
   exactly four ways, and every one of them is second-class (spec: "a
   borrow cannot escape its scope"):

     - a parameter declared without `take` (`p: T` shared, `mut p: T`
       exclusive) — a borrow of the *caller's* value, live for the whole
       body. Its source place is unknown here; the caller's own call site
       is where aliasing between two such borrows is checked.
     - `self` — always a borrow (spec rule 6: shared if the body only
       reads, exclusive if it writes; inferred, see method_writes_self).
     - `let r = <owned place with a projection>` (`let r = b.inner`) —
       milestone 1 has no syntax for moving a value *out of* a field or
       element, so this binds a borrow of that place, not a partial move.
       Consequence: `return r` is a borrow escape, and moving the place
       out from under `r` is a use-while-borrowed.
     - `for it in items` — `it` borrows an element of `items`, and the
       borrow is live for the whole loop body.

   Argument passing creates borrows too, but only for the duration of the
   call itself; that is the "region" of a call expression, and all of a
   call's argument borrows plus its receiver borrow are live
   simultaneously. Conflicts inside one region are checked pairwise.

   ---- states, joins, loops ---------------------------------------------

   Per owned local: Live | Moved <site> | Borrowed <site> (the brief's
   three states). Join at an if/else merge takes Moved over Live (a value
   moved on one branch is treated as moved after the merge — the standard
   conservative rule; a diverging branch, i.e. one that returned, is
   dropped from the join instead of poisoning it). Loops are analyzed to
   a fixpoint the cheap way the brief prescribes: run the body once with
   diagnostics and table recording *off* (ctx.recording = false), join
   that result with the loop's entry state, then run it again for real.
   Two passes are enough because the lattice is three states tall and the
   only transition the body can make is Live -> Moved; the second pass
   therefore sees every loop-carried move (that is what catches
   `while c { consume(take b) }`).

   ---- what is exempt ---------------------------------------------------

   `@gc` values (Types.is_gc_class) are exempt from every rule — aliasing
   them is legal by design (spec rule 5); they appear only in the rc
   table. Builtin scalars and `ref T` ids are copied, never moved, and
   are exempt too. `?T` follows T exactly
   (nil is just a value; null-narrowing does not change ownership state),
   so oclass_of unwraps Nullable first. An unknown type name is treated
   as Copy: types.ml already reported WO-E225 for it and a cascade of
   ownership errors on top would be noise.

   ---- deliberate milestone-1 simplifications ---------------------------

   Each of these is a place where a *later* milestone adds precision, not
   a place where this pass is wrong-by-accident:

     - No partial moves. A move site must name a whole local (`x`), never
       a projection (`x.f`, `x[0]`); see the `let` case above.
     - Alias provability is syntactic *after canonicalization*: a place
       written through a borrow binding is first rewritten to the storage
       that borrow names (see canon), then two places overlap only if they
       share a root and every projection pair matches. Distinct roots are
       assumed disjoint — sound because a borrow can never be stored, so
       two distinct locals can only alias through parameters, and that
       aliasing is checked at the caller's call site. Canonicalization is
       what makes `swap(r, s)` after `let r = bag.items[i]; let s =
       bag.items[k]` behave identically to the direct
       `swap(bag.items[i], bag.items[k])`.
     - Two element accesses through *runtime* indices are neither proven
       equal nor proven distinct: they become residual sites, which is
       exactly the case runtime/src/borrow.c exists for.
     - Self-mutability (spec rule 6) is inferred from direct assignments
       to `self.…` in the method body only; a method that mutates self
       *indirectly*, by calling another mutating method on self, is read
       as read-only here. Types.method_info carries a `mutates` field but
       it is hardcoded `false` (types.ml never infers it), so this pass
       computes its own rather than trusting it.
     - Reassigning a live owned local (`x = other`) drops the overwritten
       value; that DROP is recorded in the drops table as OVERWRITE. The
       brief names the table "scope-end drop sets", but an overwritten
       value dies just as deterministically as one falling out of scope,
       and leaving it out would leak in plan 3.
     - A value moved on only one branch of an `if` is normalized rather
       than tracked per path: the branch that kept it drops it at its own
       end (JOIN-DROP), so the joined Moved state is true on both paths and
       nothing leaks. The cost is that the value dies earlier on that path
       than a drop-flag implementation would allow. Two drop caveats
       survive that fix, both deliberate: a `while`/`for` body that moves
       an outer local is an error anyway (the fixpoint's second pass
       reports WO-E301), so no normalization is attempted there; and a
       value whose *type* is unknown to the pass — the result of an
       unresolved builtin call — classifies as Copy and so is never
       dropped at all, which stays true until builtin signatures land. *)

open Ast

(* ============================================================
   Diagnostics (WO-E3xx). Task 8 catalogs these.
   ============================================================ *)

(* WO-E301 — a local is read (or moved again) after its value was moved
   away. Primary site: the use. Related site: the move. *)
let use_after_move_code = Diag.ownership_prefix ^ "01"

(* WO-E302 — a value is moved while a live borrow of it (or of something
   it contains) still exists. Primary site: the move. Related: the
   borrow. *)
let move_while_borrowed_code = Diag.ownership_prefix ^ "02"

(* WO-E303 — conflicting borrows of one place where the analysis can
   prove the two accesses alias, and at least one is exclusive (`mut`).
   Primary site: the second borrow. Related: the first. *)
let conflicting_borrow_code = Diag.ownership_prefix ^ "03"

(* WO-E304 — a borrow escapes its scope: returned, stored into a field,
   or moved out to a `take` parameter (spec rule 3). Primary site: the
   escape. Related: where the borrow was created. *)
let borrow_escape_code = Diag.ownership_prefix ^ "04"

(* ============================================================
   Ownership classes and places
   ============================================================ *)

(* Copy   — scalars, `ref T`, unknown types: copied,
             never moved, no borrow rules, no drops.
   Owned  — a non-@gc class/type instance, `multi T`, `map<K,V>`: one
             owner, moves, borrow-checked, dropped at scope end.
   Gc     — a @gc class instance: freely aliased, refcounted. *)
type oclass =
  | Copy
  | Owned
  | Gc

(* A projection step. An index is split by what the analysis can prove
   about it: a literal (PConst) is comparable with other literals; a
   runtime index (PDyn) carries a *rendering* of the index expression, so
   two syntactically identical runtime indices compare equal (same
   element) and two different ones compare unknown (residual site). See
   idx_text for why the fallback rendering is node-unique. *)
type proj =
  | PField of string
  | PConst of int
  | PDyn of string

(* root is a local name ("self" included). ppos is the *root token's*
   position — a caret under `bag` reads better than one under the `.` or
   `[` that Ast.Field/Ast.Index carry. pnode is the outermost expression
   node's id: the emitter's key for this site. *)
type place = {
  root : string;
  projs : proj list;
  ppos : Ast.pos;
  pnode : int;
}

let proj_text = function
  | PField f -> "." ^ f
  | PConst n -> "[" ^ string_of_int n ^ "]"
  | PDyn s -> "[" ^ s ^ "]"

let place_text (p : place) : string =
  p.root ^ String.concat "" (List.map proj_text p.projs)

(* How two places relate. Overlap = provably the same storage, or one
   contains the other (a prefix). Disjoint = provably different storage.
   Unknown = the analysis cannot decide; the VM must (residual site). *)
type rel =
  | Overlap
  | Disjoint
  | Unknown

let rec relate_projs (a : proj list) (b : proj list) : rel =
  match (a, b) with
  | [], _ | _, [] -> Overlap (* a prefix contains the longer path *)
  | PField x :: ta, PField y :: tb -> if x = y then relate_projs ta tb else Disjoint
  | PConst i :: ta, PConst j :: tb -> if i = j then relate_projs ta tb else Disjoint
  | PDyn x :: ta, PDyn y :: tb -> if x = y then relate_projs ta tb else Unknown
  | PConst _ :: _, PDyn _ :: _ | PDyn _ :: _, PConst _ :: _ -> Unknown
  | _ -> Unknown (* field vs index: ill-typed, stay conservative *)

let relate (a : place) (b : place) : rel =
  if a.root <> b.root then Disjoint else relate_projs a.projs b.projs

(* ============================================================
   The four tables (the plan-3 emitter contract)
   ============================================================ *)

(* Which MOVE is a real ownership transfer. Copy-classed assignments,
   returns and arguments are *not* here: for those the emitter's MOVE is
   a plain register copy. @gc transfers are not here either — they are rc
   sites. *)
type move_kind =
  | MvLet
  | MvAssign
  | MvReturn
  | MvArg of string (* callee parameter name *)
  | MvCtorField of string (* field being initialized *)

type move_site = {
  mv_node : int;
  mv_pos : Ast.pos;
  mv_place : string;
  mv_kind : move_kind;
}

(* An entry in a drop set / live mask. Owned values need DROP; @gc
   handles need a decrement, so masks tag them. *)
type local_kind =
  | LOwned
  | LGc

(* One name in a drop set / live mask. di_node is the *declaring* node id
   (a param's id, a `let`'s statement id, a `for`'s statement id, the
   method id for `self`) — carried because a name alone is ambiguous under
   shadowing: two nested scopes may each bind `x`, and the emitter must
   know which register it is being told to destroy. For an OVERWRITE entry
   there is no declaring node: di_name is the assigned place's text and
   di_node is that place expression's own node. *)
type drop_item = {
  di_name : string;
  di_kind : local_kind;
  di_node : int;
}

(* DScope  — every owned local of a scope that is still live where the
              scope ends (label names which block: BODY/THEN/ELSE/
              WHILE/FOR). Suppressed when the scope diverges (every path
              returned), because that scope end is unreachable.
   DReturn — every live owned local in *all* enclosing scopes at an
              early (or final) return, after the returned value's own
              move: the DROPs that must run before the frame leaves.
   DBreak/DContinue (haxe-parity Task 2) — every live owned local in
              every scope from here up to *and including* the nearest
              enclosing loop's own body scope (WHILE/FOR/DO — never
              beyond it, since a break/continue only exits the loop, not
              the function). Same "reuse DReturn's own machinery" shape,
              bounded to the loop instead of the whole function — see
              live_holders_upto.
   DOverwrite — the value an assignment overwrites (see the module doc).
   DBranchJoin — join normalization: the locals the *other* branch of an if
              moved and this one did not, dropped at this branch's end so
              both paths leave the merge in the same state (see
              branch_join_drops and the module doc's simplifications list).
              The label names the branch whose end they belong to; an `if`
              with no `else` anchors its implicit else at the `if` itself.
   DLiveMask  — the frame's drop map at a call / DB_STUB site: everything
              live there, @gc handles included, taken *after* the call's
              own argument transfers because a moved-in value is the
              callee's responsibility, not this frame's. *)
type drop_kind =
  | DScope of string
  | DReturn
  | DOverwrite
  | DBranchJoin of string
  | DLiveMask
  | DBreak
  | DContinue

type drop_site = {
  dr_node : int;
  dr_pos : Ast.pos;
  dr_kind : drop_kind;
  dr_items : drop_item list;
}

(* iteration 7b: the rc-site machinery (acquire/release pairs, elision
   groups, the clobber rule) is gone with reference counting itself — a
   traced value's aliases need no bookkeeping, tracing owns the lifetime. *)

type acc_kind =
  | AShared
  | AExcl
  | AMove

(* Where static proof failed: the emitter wraps the region in
   BORROW_S/BORROW_X + RELEASE (runtime/src/borrow.c) and the VM traps on
   a real violation. *)
type residual_site = {
  rs_node : int;
  rs_pos : Ast.pos;
  (* each side: its rendered place, its role, and the node of the
     expression the place came from — what the emitter needs to know
     *which* operand to wrap in BORROW_S/BORROW_X *)
  rs_a : string;
  rs_a_kind : acc_kind;
  rs_a_node : int;
  rs_b : string;
  rs_b_kind : acc_kind;
  rs_b_node : int;
}

type tables = {
  moves : move_site list;
  drops : drop_site list;
  residuals : residual_site list;
}

(* ============================================================
   Flow state
   ============================================================ *)

type state =
  | Live
  | Moved of Ast.pos
  | Borrowed of Ast.pos

type local = {
  l_name : string;
  l_ty : Ast.field_ty;
  l_class : oclass;
  l_node : int; (* declaring node: param id / let stmt id / for stmt id / method id for self *)
  l_pos : Ast.pos;
  (* true when this frame holds the value (owned) or a counted handle
     (@gc) and must destroy it at scope end. False for borrows and for
     Copy-classed locals. *)
  l_holds : bool;
  (* for a borrow bound to a place in *this* frame (`let r = b.inner`,
     `for it in items`), the place borrowed; None for parameter borrows,
     whose source lives in the caller. Also set for a @gc alias, where it
     names the place the extra reference came from (rc elision needs it).
     This field is also what makes a place *canonical* — see canon. *)
  l_src : place option;
  (* only meaningful for a borrow: whether it is shared or exclusive. A
     `mut` parameter is an exclusive borrow; every borrow a `let` or a
     `for` can bind is shared, because the grammar has no `let mut`. Read
     when naming the *other* side of a residual site, so that side's role
     comes from data rather than from a literal at the reporting site. *)
  l_bkind : acc_kind;
  mutable l_state : state;
}

type scope = {
  sc_node : int;
  sc_pos : Ast.pos;
  sc_label : string;
  mutable sc_locals : local list; (* reverse declaration order = destruction order *)
}

(* Table accumulators, shared across all functions of a program. *)
type sink = {
  mutable s_moves : move_site list;
  mutable s_drops : drop_site list;
  mutable s_res : residual_site list;
}

type ctx = {
  file : string;
  syms : Types.symbols;
  coll : Diag.Collector.t;
  sink : sink;
  fn_name : string;
  mutable scopes : scope list; (* innermost first *)
  (* haxe-parity Task 2: the enclosing While/For/DoWhile statement ids,
     innermost first — the nearest enclosing loop's own `s.s_id`, which
     is also its own scope's `sc_node` (analyze_block ~node:s.s_id).
     Empty outside any loop; a break/continue found there records no
     drop site at all (nothing to bound the drop to) and leaves the
     actual rejection to emit.ml, which has no jump target to lower it
     onto — the same WO-E403 "no legal target" convention as every
     other construct with nothing to lower to. *)
  mutable loop_stack : int list;
  (* false during a loop's probe pass: no diagnostics, no table entries *)
  mutable recording : bool;
  (* set when the current path has returned; a diverged path contributes
     no scope-end drops and drops out of if/else joins *)
  mutable diverged : bool;
  (* iteration 7b demand-promotion (Gcinfer): when Some, the pass runs in
     collect mode — a class value that would fail the escape rule records its
     class here instead of raising WO-E304, so inference can promote it to
     traced. None = normal report mode. *)
  promote : (string -> unit) option;
}

(* ============================================================
   Type/class resolution (reuses Types' symbol tables; derives nothing
   types.ml already exports)
   ============================================================ *)

let rec unwrap_nullable (ft : Ast.field_ty) : Ast.field_ty =
  match ft with Nullable inner -> unwrap_nullable inner | t -> t

let oclass_of (ctx : ctx) (ft : Ast.field_ty) : oclass =
  match unwrap_nullable ft with
  (* `Text` is a HEAP value (runtime/src/obj.h's wo_str), so a binding that
     holds a fresh one owns it and must drop it at scope end. It was Copy
     until 2026-08-14 — grouped with Int/Bool because types.ml calls it a
     builtin scalar — and the consequence was that no Text local was ever
     dropped: every concatenation, interpolation and stdlib read accumulated
     in the arena for the life of the process. Invisible in the corpus (small
     strings live in the arena, which is freed wholesale at exit) and fatal in
     a daemon (the workload's supervisor leaked a 1 MiB `fs.read_all` result
     per rescan, which is a plain malloc and so ASan-visible). A Text read out
     of a PLACE is still a borrow — analyze_let's own place logic decides
     that, exactly as it does for a record field. *)
  (* iteration 19: Bytes joins Text here — see Types.is_heap_scalar *)
  | Scalar n when Types.is_heap_scalar n -> Owned
  | Scalar n ->
    if Types.is_builtin_scalar n then Copy
    else if Types.is_gc_class ctx.syms n then Gc
    else if Types.StringMap.mem n ctx.syms.Types.classes then Owned
    else (
      (* haxe-parity Task 4: an all-bare union value is a plain integer
         tag — Copy, exactly like a builtin scalar (moving/aliasing it
         is copying an int). A payload union value is a heap variant
         object — Owned, one owner, dropped at scope end like any other
         non-@gc instance. *)
      match Types.StringMap.find_opt n ctx.syms.Types.unions with
      | Some u -> if u.Types.u_has_payload then Owned else Copy
      | None -> Copy (* unknown type: WO-E225 already reported by types.ml *))
  | Ref _ -> Copy
  | Actor _ -> Copy (* an address is a copyable word; the runtime owns actors *)
  | Backlink _ -> Copy (* a virtual collection of row ids read on demand *)
  | Multi _ | Map _ -> Owned
  | Nullable _ -> Copy (* unreachable: unwrapped above *)

let field_ty_of (ctx : ctx) (class_name : string) (fname : string) : Ast.field_ty option =
  match Types.StringMap.find_opt class_name ctx.syms.Types.classes with
  | None -> None
  | Some (cls : Types.class_info) ->
    let rec find = function
      | [] -> None
      | (n, ft, _, _) :: tl -> if n = fname then Some ft else find tl
    in
    find cls.Types.fields

let elem_ty (ft : Ast.field_ty) : Ast.field_ty option =
  match unwrap_nullable ft with
  | Multi n -> Some (Scalar n)
  | Map (_, v) -> Some (Scalar v)
  | _ -> None

let find_local (ctx : ctx) (name : string) : local option =
  let rec go = function
    | [] -> None
    | sc :: tl -> (
      match List.find_opt (fun l -> l.l_name = name) sc.sc_locals with
      | Some l -> Some l
      | None -> go tl)
  in
  go ctx.scopes

(* Does this method write `self`? Spec rule 6's inference, direct
   assignments only (see the module doc's simplifications). *)
let rec root_name_of (e : Ast.expr) : string option =
  match e.kind with
  | Ident n -> Some n
  | Field (base, _) | Index (base, _) -> root_name_of base
  | _ -> None

let rec body_writes_self (body : Ast.stmt list) : bool =
  List.exists stmt_writes_self body

and stmt_writes_self (s : Ast.stmt) : bool =
  match s.s_kind with
  | Assign { target; _ } -> root_name_of target = Some "self"
  | If { then_body; else_body; _ } ->
    body_writes_self then_body
    || (match else_body with Some (_, b) -> body_writes_self b | None -> false)
  | While { body; _ } | For { body; _ } | DoWhile { body; _ } -> body_writes_self body
  | Let _ | Return _ | ExprStmt _ | Break | Continue -> false

(* A resolved callee: its parameter conventions (positional), its return
   type, and — for a method call — whether the receiver is borrowed
   exclusively. Unresolved callees (a builtin such as `latest`, an
   undeclared name) resolve to None and are then treated as taking every
   argument by shared borrow: the conservative choice for *errors* (never
   invent a move) and the honest one for the tables (no transfer to
   record). *)
type callee = {
  ce_params : (string * Ast.param_conv) list;
  ce_ret : Ast.field_ty option;
  ce_recv_excl : bool;
}

(* haxe-parity Task 4: a bare variant reference (`Pending`) or a variant
   construction (`Failed("x")`) types as its union — locals always win
   first (find_local / resolve_callee run before this), so a shadowing
   binding is never mistaken for a variant. *)
let variant_union_ty (ctx : ctx) (n : string) : Ast.field_ty option =
  match Types.find_variant ctx.syms n with
  | Some (u, _) -> Some (Ast.Scalar u.Types.u_name)
  | None -> None

let rec expr_ty (ctx : ctx) (e : Ast.expr) : Ast.field_ty option =
  match e.kind with
  | IntLit _ -> Some (Scalar "Int")
  | FloatLit _ -> Some (Scalar "Float") (* iteration 19 *)
  | StrLit _ -> Some (Scalar "Text")
  | BoolLit _ -> Some (Scalar "Bool")
  (* A non-empty list literal knows its element type, so an unannotated
     `let names = ["a", "b"]` is still classified Owned and dropped. An
     empty `[]`/`{}` is contextual: only the destination's declared type
     says what it holds, so this stays None and the annotation (or the
     field/parameter it is built into) decides. *)
  | ListLit (first :: _) -> (
    match expr_ty ctx first with Some (Scalar n) -> Some (Multi n) | _ -> None)
  (* arc: a spawn's value is a typed address — a copyable scalar word *)
  | Spawn (cn, _) -> (
    match Types.StringMap.find_opt cn ctx.syms.Types.classes with
    | Some cls -> (
      match
        List.find_opt (fun (m : Types.method_info) -> m.Types.name = "receive") cls.Types.methods
      with
      | Some { Types.params = [ (_, pty, _) ]; _ } -> (
        match pty with Ast.Scalar mname -> Some (Ast.Actor mname) | _ -> None)
      | _ -> None)
    | None -> None)
  | ListLit [] | MapLit -> None
  (* haxe-parity Task 6: `nil` is the zero word — contextual on its
     destination, and never something this frame owns. *)
  | NilLit -> None
  (* a checked decode's value is a fresh instance of the named type (or
     nil) — owned, so the binding that holds it gets its drop *)
  | As (_, ty) -> Some (Nullable ty)
  (* haxe-parity Task 5: a `try` yields its try arm's type (types.ml has
     already required the catch arm to agree). *)
  | Try t -> expr_ty ctx t.body
  | Ident n -> (
    match find_local ctx n with
    | Some l -> Some l.l_ty
    | None -> variant_union_ty ctx n)
  | Field (base, f) -> (
    match expr_ty ctx base with
    | Some bt -> ( match unwrap_nullable bt with Scalar cn -> field_ty_of ctx cn f | _ -> None)
    | None -> None)
  | Index (base, _) -> ( match expr_ty ctx base with Some bt -> elem_ty bt | None -> None)
  | Call (callee, _) -> (
    match resolve_callee ctx callee with
    | Some c -> c.ce_ret
    | None -> (
      (* an unresolved Ident callee may be a variant construction — its
         value is a fresh Owned variant object of the union's type, and
         missing this here is a real leak (analyze_let's None-fallback
         classifies as Copy, so the object would never be dropped). *)
      match callee.kind with
      | Ident n -> variant_union_ty ctx n
      | _ -> None))
  | Unary (_, o) -> expr_ty ctx o
  (* `..` (CONCAT) always produces a FRESH Text, and interpolation desugars to
     exactly such a chain — so this is what gives every interpolated or
     concatenated binding an owner and a drop. Left as None before
     2026-08-14, which made those bindings Copy and leaked every one. *)
  | Binary (Concat, _, _) -> Some (Scalar "Text")
  | Binary _ -> None (* arithmetic/comparison: Copy either way *)
  | Ctor (cn, _) -> Some (Scalar cn)
  | Insert _ -> Some (Scalar "Int") (* the new row's id — Copy, nothing to drop *)
  | Query _ -> Some (Multi "Int") (* a query yields a fresh multi of ids — owned *)
  | Delete _ -> Some (Scalar "Int") (* the deleted id — Copy *)
  | Interp _ -> Some (Scalar "Text") (* an interpolation always produces Text *)
  | DbStub _ -> None
  | Switch (subject, arms) ->
    (* review fix, Critical 3: this was `None` ("not chased", the same
       call as `Binary`/`DbStub` above) — a real, reviewer-reproduced
       leak, not a theoretical gap: `analyze_let`'s own fallback for
       `expr_ty = None` is `Scalar "Int"` (Copy), so an *unannotated*
       `let v = switch ... { case ...: SomeClass{...}; ... }` was
       classified Copy and never dropped, even for a plain class with
       no union involved. Mirrors emit.ml's own `ty_of_expr` Switch
       case exactly (same shape, this file's own `Ast.field_ty`/`ctx`
       types instead of `Types.typ`/`pctx`) rather than duplicating
       types.ml's `typecheck_switch` unification: the first arm's
       trailing `ExprStmt`'s own type wins — types.ml already proved
       every other arm agrees, or reported WO-E201 if not, so trusting
       the first arm here is not a second, weaker check, just this
       file's own narrower deriver reading the same fact.

       Task 4 fix round 1 (review Critical 1b): an arm may yield its own
       payload BINDING (`case Boxed(b): b;` — the escape shape the
       log-watcher's own return pattern uses). The binding is not a
       local at derivation time (it exists only during the arm's own
       walk), so the plain recursive call typed the whole switch `None`
       -> the `Scalar "Int"` fallback -> Copy — a leak (unannotated
       `let`) or a bogus downstream error. `binding_ty_of_arm` reads the
       binding's type straight off the subject's variant declaration. *)
    (match arms with
     | [] -> None
     | first :: _ -> (
       match List.rev first.Ast.body with
       | { Ast.s_kind = ExprStmt ve; _ } :: _ -> (
         match ve.Ast.kind with
         | Ident n -> (
           match binding_ty_of_arm ctx subject first n with
           | Some fty -> Some fty
           | None -> expr_ty ctx ve)
         | _ -> expr_ty ctx ve)
       | _ -> None))

(* The declared type of payload binding [n], when [arm]'s pattern binds
   it off [subject]'s union — None whenever this is not that shape. *)
and binding_ty_of_arm (ctx : ctx) (subject : Ast.expr) (arm : Ast.switch_arm) (n : string) :
    Ast.field_ty option =
  match expr_ty ctx subject with
  | Some (Scalar sn) -> (
    match Types.StringMap.find_opt sn ctx.syms.Types.unions with
    | Some u when u.Types.u_has_payload -> (
      match arm.Ast.values with
      | [ { Ast.kind = Ast.Call ({ Ast.kind = Ast.Ident vname; _ }, bargs); _ } ] -> (
        match
          List.find_opt (fun (vi : Types.variant_info) -> vi.Types.vi_name = vname)
            u.Types.u_variants
        with
        | Some vi when List.length bargs = List.length vi.Types.vi_fields ->
          let rec zip (args : Ast.expr list) fields =
            match (args, fields) with
            | { Ast.kind = Ast.Ident bn; _ } :: _, (_, fty) :: _ when bn = n -> Some fty
            | _ :: ta, _ :: tf -> zip ta tf
            | _ -> None
          in
          zip bargs vi.Types.vi_fields
        | _ -> None)
      | _ -> None)
    | _ -> None)
  | _ -> None

and resolve_callee (ctx : ctx) (callee : Ast.expr) : callee option =
  let params_of ps = List.map (fun (n, _, conv) -> (n, conv)) ps in
  match callee.kind with
  | Ident name -> (
    match Types.StringMap.find_opt name ctx.syms.Types.free_fns with
    | Some (f : Types.free_fn_info) ->
      Some
        { ce_params = params_of f.Types.params; ce_ret = f.Types.ret; ce_recv_excl = false }
    (* A BUILTIN's return type, from the table types.ml and emit.ml already
       read. `split`/`split_ws`/`slice` hand back a fresh `multi` and
       `substr`/`trim`/`join`/… a fresh Text; without this they resolved to
       nothing, the binding fell back to Copy, and every one of those
       containers leaked (measured: the workload's supervisor mode). A
       user-declared fn of the same name wins above, the shadowing rule the
       builtin surface already states. *)
    | None -> (
      let arg0 = None in
      match Types.builtin_confident_ret name arg0 with
      | Some t -> (
        match Types.field_ty_of_typ t with
        | Some ft -> Some { ce_params = []; ce_ret = Some ft; ce_recv_excl = false }
        | None -> None)
      | None -> None))
  | Field (base, mname) -> (
    match expr_ty ctx base with
    | Some bt -> (
      match unwrap_nullable bt with
      | Scalar cn -> (
        match Types.StringMap.find_opt cn ctx.syms.Types.classes with
        | None -> None
        | Some (cls : Types.class_info) -> (
          match List.find_opt (fun (m : Types.method_info) -> m.Types.name = mname) cls.Types.methods with
          | None -> None
          | Some m ->
            Some
              { ce_params = params_of m.Types.params; ce_ret = m.Types.ret;
                ce_recv_excl = body_writes_self m.Types.body }))
      | _ -> None)
    (* The base is not a value: it names a reserved stdlib module
       (`fs.read_all(path, cap)`) or a class with a static member
       (`Tools.needle(cmd)`). Both shapes were unresolved here until
       2026-08-14, and an unresolved callee is not a missing *type* — it is a
       missing LIFETIME: analyze_let's None-fallback classifies the binding
       `Scalar "Int"`, oclass_of calls that Copy, and the fresh Text or
       `multi` the call returned is never dropped. That was the measured
       >1 MB leak in eight seconds of the workload's supervisor mode (one
       `fs.read_all` result per cron file). The tables read here are the same
       ones types.ml and emit.ml already read; a local of the same name
       shadows the module, exactly as it does everywhere else. *)
    | None -> (
      match base.kind with
      | Ident head when find_local ctx head = None -> (
        match Types.stdlib_member head mname with
        | Some sm ->
          Some
            { ce_params = [];
              ce_ret = ( match sm.Types.sm_ret with Some t -> Types.field_ty_of_typ t | None -> None);
              ce_recv_excl = false }
        | None -> (
          match Types.StringMap.find_opt head ctx.syms.Types.classes with
          | None -> None
          | Some (cls : Types.class_info) -> (
            match
              List.find_opt
                (fun (m : Types.method_info) -> m.Types.name = mname && m.Types.is_static)
                cls.Types.methods
            with
            | None -> None
            | Some m ->
              Some
                { ce_params = params_of m.Types.params; ce_ret = m.Types.ret;
                  ce_recv_excl = false })))
      | _ -> None))
  | _ -> None

(* ============================================================
   Places
   ============================================================ *)

(* A runtime index's identity. Simple place-shaped indices render to
   readable text so `items[i]` twice is recognized as the same element;
   anything else falls back to a node-unique token, so two complex
   indices are never mistaken for each other (Unknown -> residual site,
   the safe side) even when they look identical in source. *)
let rec idx_text (e : Ast.expr) : string =
  match e.kind with
  | IntLit n -> string_of_int n
  | Ident n -> n
  | Field (base, f) -> idx_text base ^ "." ^ f
  | _ -> "#" ^ string_of_int e.id

let idx_proj (e : Ast.expr) : proj =
  match e.kind with IntLit n -> PConst n | _ -> PDyn (idx_text e)

(* Builtins that hand back a POINTER INTO their container rather than a fresh
   value: binding one binds a borrow of that container, not a second owner.
   `pop`/`shift` are deliberately absent — they remove the element, so the
   caller really does take ownership. Now that Text is Owned (oclass_of), this
   distinction is what keeps `let v = get(m, k)` from dropping a string the
   map still holds. *)
let borrowing_builtin (name : string) : bool =
  List.mem name [ "get"; "latest"; "key_at"; "val_at" ]

let rec place_of (e : Ast.expr) : place option =
  match e.kind with
  | Ident n -> Some { root = n; projs = []; ppos = e.pos; pnode = e.id }
  | Call ({ kind = Ident bname; _ }, (container :: rest)) when borrowing_builtin bname -> (
    match place_of container with
    | Some p ->
      let proj = match rest with idx :: _ -> idx_proj idx | [] -> PDyn ("#" ^ string_of_int e.id) in
      Some { p with projs = p.projs @ [ proj ]; pnode = e.id }
    | None -> None)
  | Field (base, f) -> (
    match place_of base with
    | Some p -> Some { p with projs = p.projs @ [ PField f ]; pnode = e.id }
    | None -> None)
  | Index (base, idx) -> (
    match place_of base with
    | Some p -> Some { p with projs = p.projs @ [ idx_proj idx ]; pnode = e.id }
    | None -> None)
  | _ -> None

let place_ty (ctx : ctx) (p : place) : Ast.field_ty option =
  match find_local ctx p.root with
  | None -> None
  | Some l ->
    List.fold_left
      (fun acc pr ->
        match acc with
        | None -> None
        | Some t -> (
          match (pr, unwrap_nullable t) with
          | PField f, Scalar cn -> field_ty_of ctx cn f
          | (PConst _ | PDyn _), t' -> elem_ty t'
          | _ -> None))
      (Some l.l_ty) p.projs

let place_class (ctx : ctx) (p : place) : oclass =
  match place_ty ctx p with Some t -> oclass_of ctx t | None -> Copy

(* ============================================================
   Canonical places
   ============================================================ *)

(* A place written through a borrow binding names the *same storage* as the
   place that borrow came from, so alias questions must be asked about the
   canonical form, never the syntax. `let r = bag.items[i]` makes `r`
   canonically `bag.items[i]`, and `r.n` canonically `bag.items[i].n`;
   substitution is transitive (`let s = r.inner`). Without this,
   `swap(r, s)` would compare roots `r` and `s`, conclude Disjoint, and
   emit neither an error nor a residual site — the exact hole a syntactic
   root check leaves open, and the one case where nobody (not the compiler,
   not the VM) would be enforcing the rule.

   Termination: each substitution step records the root it replaced, so a
   self-referential chain created by shadowing (`let r = r.inner` in an
   inner scope, where the inner `r` shadows the outer one that the source
   place names) stops instead of looping. Such a chain stays partially
   canonicalized; it is the one imprecise case, and it cannot hang. *)
let canon_walk (ctx : ctx) (p : place) : place * string list =
  let rec go seen (p : place) =
    if List.mem p.root seen then (p, seen)
    else
      match find_local ctx p.root with
      | Some { l_src = Some src; _ } ->
        go (p.root :: seen) { p with root = src.root; projs = src.projs @ p.projs }
      | _ -> (p, p.root :: seen)
  in
  go [] p

let canon (ctx : ctx) (p : place) : place = fst (canon_walk ctx p)

(* Every root a place is reached *through*: the binding names it was written
   with, plus the root it finally lands on. `is_own_binding`'s test lives on
   this list, so a reborrow chain (`let t = r` where `r` borrows
   `bag.items[i]`) is treated the same as a direct binding — otherwise using
   `t` would be reported as conflicting with `r`, a borrow it *is*. *)
let canon_roots (ctx : ctx) (p : place) : string list = snd (canon_walk ctx p)

let relate_places (ctx : ctx) (a : place) (b : place) : rel =
  relate (canon ctx a) (canon ctx b)

(* `outer` is a proper container of `inner` (`bag` of `bag.items[i]`), not
   the same storage. *)
let is_strict_prefix (outer : place) (inner : place) : bool =
  outer.root = inner.root
  && List.length outer.projs < List.length inner.projs
  && relate_projs outer.projs inner.projs = Overlap


(* ============================================================
   Diagnostics
   ============================================================ *)

let report (ctx : ctx) ~code ~(pos : Ast.pos) ~message ~(rel : Ast.pos) ~label : unit =
  if ctx.recording then
    Diag.Collector.add ctx.coll
      (Diag.error ~code ~file:ctx.file ~line:pos.line ~col:pos.col ~message
         ~related:[ Diag.related_site ~file:ctx.file ~line:rel.line ~col:rel.col ~label ]
         ())

(* "borrowed here" wording, chosen from how the borrow was created —
   the second half of every two-site borrow error. *)
let borrow_label (l : local) : string =
  match l.l_src with
  | Some src -> Printf.sprintf "`%s` borrows `%s` here" l.l_name (place_text src)
  | None ->
    Printf.sprintf "`%s` is borrowed here — declare it `take %s: T` to pass ownership in" l.l_name
      l.l_name

(* ============================================================
   Table recording
   ============================================================ *)

let record_move (ctx : ctx) (p : place) (kind : move_kind) : unit =
  if ctx.recording then
    ctx.sink.s_moves <-
      { mv_node = p.pnode; mv_pos = p.ppos; mv_place = place_text p; mv_kind = kind }
      :: ctx.sink.s_moves

let record_drop (ctx : ctx) ~node ~(pos : Ast.pos) ~kind ~items : unit =
  if ctx.recording && items <> [] then
    ctx.sink.s_drops <-
      { dr_node = node; dr_pos = pos; dr_kind = kind; dr_items = items } :: ctx.sink.s_drops

let record_residual (ctx : ctx) ~node ~(pos : Ast.pos) ~(a : place) ~a_kind ~(b : place) ~b_kind :
    unit =
  if ctx.recording then
    ctx.sink.s_res <-
      (* canonical text, so two accesses that reach the same container
         through different borrow bindings read as what they are; the node
         ids stay the precise key *)
      { rs_node = node; rs_pos = pos; rs_a = place_text (canon ctx a); rs_a_kind = a_kind;
        rs_a_node = a.pnode; rs_b = place_text (canon ctx b); rs_b_kind = b_kind;
        rs_b_node = b.pnode }
      :: ctx.sink.s_res

(* ============================================================
   Scopes, live sets, snapshots
   ============================================================ *)

let push_scope (ctx : ctx) ~node ~pos ~label : unit =
  ctx.scopes <- { sc_node = node; sc_pos = pos; sc_label = label; sc_locals = [] } :: ctx.scopes

let declare (ctx : ctx) (l : local) : unit =
  match ctx.scopes with
  | [] -> ()
  | sc :: _ -> sc.sc_locals <- l :: sc.sc_locals

let is_live_holder (l : local) : bool =
  l.l_holds && match l.l_state with Live -> true | Moved _ | Borrowed _ -> false

(* Every live holder, innermost scope first, destruction order inside
   each scope. *)
let live_holders (ctx : ctx) : local list =
  List.concat_map (fun sc -> List.filter is_live_holder sc.sc_locals) ctx.scopes

(* haxe-parity Task 2: every live holder from the innermost scope up to
   and including the scope whose sc_node is [loop_node] (the nearest
   enclosing loop's own body scope) — DBreak/DContinue's own bound
   version of live_holders, which goes all the way to the function's
   own outermost scope (return's job: leave the whole frame). A
   break/continue only leaves the loop, so scopes *outside* it are
   untouched — their own DScope drop still runs later, at the loop's
   normal exit. *)
let live_holders_upto (ctx : ctx) (loop_node : int) : local list =
  let rec go = function
    | [] -> []
    | (sc : scope) :: rest ->
      let here = List.filter is_live_holder sc.sc_locals in
      if sc.sc_node = loop_node then here else here @ go rest
  in
  go ctx.scopes

let owned_items (ls : local list) : drop_item list =
  ls
  |> List.filter (fun l -> l.l_class = Owned)
  |> List.map (fun l -> { di_name = l.l_name; di_kind = LOwned; di_node = l.l_node })

let mask_items (ls : local list) : drop_item list =
  List.map
    (fun l ->
      { di_name = l.l_name; di_kind = (if l.l_class = Gc then LGc else LOwned); di_node = l.l_node })
    ls

let pop_scope (ctx : ctx) : unit =
  match ctx.scopes with
  | [] -> ()
  | sc :: rest ->
    if not ctx.diverged then begin
      let live = List.filter is_live_holder sc.sc_locals in
      record_drop ctx ~node:sc.sc_node ~pos:sc.sc_pos ~kind:(DScope sc.sc_label)
        ~items:(owned_items live)
    end;
    ctx.scopes <- rest

type snapshot = (local * state) list

let snapshot (ctx : ctx) : snapshot =
  List.concat_map (fun sc -> List.map (fun l -> (l, l.l_state)) sc.sc_locals) ctx.scopes

let restore (s : snapshot) : unit = List.iter (fun (l, st) -> l.l_state <- st) s

let earlier (a : Ast.pos) (b : Ast.pos) : Ast.pos =
  if (a.line, a.col) <= (b.line, b.col) then a else b

let join_state (a : state) (b : state) : state =
  match (a, b) with
  | Moved p1, Moved p2 -> Moved (earlier p1 p2)
  | Moved p, _ | _, Moved p -> Moved p
  | Borrowed p, _ | _, Borrowed p -> Borrowed p
  | Live, Live -> Live

(* Both snapshots come from the same physical local records (branch-local
   locals are gone by the time a branch is joined), so keys are compared
   with physical equality. *)
let join (a : snapshot) (b : snapshot) : snapshot =
  List.map
    (fun (l, sa) ->
      let sb = try List.assq l b with Not_found -> sa in
      (l, join_state sa sb))
    a

(* ============================================================
   Borrow bookkeeping
   ============================================================ *)

(* Borrows of a place in this frame that are still live: let-bound
   aliases and loop cursors. Parameter borrows are excluded — their
   source is the caller's, and the caller checks it. @gc aliases are
   excluded because @gc is exempt. *)
let outstanding_borrows (ctx : ctx) : (local * place) list =
  List.concat_map
    (fun sc ->
      List.filter_map
        (fun l ->
          match (l.l_src, l.l_class, l.l_state) with
          | Some src, Owned, Borrowed _ -> Some (l, src)
          | _ -> None)
        sc.sc_locals)
    ctx.scopes

(* An access through a borrow's own binding (`it.touch()` inside
   `for it in items`) is what the borrow is *for*, so it never conflicts
   with that same borrow — nor with any borrow further up the chain the
   access is reached through. Checked on the roots the place was *written*
   with, since after canonicalization an access and its own borrow are the
   same place by construction. *)
let is_own_binding (ctx : ctx) (l : local) (p : place) : bool =
  List.mem l.l_name (canon_roots ctx p)

(* [skip_roots] names borrow bindings that another access in the same
   region already stands for; without it, `swap(r, s)` (both borrow
   bindings) would be reported once by the region's pairwise check and then
   twice more here, once per (access, other binding) pair. *)
let check_against_borrows (ctx : ctx) ~(node : int) ~(pos : Ast.pos) ?(skip_roots = [])
    (p : place) (kind : acc_kind) : unit =
  List.iter
    (fun ((l : local), src) ->
      if not (is_own_binding ctx l p || List.mem l.l_name skip_roots) then
        match relate_places ctx p src with
        | Overlap ->
          let code, message =
            match kind with
            | AMove ->
              ( move_while_borrowed_code,
                Printf.sprintf "cannot move `%s` while `%s` is borrowed" (place_text p)
                  (place_text src) )
            | AExcl | AShared ->
              (* "mutate" covers both exclusive accesses this reaches: a
                 `mut` argument and an assignment to the place *)
              ( conflicting_borrow_code,
                Printf.sprintf "cannot mutate `%s` while `%s` is borrowed" (place_text p)
                  (place_text src) )
          in
          report ctx ~code ~pos ~message ~rel:l.l_pos ~label:(borrow_label l)
        | Unknown ->
          record_residual ctx ~node ~pos ~a:p ~a_kind:kind ~b:src ~b_kind:l.l_bkind
        | Disjoint -> ())
    (outstanding_borrows ctx)

(* ============================================================
   Moves and escapes
   ============================================================ *)

let root_local (ctx : ctx) (p : place) : local option = find_local ctx p.root

let is_borrow_root (ctx : ctx) (p : place) : local option =
  match root_local ctx p with
  | Some l -> ( match l.l_state with Borrowed _ -> Some l | Live | Moved _ -> None)
  | None -> None

(* Reading a place: the one place use-after-move is reported. When the
   move site *is* the use site, the move happened on an earlier turn
   through a loop (the fixpoint's second pass found it) — say so, because
   "moved here" pointing at the line you are already looking at is
   otherwise baffling. *)
let use_place (ctx : ctx) (p : place) : unit =
  match root_local ctx p with
  | Some { l_state = Moved mpos; l_name; _ } ->
    let same_site = mpos.line = p.ppos.line && mpos.col = p.ppos.col in
    report ctx ~code:use_after_move_code ~pos:p.ppos
      ~message:
        (if same_site then
           Printf.sprintf "use of `%s` after it was moved on the previous loop iteration" l_name
         else Printf.sprintf "use of `%s` after it was moved" l_name)
      ~rel:mpos
      ~label:
        (if same_site then Printf.sprintf "`%s` is moved here, once per iteration" l_name
         else Printf.sprintf "`%s` moved here" l_name)
  | _ -> ()

(* The user class an escaping value's type resolves to, if any (unwrapping `?`).
   A class value that escapes is the demand-promotion signal: second-class
   borrows cannot be stored or returned, so a class that must escape has to be
   traced. Scalars, builtins, and non-class names are genuine escape errors. *)
let class_of_ty (syms : Types.symbols) (t : Ast.field_ty) : string option =
  let rec base = function Ast.Nullable ft -> base ft | ft -> ft in
  match base t with
  | Ast.Scalar n when Types.StringMap.mem n syms.Types.classes -> Some n
  | _ -> None

let escape (ctx : ctx) (l : local) ~(pos : Ast.pos) ~message
    ~(promote_class : string option) : unit =
  (* `promote_class` is the class of the value that actually escapes (the
     escaping place's type), not the borrow-root local's — so `return h.box`
     promotes `Box`, never `Holder`. In collect mode with a class in hand, record
     it; otherwise report WO-E304. *)
  match (ctx.promote, promote_class) with
  | Some record, Some c -> record c
  | _ ->
    report ctx ~code:borrow_escape_code ~pos ~message ~rel:l.l_pos
      ~label:(borrow_label l)

(* Whether passing/assigning this place would be a *real* transfer — the
   positive half of `transfer`'s decision below, needed one step earlier by
   analyze_call: an argument that cannot transfer (a borrow, a projection,
   an already-moved local) must be classified as a read, or the region's
   pairwise check reports a bogus move conflict on top of the escape error
   `transfer` is about to give. *)
(* A `Text` stored into a field or a record is COPIED by the VM (SETF's own
   rule, the same one push/set follow) — so it is neither a move out of the
   source nor a borrow escaping its scope. Without this, `fn rename(name: Text)
   { self.name = name }` — the most ordinary line in the workload — is
   WO-E304, and the only way to write it would be `take name: Text`. Copying
   is what keeps a field's owner the object itself. *)
let stores_by_copy (ctx : ctx) (p : place) : bool =
  match place_ty ctx p with
  | Some t -> ( match unwrap_nullable t with
                | Scalar n -> Types.is_heap_scalar n
                | _ -> false)
  | None -> false

let is_real_transfer (ctx : ctx) (p : place) : bool =
  p.projs = []
  && match root_local ctx p with Some l -> l.l_holds && l.l_state = Live | None -> false

(* The one gate every ownership transfer goes through. Returns true when
   a real transfer happened (the caller then records the move site with
   the right kind). `what` names the escape flavour for the E304 message.

   Order of decisions matters: a moved-from local was already reported by
   use_place, a borrow can never be moved out of, and a projection is not
   a move at all in milestone 1 (see the module doc). *)
let transfer (ctx : ctx) (p : place) ~(what : string) : bool =
  match place_class ctx p with
  | Copy -> false
  | Gc -> false (* traced values alias freely; tracing owns the lifetime *)
  | Owned -> (
    match is_borrow_root ctx p with
    | Some l ->
      escape ctx l ~pos:p.ppos
        ~message:
          (Printf.sprintf "borrow of `%s` %s — borrows cannot outlive their scope" (place_text p)
             what)
        ~promote_class:
          (match place_ty ctx p with Some t -> class_of_ty ctx.syms t | None -> None);
      false
    | None -> (
      match root_local ctx p with
      | None -> false
      | Some l -> (
        match l.l_state with
        | Moved _ -> false (* already reported at the read *)
        | Borrowed _ -> false (* unreachable: is_borrow_root covered it *)
        | Live ->
          if p.projs <> [] then false (* no partial moves in milestone 1 *)
          else begin
            check_against_borrows ctx ~node:p.pnode ~pos:p.ppos p AMove;
            l.l_state <- Moved p.ppos;
            true
          end)))

(* ============================================================
   The walk
   ============================================================ *)

(* One access inside a call's region. Its position is the accessed
   place's own root token (place.ppos), so no separate field. *)
type access = {
  ac_place : place;
  ac_kind : acc_kind;
}

let rec read_expr (ctx : ctx) (e : Ast.expr) : unit =
  match e.kind with
  (* iteration 19: a Float literal owns nothing — it is a word in a register,
     exactly like an Int. (A Bytes value DOES own its heap object, but Bytes
     has no literal form, so nothing new lands in this arm.) *)
  | IntLit _ | FloatLit _ | StrLit _ | BoolLit _ -> ()
  | Ident _ | Field _ | Index _ ->
    (match place_of e with Some p -> use_place ctx p | None -> ());
    read_place_parts ctx e
  | Call (callee, args) -> analyze_call ctx e callee args
  | Ctor (cn, fields) -> analyze_ctor ctx cn fields
  (* arc: spawn's ctor half moves fields exactly as a ctor literal does;
     the result (an address) is Copy, so no drop for the spawn itself *)
  | Spawn (cn, fields) -> analyze_ctor ctx cn fields
  | Insert (_, fields) ->
    (* iteration 9 Task 3: the engine copies every field value at the row
       API (the two-worlds bulkhead), so an insert BORROWS its values —
       no transfer, no E304, the source keeps what it had. Trap-capable
       (unique violations arrive with Task 4), so the drop map is
       recorded exactly like DbStub's. *)
    List.iter (fun (_, fe) -> read_expr ctx fe) fields;
    record_drop ctx ~node:e.id ~pos:e.pos ~kind:DLiveMask ~items:(mask_items (live_holders ctx))
  | Unary (_, o) -> read_expr ctx o
  | Binary (_, a, b) ->
    read_expr ctx a;
    read_expr ctx b
  | Interp inner -> read_expr ctx inner
  (* A list literal reads each element and hands it to the fresh
     container, exactly as `push(m, v)` does — so it inherits `push`'s own
     open gap, recorded in docs/plan/oop-vm/08-builtin-surface.md: an
     element that is a *borrowed* non-constant Text (or any borrowed
     owned value) is stored by pointer while the container's declared
     element kind makes it the container's to free. The workload's own
     literals are string constants (never freed) plus borrowed params
     handed straight to a stdlib call, so nothing reachable today hits
     it; it is a runtime-semantics gap to close with `push`, not a
     literal-specific one. *)
  | ListLit items -> List.iter (read_expr ctx) items
  | MapLit | NilLit -> ()
  | As (inner, _) -> read_expr ctx inner
  | Try t -> analyze_try ctx e t.body t.ename t.handler
  | DbStub _ ->
    (* trap-capable: the frame needs its drop map here *)
    record_drop ctx ~node:e.id ~pos:e.pos ~kind:DLiveMask ~items:(mask_items (live_holders ctx))
  | Delete t ->
    read_expr ctx t;
    record_drop ctx ~node:e.id ~pos:e.pos ~kind:DLiveMask ~items:(mask_items (live_holders ctx))
  | Query q ->
    (* iteration 9b: the sub-expressions only READ (engine field-reads copy
       out at the boundary); the query is trap-capable (engine faults), so
       the frame needs its drop map here, exactly like DbStub. *)
    (match q.q_src with QNav e2 -> read_expr ctx e2 | QTable _ -> ());
    List.iter (read_expr ctx) q.q_wheres;
    (match q.q_group with Some (_, k) -> read_expr ctx k | None -> ());
    (match q.q_order with Some (k, _) -> read_expr ctx k | None -> ());
    (match q.q_take with Some t -> read_expr ctx t | None -> ());
    read_expr ctx q.q_select;
    record_drop ctx ~node:e.id ~pos:e.pos ~kind:DLiveMask ~items:(mask_items (live_holders ctx))
  | Switch (subject, arms) -> analyze_switch ctx e.id subject arms

(* The root of a place expression is already accounted for by use_place;
   what still needs walking are index subexpressions and a non-place base
   (`f().x`). *)
and read_place_parts (ctx : ctx) (e : Ast.expr) : unit =
  match e.kind with
  | Ident _ -> ()
  | Field (base, _) -> read_place_parts ctx base
  | Index (base, idx) ->
    read_place_parts ctx base;
    read_expr ctx idx
  | _ -> read_expr ctx e

and analyze_ctor (ctx : ctx) (cn : string) (fields : (string * Ast.expr) list) : unit =
  List.iter
    (fun (fname, fe) ->
      read_expr ctx fe;
      match place_of fe with
      | None -> ()
      | Some p ->
        if stores_by_copy ctx p then () (* the field gets its own copy *)
        else if transfer ctx p ~what:(Printf.sprintf "cannot be stored in `%s.%s`" cn fname) then
          record_move ctx p (MvCtorField fname))
    fields

(* A call is one region: the receiver borrow and every argument borrow
   are live at the same time, so they are checked pairwise against each
   other and against any outstanding borrow in the frame. Reads happen
   first (against the pre-call state), then the conflict checks, then the
   transfers — that ordering is what makes `f(x, take x)` a
   move-while-borrowed rather than a use-after-move. *)
and analyze_call (ctx : ctx) (call_e : Ast.expr) (callee : Ast.expr) (args : Ast.expr list) : unit =
  (* haxe-parity Task 4: a variant construction is not a call — it
     lowers to NEW + SETF, no CALL instruction — so every place-shaped
     payload argument is a ctor-field ESCAPE (analyze_ctor's own rule:
     the value is stored into the fresh object, which owns it from
     here), never a borrow-for-the-duration-of-the-call. Getting this
     wrong is a double free, not an imprecision: a local moved into the
     payload would otherwise stay Live and be dropped at scope end on
     top of the variant object's own recursive drop. No LIVE-MASK is
     recorded either — there is no trap-capable call site to sync a
     drop map at. A declared free fn of the same name shadows the
     variant (the same flat-table resolution resolve_callee itself
     uses). *)
  let variant_ctor =
    match callee.kind with
    | Ident n when not (Types.StringMap.mem n ctx.syms.Types.free_fns) ->
      Types.find_variant ctx.syms n
    | _ -> None
  in
  match variant_ctor with
  | Some (_, vi) ->
    List.iteri
      (fun i (fe : Ast.expr) ->
        read_expr ctx fe;
        match place_of fe with
        | None -> ()
        | Some p ->
          let fname =
            match List.nth_opt vi.Types.vi_fields i with
            | Some (n, _) -> n
            | None -> Printf.sprintf "arg%d" (i + 1)
          in
          if
            transfer ctx p
              ~what:(Printf.sprintf "cannot be stored in `%s.%s`" vi.Types.vi_name fname)
          then record_move ctx p (MvCtorField fname))
      args
  | None ->
  let resolved = resolve_callee ctx callee in
  (* receiver *)
  let recv =
    match callee.kind with
    | Ident _ -> None
    | Field (base, _) ->
      read_expr ctx base;
      let excl = match resolved with Some c -> c.ce_recv_excl | None -> false in
      (match place_of base with
      | Some p ->
        if place_class ctx p = Owned then
          Some { ac_place = p; ac_kind = (if excl then AExcl else AShared) }
        else None
      | None -> None)
    | _ ->
      read_expr ctx callee;
      None
  in
  List.iter (fun a -> read_expr ctx a) args;
  (* argument conventions, positionally; unresolved or over-supplied
     arguments fall back to a shared borrow *)
  let conv_of i =
    match resolved with
    | None -> (Printf.sprintf "arg%d" (i + 1), Borrow)
    | Some c -> ( match List.nth_opt c.ce_params i with Some p -> p | None -> ("_", Borrow))
  in
  let arg_accesses =
    List.concat
      (List.mapi
         (fun i a ->
           match place_of a with
           | None -> []
           | Some p -> (
             let _, conv = conv_of i in
             match (place_class ctx p, conv) with
             | Copy, _ | Gc, _ -> []
             | Owned, Take ->
               let kind = if is_real_transfer ctx p then AMove else AShared in
               [ { ac_place = p; ac_kind = kind } ]
             | Owned, Mut -> [ { ac_place = p; ac_kind = AExcl } ]
             | Owned, Borrow -> [ { ac_place = p; ac_kind = AShared } ]))
         args)
  in
  let accesses = (match recv with Some r -> [ r ] | None -> []) @ arg_accesses in
  (* pairwise, in argument order *)
  let rec pairs = function
    | [] -> ()
    | a :: tl ->
      List.iter (fun b -> check_pair ctx call_e a b) tl;
      pairs tl
  in
  pairs accesses;
  (* An exclusive borrow also has to clear the frame's outstanding borrows
     (moves do that inside `transfer`) — minus the bindings this region's
     own accesses already stand for, which the pairwise check just
     covered. *)
  let region_roots = List.map (fun a -> a.ac_place.root) accesses in
  List.iter
    (fun a ->
      if a.ac_kind = AExcl then
        check_against_borrows ctx ~node:call_e.id ~pos:a.ac_place.ppos ~skip_roots:region_roots
          a.ac_place AExcl)
    accesses;
  (* transfers last *)
  (* iteration 7b deleted the `push`-of-a-gc-value special case (an RC_INC
     escape site): a traced value stored into a container needs no
     bookkeeping — tracing finds it through the container.

     Storing an OWNED, non-copied value into a container is a MOVE, exactly
     like a `take` argument: the container owns the element and frees it in
     its own drop plan (runtime/src/gc.c multi_free/map_free), so the caller
     dropping it too was a double free. This was the disclosed
     "move-on-push" gap — it stayed latent while pushed elements were Texts
     (copied at the boundary, 2026-08-14) and detonated the moment iteration
     16's route-table pattern pushed a CLASS value (`push(self.routes, r)`
     plus the take-param's scope-end DROP = the container's element freed
     twice). `push`'s value slot and `set`'s key/value slots transfer;
     Text/json.Value stay copy-stored (`stores_by_copy`), so their fresh-
     value drop is still the caller's. A user-declared fn of the same name
     wins, exactly like the builtin table's shadowing rule. *)
  let container_store_slot (i : int) : bool =
    match callee.kind with
    | Ident "push" -> i = 1 && Types.StringMap.find_opt "push" ctx.syms.Types.free_fns = None
    | Ident "set" ->
      (i = 1 || i = 2) && Types.StringMap.find_opt "set" ctx.syms.Types.free_fns = None
    (* arc: send(addr, msg) MOVES the message to the runtime — the sender's
       binding dies (compile-time move, iteration 8's criterion).
       iteration 24: call(addr, msg) moves its message identically. *)
    | Ident "send" -> i = 1 && Types.StringMap.find_opt "send" ctx.syms.Types.free_fns = None
    | Ident "call" -> i = 1 && Types.StringMap.find_opt "call" ctx.syms.Types.free_fns = None
    | _ -> false
  in
  List.iteri
    (fun i a ->
      match place_of a with
      | None -> ()
      | Some p ->
        let pname, conv = conv_of i in
        if conv = Take then
          (if transfer ctx p ~what:(Printf.sprintf "cannot be passed to `take %s`" pname) then
             record_move ctx p (MvArg pname))
        else if container_store_slot i && place_class ctx p = Owned
                && not (stores_by_copy ctx p) then
          (if
             transfer ctx p
               ~what:
                 (match callee.kind with
                 | Ident "send" | Ident "call" ->
                   "cannot be sent — a message moves to the receiver"
                 | _ -> "cannot be stored in a container")
           then record_move ctx p (MvArg "element"))
        )
    args;
  record_drop ctx ~node:call_e.id ~pos:call_e.pos ~kind:DLiveMask
    ~items:(mask_items (live_holders ctx))

(* The pairwise equivalent of is_own_binding: one side reaches the storage
   through a borrow binding, the other names the place that borrow came
   from (`bag.eat(it)` inside `for it in bag.items`). Using a borrow
   alongside the thing it borrows from is what the binding is for, so it is
   not a conflict — and after canonicalization the two would otherwise
   always Overlap. Two *different* bindings of the same container (`r` and
   `s`) are not this case and stay checked. *)
and reuses_binding (ctx : ctx) (a : place) (b : place) : bool =
  (* narrow on purpose: only when one side is written through a borrow
     binding *and* the other side names a proper container of what that
     borrow points at. Two bindings of the same container (`r` and `s`), or
     a binding against the very element it borrows, are not this case and
     stay checked. *)
  let through (p : place) (other : place) =
    match find_local ctx p.root with
    | Some { l_src = Some _; _ } -> is_strict_prefix (canon ctx other) (canon ctx p)
    | _ -> false
  in
  through a b || through b a

and check_pair (ctx : ctx) (call_e : Ast.expr) (a : access) (b : access) : unit =
  if a.ac_kind = AShared && b.ac_kind = AShared then ()
  else if reuses_binding ctx a.ac_place b.ac_place then ()
  else
    match relate_places ctx a.ac_place b.ac_place with
    | Disjoint -> ()
    | Unknown ->
      record_residual ctx ~node:call_e.id ~pos:call_e.pos ~a:a.ac_place ~a_kind:a.ac_kind
        ~b:b.ac_place ~b_kind:b.ac_kind
    | Overlap -> (
      match (a.ac_kind, b.ac_kind) with
      | AMove, AMove ->
        report ctx ~code:use_after_move_code ~pos:b.ac_place.ppos
          ~message:
            (Printf.sprintf "use of `%s` after it was moved" (place_text b.ac_place))
          ~rel:a.ac_place.ppos
          ~label:(Printf.sprintf "`%s` moved here" (place_text a.ac_place))
      | AMove, _ ->
        report ctx ~code:move_while_borrowed_code ~pos:a.ac_place.ppos
          ~message:
            (Printf.sprintf "cannot move `%s` while `%s` is borrowed in the same call"
               (place_text a.ac_place) (place_text b.ac_place))
          ~rel:b.ac_place.ppos
          ~label:(Printf.sprintf "`%s` borrowed here" (place_text b.ac_place))
      | _, AMove ->
        report ctx ~code:move_while_borrowed_code ~pos:b.ac_place.ppos
          ~message:
            (Printf.sprintf "cannot move `%s` while `%s` is borrowed in the same call"
               (place_text b.ac_place) (place_text a.ac_place))
          ~rel:a.ac_place.ppos
          ~label:(Printf.sprintf "`%s` borrowed here" (place_text a.ac_place))
      | _ ->
        report ctx ~code:conflicting_borrow_code ~pos:b.ac_place.ppos
          ~message:
            (Printf.sprintf "cannot borrow `%s` as `mut` twice in the same call"
               (place_text b.ac_place))
          ~rel:a.ac_place.ppos
          ~label:(Printf.sprintf "`%s` first borrowed here" (place_text a.ac_place)))

(* ---- statements ------------------------------------------------------ *)

(* Join normalization. A value moved on one branch of an `if` and not the
   other leaves the merge in two different real states while the join can
   record only one — and it records Moved, so nothing would ever drop the
   value on the path that did *not* move it: a leak. Rather than spend a
   runtime drop flag, the non-moving branch drops it at its own end, which
   makes the joined Moved state true on both paths. Sound because the join
   already marked it Moved, so a later use is WO-E301 either way. *)
and branch_join_drops (ctx : ctx) ~(node : int) ~(label : string) ~(pos : Ast.pos)
    ~(moving : snapshot) ~(other : snapshot) : unit =
  let items =
    List.filter_map
      (fun ((l : local), st_other) ->
        let st_moving = try List.assq l moving with Not_found -> st_other in
        match (st_other, st_moving) with
        | Live, Moved _ when l.l_holds && l.l_class = Owned ->
          Some { di_name = l.l_name; di_kind = LOwned; di_node = l.l_node }
        | _ -> None)
      other
  in
  record_drop ctx ~node ~pos ~kind:(DBranchJoin label) ~items

(* haxe-parity Task 3: `switch`'s own arms are alternate flows joining
   back together after the switch — exactly what `if`/`else` already
   is, generalized from two branches to N (one per arm). Reused, not
   reinvented, per the brief's own instruction: each arm is its own
   `analyze_block` (so an arm-local owned value that is never moved
   still gets its ordinary DScope drop at that arm's own end — nothing
   special to write for that half); a diverging arm (every path inside
   it returned) drops out of the join exactly like a diverging `if`
   branch does; and a value moved in *some* arms but not others gets
   `branch_join_drops`'s own JOIN-DROP treatment, called once per kept
   arm against the join of every *other* (non-diverging) arm's ending
   state — the N-way shape of the same "the branch that kept it drops
   it at its own end" rule the module doc above states for two.

   The subject is read (`read_expr`, never `transfer`) exactly once,
   before any arm runs: it is compared against, never consumed — "the
   SUBJECT's ownership — borrowed for the comparison, not consumed" per
   this task's own brief. Case values are read the same way; for this
   task's scalar/Text subjects they are always literals, so this is a
   no-op today and only matters once a union variant tag becomes a real
   bound reference (Task 4). *)
and analyze_switch (ctx : ctx) (node : int) (subject : Ast.expr) (arms : Ast.switch_arm list) :
    unit =
  read_expr ctx subject;
  (* haxe-parity Task 4: over a union-typed subject the case "values"
     are variant PATTERNS (`case Ok:`, `case Failed(reason):`), not
     expressions — never read as such (a pattern's binding names are
     unbound on purpose; reading them would be noise at best). The
     subject's own union-ness comes from this pass's own expr_ty,
     deliberately NOT unwrapped through `?T` (types.ml's subj_union
     makes the same call — a `?Union` subject stays on the plain-value
     path until Task 6). *)
  let subj_union =
    match expr_ty ctx subject with
    | Some (Scalar n) -> Types.StringMap.find_opt n ctx.syms.Types.unions
    | _ -> None
  in
  (match subj_union with
   | Some _ -> ()
   | None ->
     List.iter (fun (a : Ast.switch_arm) -> List.iter (read_expr ctx) a.Ast.values) arms);
  (* A payload pattern's bindings are BORROWS of the subject's own
     fields (`case Failed(reason):` reads `reason` straight out of the
     variant object via GETF — the subject keeps owning the payload, so
     the binding must never be dropped by the arm or the subject's own
     drop double-frees). Declared into the arm's own scope, exactly like
     a `for` cursor is into its loop's (same Borrowed state, same
     l_holds = false); when the subject is a place, the binding's source
     place is that place plus the field projection, so moving the
     subject out from under a live binding is the ordinary WO-E302. *)
  let arm_bindings (a : Ast.switch_arm) : local list =
    match subj_union with
    | None -> []
    | Some u -> (
      match a.Ast.values with
      | [ { Ast.kind = Ast.Call ({ Ast.kind = Ast.Ident vname; _ }, args); _ } ] -> (
        match
          List.find_opt (fun (v : Types.variant_info) -> v.Types.vi_name = vname)
            u.Types.u_variants
        with
        | Some vi when List.length args = List.length vi.Types.vi_fields ->
          let subj_place = place_of subject in
          List.concat
            (List.map2
               (fun (arg : Ast.expr) (fname, fty) ->
                 match arg.Ast.kind with
                 | Ast.Ident bn ->
                   let src =
                     match subj_place with
                     | Some p ->
                       Some { p with projs = p.projs @ [ PField fname ]; pnode = arg.Ast.id }
                     | None -> None
                   in
                   [ { l_name = bn; l_ty = fty; l_class = oclass_of ctx fty;
                       l_node = arg.Ast.id; l_pos = arg.Ast.pos; l_holds = false; l_src = src;
                       l_bkind = AShared; l_state = Borrowed arg.Ast.pos } ]
                 | _ -> [])
               args vi.Types.vi_fields)
        | _ -> [])
      | _ -> [])
  in
  let entry = snapshot ctx in
  let div0 = ctx.diverged in
  (* review fix, Critical 1: walk the *lowering* order (default last),
     not raw source order — see Ast.switch_lowering_order's own doc
     comment. "ARM<i>" must be the same index emit.ml's own
     emit_switch hands the owner tables, or every DScope/JOIN-DROP
     lookup below silently misses. *)
  let results =
    List.mapi
      (fun i (a : Ast.switch_arm) ->
        restore entry;
        ctx.diverged <- div0;
        let label = Printf.sprintf "ARM%d" i in
        analyze_block ctx ~pre:(arm_bindings a) ~node ~pos:a.Ast.arm_pos ~label a.Ast.body;
        (label, a.Ast.arm_pos, snapshot ctx, ctx.diverged))
      (Ast.switch_lowering_order arms)
  in
  let non_diverged = List.filter (fun (_, _, _, d) -> not d) results in
  match non_diverged with
  | [] ->
    (* every arm diverged (or there were no arms at all — a malformed
       switch types.ml already reports on): nothing reachable follows,
       so — mirroring analyze_stmt's own `If` case, which restores
       *some* snapshot purely for hygiene even though it is provably
       unobservable — restore the last arm's ending state, if any. *)
    (match List.rev results with (_, _, sn, _) :: _ -> restore sn | [] -> ());
    ctx.diverged <- true
  | (_, _, first_sn, _) :: rest ->
    List.iter
      (fun (label, pos, sn, _) ->
        match List.filter (fun (l, _, _, _) -> l <> label) non_diverged with
        | [] -> () (* the only non-diverging arm: nothing else could have moved anything *)
        | (_, _, first_other, _) :: rest_other ->
          let moved_elsewhere =
            List.fold_left (fun acc (_, _, s, _) -> join acc s) first_other rest_other
          in
          branch_join_drops ctx ~node ~label ~pos ~moving:moved_elsewhere ~other:sn)
      non_diverged;
    restore (List.fold_left (fun acc (_, _, s, _) -> join acc s) first_sn rest);
    ctx.diverged <- div0

and analyze_stmt (ctx : ctx) (s : Ast.stmt) : unit =
  match s.s_kind with
  | Let { name; ty; value } -> analyze_let ctx s name ty value
  | Assign { target; value } -> analyze_assign ctx s target value
  | ExprStmt e -> read_expr ctx e
  | Return opt -> analyze_return ctx s opt
  | If { cond; then_body; else_body } ->
    read_expr ctx cond;
    let entry = snapshot ctx in
    let div0 = ctx.diverged in
    analyze_block ctx ~node:s.s_id ~pos:s.s_pos ~label:"THEN" then_body;
    let after_then = snapshot ctx in
    let div_then = ctx.diverged in
    restore entry;
    ctx.diverged <- div0;
    let after_else, div_else =
      match else_body with
      | None -> (entry, div0)
      | Some (epos, body) ->
        analyze_block ctx ~node:s.s_id ~pos:epos ~label:"ELSE" body;
        let sn = snapshot ctx in
        let dv = ctx.diverged in
        restore entry;
        (sn, dv)
    in
    (* a branch that returned contributes no state to the merge *)
    if div_then && div_else then begin
      restore after_then;
      ctx.diverged <- true
    end
    else if div_then then begin
      restore after_else;
      ctx.diverged <- div0
    end
    else if div_else then begin
      restore after_then;
      ctx.diverged <- div0
    end
    else begin
      (* neither branch returned: normalize a one-sided move by dropping on
         the side that kept the value. An `if` without `else` anchors its
         implicit else at the `if` token — there is no other position to
         name. *)
      let else_anchor = match else_body with Some (epos, _) -> epos | None -> s.s_pos in
      branch_join_drops ctx ~node:s.s_id ~label:"ELSE" ~pos:else_anchor ~moving:after_then
        ~other:after_else;
      branch_join_drops ctx ~node:s.s_id ~label:"THEN" ~pos:s.s_pos ~moving:after_else
        ~other:after_then;
      restore (join after_then after_else);
      ctx.diverged <- div0
    end
  | While { cond; body } ->
    ctx.loop_stack <- s.s_id :: ctx.loop_stack;
    fixpoint ctx
      (fun () ->
        read_expr ctx cond;
        analyze_block ctx ~node:s.s_id ~pos:s.s_pos ~label:"WHILE" body);
    ctx.loop_stack <- List.tl ctx.loop_stack
  | For { var; var2; iter; body } ->
    read_expr ctx iter;
    let src = place_of iter in
    let iter_ty = expr_ty ctx iter in
    (* `for k, v in m` binds the map's key and value types; the one-name
       form binds the element type (elem_ty). Both cursors are BORROWS of
       what the container owns, so neither is ever dropped by the body. *)
    let key_ty, item_ty =
      match (iter_ty, var2) with
      | Some (Map (k, v)), Some _ -> (Some (Scalar k), Scalar v)
      | Some (Nullable (Map (k, v))), Some _ -> (Some (Scalar k), Scalar v)
      | Some t, _ -> (None, ( match elem_ty t with Some e -> e | None -> t))
      | None, _ -> (None, Scalar "Int")
    in
    (* A cursor over Text elements holds a COPY, not a borrow: the emitter
       copies each element as it loads it (the same boundary rule containers,
       fields and returns follow), so the body owns its cursor and drops it per
       iteration. That is also what lets `for k, v in m { v.field = … }` work —
       a borrowing key cursor made every mutation through the value cursor a
       WO-E303 against the container's own borrow. Any other element type is
       still a borrow: records are not copied. *)
    let cursor (n : string) (t : Ast.field_ty) : local =
      let copied =
        match unwrap_nullable t with
        | Scalar cn -> Types.is_heap_scalar cn
        | _ -> false
      in
      if copied then
        { l_name = n; l_ty = t; l_class = Owned; l_node = s.s_id; l_pos = s.s_pos; l_holds = true;
          l_src = None; l_bkind = AShared; l_state = Live }
      else
        { l_name = n; l_ty = t; l_class = oclass_of ctx t; l_node = s.s_id; l_pos = s.s_pos;
          l_holds = false; l_src = src; l_bkind = AShared; l_state = Borrowed s.s_pos }
    in
    ctx.loop_stack <- s.s_id :: ctx.loop_stack;
    fixpoint ctx
      (fun () ->
        push_scope ctx ~node:s.s_id ~pos:s.s_pos ~label:"FOR";
        (* the cursor borrows an element of the iterable for the whole
           body: moving the container out from under it is E302 *)
        (match (key_ty, var2) with
        | Some kt, Some v2 ->
          declare ctx (cursor var kt);
          declare ctx (cursor v2 item_ty)
        | _ -> declare ctx (cursor var item_ty));
        List.iter (analyze_stmt ctx) body;
        pop_scope ctx);
    ctx.loop_stack <- List.tl ctx.loop_stack
  | DoWhile { body; cond } ->
    (* `do { body } while cond` — body always runs before the condition
       is ever consulted, so it is analyzed first; still wrapped in
       `fixpoint` for the same reason `while`/`for` are (a *second*
       iteration's move state must be joined against the first's, the
       brief's own loop rule — see fixpoint's doc comment). *)
    ctx.loop_stack <- s.s_id :: ctx.loop_stack;
    fixpoint ctx
      (fun () ->
        analyze_block ctx ~node:s.s_id ~pos:s.s_pos ~label:"DO" body;
        read_expr ctx cond);
    ctx.loop_stack <- List.tl ctx.loop_stack
  | Break -> analyze_break ctx s
  | Continue -> analyze_continue ctx s

(* The brief's loop rule: probe the body once with recording off, join
   that with the entry state, then analyze for real against the joined
   state. The exit state joins the entry again — a `while` may run zero
   times — and a `return` inside the body does not make the loop diverge
   for the same reason. *)
and fixpoint (ctx : ctx) (run : unit -> unit) : unit =
  let entry = snapshot ctx in
  let div0 = ctx.diverged in
  let was_recording = ctx.recording in
  ctx.recording <- false;
  run ();
  ctx.recording <- was_recording;
  ctx.diverged <- div0;
  restore (join entry (snapshot ctx));
  run ();
  let after = snapshot ctx in
  restore (join entry after);
  ctx.diverged <- div0

(* `~pre` (haxe-parity Task 4): locals to declare into the fresh scope
   before its statements run — a payload pattern's bindings, and nothing
   else today. Borrows only (l_holds = false), so pop_scope's drop
   recording never sees them; every pre-existing call site passes
   nothing and is byte-identical. *)
(* haxe-parity Task 5: `try body catch (e) handler`. The two arms are
   alternate flows joining at one point — the same shape `if`/`switch`
   already have, so the same join machinery applies: whatever one arm
   moved out, the arm that still holds it drops at its own end
   (branch_join_drops), and the state after the whole expression is the
   join of both.

   What is genuinely different from a branch is WHERE the catch arm starts
   from: a trap can be raised anywhere inside the body, so the handler may
   run after *any prefix* of it. Taking the entry state as the handler's
   starting point is the conservative reading — it never claims the body's
   moves happened — and the join then makes the surviving path responsible
   for the drop. The frame also needs a live mask at the try itself (like
   every other trap-capable site): that mask is what the VM's unwind uses
   to release the body's own values before landing in the handler.

   `e` is a local of the predeclared `Error` record, owned by the handler
   scope (the record and its two Texts are freshly allocated at landing),
   so pop_scope records its drop like any other owned local's. *)
and analyze_try (ctx : ctx) (e : Ast.expr) (body : Ast.expr) (ename : string)
    (handler : Ast.stmt list) : unit =
  record_drop ctx ~node:e.id ~pos:e.pos ~kind:DLiveMask ~items:(mask_items (live_holders ctx));
  let entry = snapshot ctx in
  let div0 = ctx.diverged in
  read_expr ctx body;
  let body_sn = snapshot ctx in
  let body_div = ctx.diverged in
  restore entry;
  ctx.diverged <- div0;
  let ety = Ast.Scalar Types.error_record_name in
  let ebind =
    { l_name = ename; l_ty = ety; l_class = oclass_of ctx ety; l_node = e.id; l_pos = e.pos;
      l_holds = (match oclass_of ctx ety with Copy -> false | _ -> true); l_src = None;
      l_bkind = AShared; l_state = Live }
  in
  analyze_block ctx ~pre:[ ebind ] ~node:e.id ~pos:e.pos ~label:"CATCH" handler;
  let catch_sn = snapshot ctx in
  let catch_div = ctx.diverged in
  if (not body_div) && not catch_div then begin
    branch_join_drops ctx ~node:e.id ~label:"TRYBODY" ~pos:e.pos ~moving:catch_sn ~other:body_sn;
    branch_join_drops ctx ~node:e.id ~label:"CATCHJOIN" ~pos:e.pos ~moving:body_sn ~other:catch_sn
  end;
  restore (if body_div then catch_sn else if catch_div then body_sn else join body_sn catch_sn);
  ctx.diverged <- body_div && catch_div

and analyze_block (ctx : ctx) ?(pre = []) ~node ~pos ~label (body : Ast.stmt list) : unit =
  push_scope ctx ~node ~pos ~label;
  List.iter (declare ctx) pre;
  List.iter (analyze_stmt ctx) body;
  pop_scope ctx

and analyze_let (ctx : ctx) (s : Ast.stmt) (name : string) (ty : Ast.field_ty option)
    (value : Ast.expr)
    : unit =
  read_expr ctx value;
  let vty =
    match ty with
    | Some t -> t
    | None -> ( match expr_ty ctx value with Some t -> t | None -> Scalar "Int")
  in
  let cls = oclass_of ctx vty in
  (* A container read that yields a Text is COPIED by the emitter — the fourth
     ownership boundary, and the one that keeps `let cl = headers["x"]` from
     borrowing the map for the rest of the scope (which then forbade moving
     that map into a record, WO-E302). For any other element type the read is
     still a borrow of the container: records are not copied. *)
  let reads_container =
    match value.Ast.kind with
    | Ast.Index _ -> true
    | Ast.Call ({ Ast.kind = Ast.Ident bn; _ }, _) -> borrowing_builtin bn
    | _ -> false
  in
  let copies_out = reads_container && (match unwrap_nullable vty with
                                       | Scalar n -> Types.is_heap_scalar n
                                       | _ -> false) in
  let vplace = if copies_out then None else place_of value in
  (* A `@gc` value read out of a container is neither a copy nor a new
     reference: the container holds the count, and the reader only looks. It
     must NOT take the Gc-alias path below (which records an RC_INC/RC_DEC
     pair) — doing so double-released the element and segfaulted both `gc/`
     fixtures. Reading it as a plain borrow of the container is what the pass
     did before container reads became places, now with the right type. *)
  let gc_container_read = reads_container && cls = Gc in
  let holds, src, state =
    match (cls, vplace) with
    | Gc, _ when gc_container_read -> (false, vplace, Borrowed s.s_pos)
    (* Binding a Text from a PLACE copies it — the same boundary rule as a
       field store, a container element, a loop cursor and a return. The
       source stays live and keeps its own value; this binding owns the copy
       and drops it at scope end. Without it `let range = part` MOVED `part`,
       and the next line's `index_of(part, "/")` was a use-after-move. *)
    | (Owned | Gc), Some p when stores_by_copy ctx p -> (true, None, Live)
    | Copy, _ -> (false, None, Live)
    | Owned, None -> (true, None, Live) (* fresh value: constructor or call result *)
    | Owned, Some p -> (
      (* a whole owned local moves; a projection binds a *borrow* of that
         place instead — milestone 1 has no partial moves, so
         `let r = b.inner` is an alias, not a transfer *)
      match root_local ctx p with
      | None -> (true, None, Live) (* root is not a local: treat as a fresh value *)
      | Some l -> (
        match l.l_state with
        | Moved _ -> (true, None, Live) (* use-after-move already reported *)
        | Borrowed _ -> (false, Some p, Borrowed s.s_pos) (* reborrow *)
        | Live ->
          if p.projs = [] && l.l_holds then begin
            if transfer ctx p ~what:"cannot be moved out" then record_move ctx p MvLet;
            (true, None, Live)
          end
          else (false, Some p, Borrowed s.s_pos)))
    | Gc, None -> (true, None, Live) (* traced: no bookkeeping (7b) *)
    | Gc, Some p -> (true, Some p, Live)
  in
  declare ctx
    { l_name = name; l_ty = vty; l_class = cls; l_node = s.s_id; l_pos = s.s_pos; l_holds = holds;
      l_src = src; l_bkind = AShared; l_state = state }

and analyze_assign (ctx : ctx) (s : Ast.stmt) (target : Ast.expr) (value : Ast.expr) : unit =
  read_expr ctx value;
  let tplace = place_of target in
  let vplace = place_of value in
  (* `a = a` (or `self.box = self.box`) replaces a place with itself: the
     old value and the new one are the same storage, so recording a drop for
     the "overwritten" value would destroy the value the assignment just
     stored — a double free with no diagnostic. Overlap is deliberately a
     hair wider than the exact test (which would be `canon tp = canon vp`):
     it also holds when one side *contains* the other, as in `a = a.inner`.
     Such a shape cannot type-check upstream today, so the two tests agree
     on every reachable program; if containment ever became expressible,
     suppressing the drop would still be the safe direction — the
     overwritten value and the stored one overlap, so dropping either is
     the double free. *)
  let self_assign =
    match (tplace, vplace) with
    | Some tp, Some vp -> relate_places ctx tp vp = Overlap
    | _ -> false
  in
  (* Writing a place is an exclusive access to it (so it may not be
     borrowed elsewhere), and the value it held dies here. Table entries and
     the diagnostic point at the place written, not at the statement (they
     coincide today — an assignment starts at its target — but the place is
     what the entry is about). *)
  (match tplace with
  | None -> read_expr ctx target
  | Some p ->
    check_against_borrows ctx ~node:s.s_id ~pos:p.ppos p AExcl;
    if p.projs = [] then begin
      match root_local ctx p with
      | Some l when l.l_holds && l.l_state = Live && not self_assign ->
        if l.l_class = Owned then
          record_drop ctx ~node:s.s_id ~pos:p.ppos ~kind:DOverwrite
            ~items:[ { di_name = place_text p; di_kind = LOwned; di_node = p.pnode } ]
      | _ -> ()
    end
    else begin
      (* a projection target is also a read of its root *)
      use_place ctx p;
      read_place_parts ctx target;
      match if self_assign then Copy else place_class ctx p with
      | Owned ->
        record_drop ctx ~node:s.s_id ~pos:p.ppos ~kind:DOverwrite
            ~items:[ { di_name = place_text p; di_kind = LOwned; di_node = p.pnode } ]
      | Gc -> () (* traced: tracing owns the old value's lifetime (7b) *)
      | Copy -> ()
    end);
  (* the incoming value *)
  (match vplace with
  | None -> ()
  | Some vp ->
    let into_field = match tplace with Some tp -> tp.projs <> [] | None -> true in
    let what =
      if into_field then
        Printf.sprintf "cannot be stored in `%s`"
          (match tplace with Some tp -> place_text tp | None -> "a field")
      else "cannot be moved out"
    in
    if into_field && stores_by_copy ctx vp then () (* the field gets its own copy *)
    else if transfer ctx vp ~what then record_move ctx vp MvAssign);
  (* Whatever the value was — a fresh constructor, a call result, another
     local — assigning to a whole local re-initializes it: a local that had
     been moved out of is live again afterwards. *)
  match tplace with
  | Some tp when tp.projs = [] -> (
    match root_local ctx tp with Some l when l.l_holds -> l.l_state <- Live | _ -> ())
  | _ -> ()

and analyze_return (ctx : ctx) (s : Ast.stmt) (opt : Ast.expr option) : unit =
  (match opt with
  | None -> ()
  | Some e -> (
    read_expr ctx e;
    match place_of e with
    | None -> ()
    | Some p ->
      (* Returning a Text is the third ownership boundary that COPIES (the
         other two are a container element and a field): the caller gets its
         own string, the callee's borrow stays the borrow it was. emit.ml
         emits that copy. Without this the workload's own level classifier —
         `for lv in [...] { … return lv }` — cannot be written at all. *)
      if stores_by_copy ctx p then ()
      else if transfer ctx p ~what:(Printf.sprintf "escapes `%s`" ctx.fn_name) then
        record_move ctx p MvReturn));
  let live = live_holders ctx in
  record_drop ctx ~node:s.s_id ~pos:s.s_pos ~kind:DReturn ~items:(owned_items live);
  ctx.diverged <- true

(* haxe-parity Task 2: `break`/`continue` reuse analyze_return's own
   drop machinery verbatim, bounded to the nearest enclosing loop
   instead of the whole function (live_holders_upto vs. live_holders —
   see that function's doc comment) — this is "the one non-trivial bit"
   the brief calls out: an owned value still alive in the loop body at
   a `break`/`continue` gets its DROP recorded right here, at the jump,
   not left to leak. `ctx.diverged <- true` afterward mirrors
   analyze_return's own reasoning exactly: the rest of *this* block is
   unreachable, and the surrounding if/fixpoint machinery already knows
   how to fold that into a branch join or a loop's own entry/exit meet
   (the same normalization a `return` inside a loop or an `if` already
   gets, unchanged by this task). Outside any loop, `loop_stack` is
   empty and nothing is recorded — see that field's own doc comment for
   why emit.ml, not this pass, is the actual gate for that case. *)
and analyze_break (ctx : ctx) (s : Ast.stmt) : unit =
  (match ctx.loop_stack with
  | [] -> ()
  | loop_node :: _ ->
    let live = live_holders_upto ctx loop_node in
    record_drop ctx ~node:s.s_id ~pos:s.s_pos ~kind:DBreak ~items:(owned_items live));
  ctx.diverged <- true

and analyze_continue (ctx : ctx) (s : Ast.stmt) : unit =
  (match ctx.loop_stack with
  | [] -> ()
  | loop_node :: _ ->
    let live = live_holders_upto ctx loop_node in
    record_drop ctx ~node:s.s_id ~pos:s.s_pos ~kind:DContinue ~items:(owned_items live));
  ctx.diverged <- true

(* ============================================================
   Per-function driver
   ============================================================ *)

let param_local (ctx : ctx) (p : Ast.param) : local =
  let cls = oclass_of ctx p.ty in
  match cls with
  | Copy -> { l_name = p.name; l_ty = p.ty; l_class = Copy; l_node = p.id; l_pos = p.pos;
              l_holds = false; l_src = None; l_bkind = AShared; l_state = Live }
  | Owned | Gc -> (
    match p.conv with
    | Take ->
      { l_name = p.name; l_ty = p.ty; l_class = cls; l_node = p.id; l_pos = p.pos; l_holds = true;
        l_src = None; l_bkind = AShared; l_state = Live }
    | Borrow | Mut ->
      (* @gc parameters are aliases, not borrows: no borrow rules apply,
         and the caller's reference keeps the object alive for the call *)
      let state = if cls = Gc then Live else Borrowed p.pos in
      { l_name = p.name; l_ty = p.ty; l_class = cls; l_node = p.id; l_pos = p.pos; l_holds = false;
        l_src = None; l_bkind = (if p.conv = Mut then AExcl else AShared); l_state = state })

let analyze_fn ~(file : string) ?(promote : (string -> unit) option = None)
    (syms : Types.symbols) (coll : Diag.Collector.t) (sink : sink)
    ~(self_class : string option) (m : Ast.method_decl) : unit =
  let ctx =
    { file; syms; coll; sink; fn_name = m.name; scopes = []; loop_stack = []; recording = true;
      diverged = false; promote }
  in
  push_scope ctx ~node:m.id ~pos:m.pos ~label:"BODY";
  (* `self` is always a borrow (spec rule 6) *)
  (match self_class with
  | None -> ()
  | Some cn ->
    let ty = Scalar cn in
    let cls = oclass_of ctx ty in
    declare ctx
      { l_name = "self"; l_ty = ty; l_class = cls; l_node = m.id; l_pos = m.pos; l_holds = false;
        l_src = None; l_bkind = (if body_writes_self m.body then AExcl else AShared);
        l_state = (if cls = Gc then Live else Borrowed m.pos) });
  List.iter (fun p -> declare ctx (param_local ctx p)) m.params;
  List.iter (analyze_stmt ctx) m.body;
  pop_scope ctx

(* ============================================================
   Entry point
   ============================================================ *)

let pos_key (p : Ast.pos) = (p.line, p.col)

let analyze ~(file : string) ?(promote : (string -> unit) option = None)
    (prog : Ast.program) (syms : Types.symbols) (coll : Diag.Collector.t) : tables =
  let sink = { s_moves = []; s_drops = []; s_res = [] } in
  List.iter
    (function
      | Ast.Class c ->
        List.iter
          (fun m -> analyze_fn ~file ~promote syms coll sink ~self_class:(Some c.name) m)
          c.methods
      | Ast.Interface _ -> ()
      | Ast.Fn f -> analyze_fn ~file ~promote syms coll sink ~self_class:None f
      | Ast.Use _ -> ()
      | Ast.Const _ -> ()
      | Ast.Union _ -> () (* haxe-parity Task 4: no bodies to analyze *))
    prog.decls;
  (* Source order: sort by position, stably, so entries sharing a
     position keep the order the walk produced. Node ids can *not* be
     used for ordering (ast.ml: expression ids are unique but not
     parent-before-child). *)
  let sort_by key l = List.stable_sort (fun a b -> compare (key a) (key b)) (List.rev l) in
  { moves = sort_by (fun m -> pos_key m.mv_pos) sink.s_moves;
    drops = sort_by (fun d -> pos_key d.dr_pos) sink.s_drops;
    residuals = sort_by (fun r -> pos_key r.rs_pos) sink.s_res }
