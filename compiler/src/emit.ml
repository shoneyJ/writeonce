(* emit.ml — the `.wob` bytecode emitter (plan 3, Task 1).

   Lowers the parsed, typechecked, owner-annotated program into a `.wob`
   v1 image. Two documents govern every byte produced here and neither
   is negotiable from this file:

     - docs/plan/oop-vm/00-wob-format.md — the normative format
     - runtime/src/wob.h                 — its machine-readable twin

   The round-trip rule from the plan's global constraints: an image
   `woc` produces that `wovm`'s loader (runtime/src/loader.c) rejects is
   always an emitter bug. Everything the loader validates is therefore
   maintained as an invariant here — register counts 1..64 with
   args <= registers, every static register operand < the method's
   register count, constant/class/callee/slot indexes in range, CALL
   argument windows inside the caller's frame, jump targets inside the
   code, a terminator as the last instruction, builtin ids with their
   fixed arity, and strictly ascending line/drop tables whose masks fit
   the register count.

   ---- what this module consumes rather than re-derives ----------------

   Types.symbols          declarations, per-class field lists
   Types.wob_kind_of_typ   the .wob field kind of a declared type
   Owner.tables            the four ownership tables, verbatim:
                             moves     -> which MOVEs are real transfers
                             drops     -> DROP placement + drop-table masks
                             rcs       -> RC_INC / RC_DEC, minus ELIDED pairs
                             residuals -> the ONLY places borrow ops appear

   Where the drop map is synced is worth stating once: the owner table is
   authoritative at every node it records a LIVE-MASK for, which is every
   Call expression and every DbStub — so those sync from the table. An
   `Index` read, a `map` element write and a `for` cursor step lower to
   BUILTIN instructions that are NOT Call nodes, so no table entry exists
   to sync to; they run on the emitter's running mask, which is exactly
   right for them.

   Residual regions are anchored by owner.ml on more than one node kind
   (a call expression, an assignment statement, a moved place), so each
   consumption site marks the region used and anything left over at the
   end of the unit is WO-E404 — a region nobody wrapped would ship the
   aliasing check silently disabled, which is the one failure a residual
   site exists to prevent.

   Two obligations dump.ml states for the tables and this file honors:
   the residual table may name the same canonical operand in several
   entries, so borrow guards are coalesced *per operand* (one acquire /
   release pair per operand register, strongest access kind winning) —
   emitting a pair per table entry would ask for both an exclusive and a
   shared borrow of one object and self-trap on legal code; and the
   conditional-move drop rule (JOIN-DROP) is already normalized in the
   table, so its entries are emitted as given.

   ---- register allocation --------------------------------------------

   One scope-stack allocator per method, three tiers:

     r0            `self`, for a class method (the VM's window
                   convention: ICALL leaves the receiver in the callee's
                   r0, see runtime/src/vm.c's ICALL case)
     next          parameters, in declaration order — `arg_cnt` is
                   therefore (self ? 1 : 0) + parameter count
     then          locals, allocated on declaration (`let`, and a
                   `for`'s cursor plus its three loop-carried slots),
                   released when their block ends
     above those   expression temporaries from a high-water pool, reset
                   to the local watermark at every statement boundary

   Temporaries are a bump allocator, which is what makes the Lua-style
   call window safe: a call's window sits at the current top, so every
   register the callee's frame overlaps (it zeroes r[argc..regc) at
   entry) is already dead. Argument slots are reserved *before* any
   argument is evaluated, so a nested call inside argument i cannot
   clobber an already-filled slot of the outer window.

   A method needing more than 64 registers is WO-E401 — a diagnostic,
   never a truncation.

   ---- what the front end does not supply -----------------------------

   Nothing in plan 2 exports a per-expression type table (types.ml's
   pass 2 returns unit), so this file carries a small local type
   resolver (`ty_of_expr`) for the four decisions that need one: EQ vs
   EQS, direct CALL vs ICALL, which container builtin an `Index` or a
   `get`/`set` names, and the element/key kinds a fresh container is
   created with. It resolves types the same way owner.ml's own `expr_ty`
   does; it does not re-derive ownership, kinds, or any table. *)

open Ast

module SM = Map.Make (String)

(* ============================================================
   Diagnostics — WO-E4xx, the emitter's reserved range (diag.ml)
   ============================================================ *)

(* WO-E401 — the method needs more registers than the VM's 64-slot
   window allows. The spec's register budget is a hard format limit
   (runtime/src/wob.h WO_MAX_REGS), so this is a diagnostic and the
   method is never silently truncated. *)
let over_budget_code = Diag.emitter_prefix ^ "01"

(* WO-E402 — a value that does not fit the instruction encoding: more
   than 65536 constants/classes/methods/interface slots (LOADK, NEW,
   CALL and ICALL carry a 16-bit operand), a field index above 255
   (GETF/SETF carry a byte), or a jump farther than the signed 16-bit
   displacement. Same doctrine as WO-E401: name the limit, emit
   nothing. *)
let limit_code = Diag.emitter_prefix ^ "02"

(* WO-E403 — a construct the milestone-1 instruction set cannot express
   (an unresolved name, an element write into a `multi`, iterating a
   `map`). The front end accepts more surface than the VM implements —
   see the scope fence in the plan — and the emitter's job at that
   boundary is to say so, not to invent bytecode. *)
let cannot_lower_code = Diag.emitter_prefix ^ "03"

(* WO-E404 — a residual borrow site whose operand the emitter cannot tie
   to a live register at the guarded region. Emitting the region
   unguarded would drop the one enforcement the residual site exists for
   (a real aliasing violation would go unchecked), so this fails loudly
   instead. *)
let unguardable_code = Diag.emitter_prefix ^ "04"

(* WO-E405 — the program entry (the zero-arg free fn `main` selected
   below) declares a return type other than `Int`. The systems-track
   spec (docs/superpowers/specs/2026-08-01-systems-track-design.md:70)
   is explicit that a free `fn main(args) -> Int` compiles as a program
   *because* the return value is the exit code — so an entry returning
   a class was never legal, it just wasn't checked. Left unchecked, a
   `@gc` return escapes into runtime/src/main.c's `uint64_t ret`, which
   the driver has no way to release (the .wob method table carries no
   return kind), inflating that object's refcount permanently — the
   exact leak this diagnostic closes off at the source instead of in
   the runtime. `main` with no return annotation at all is unaffected
   (nothing declared, nothing to contradict `Int`). *)
let entry_return_code = Diag.emitter_prefix ^ "05"

(* WO-E406 (haxe-parity Task 1, modules) — a call through a reserved
   stdlib alias (`use fs`/`proc`/`net`/`time`/`json`/`env`) survived
   typechecking (types.ml accepts it as UNKNOWN-BUT-RESERVED: no
   E207/E225/arity check, since the six namespaces' members arrive in
   plan 9) and reached emission. There is nothing to lower it to yet —
   no signature, no builtin id — so this is the one place that still
   says so, and only if such a call actually survives this far; `use fs`
   declared and never called never reaches this code at all. *)
let stdlib_not_linked_code = Diag.emitter_prefix ^ "06"

(* ============================================================
   Format constants (mirror of runtime/src/wob.h — never diverge)
   ============================================================ *)

let wob_magic = 0x31424F57 (* "WOB1" read as an LE u32 *)
let wob_version = 2 (* v2: per-field class-table metadata *)
let wob_hdr_size = 44
let wob_none = 0xFFFFFFFF
let k_int = 0
let k_text = 1
let max_regs = 64
let classf_gc = 0x01

let op_nop = 0
let op_loadk = 1
let op_move = 2
let op_add = 3
let op_sub = 4
let op_mul = 5
let op_div = 6
let op_neg = 7
let op_concat = 8
let op_eq = 9
let op_lt = 10
let op_le = 11
let op_eqs = 12
let op_jmp = 13
let op_jz = 14
let op_call = 15
let op_icall = 16
let op_ret = 17
let op_ret0 = 18
let op_new = 19
let op_getf = 20
let op_setf = 21
let op_drop = 22
let op_borrow_s = 23
let op_borrow_x = 24
let op_release_s = 25
let op_release_x = 26
let op_rc_inc = 27
let op_rc_dec = 28
let op_builtin = 29
let op_db_stub = 30

(* haxe-parity Task 5: try/catch (runtime/src/wob.h's WOP_TRY/WOP_ENDTRY) *)
let op_try = 32
let op_endtry = 33

let b_now = 0
let b_print = 1
let b_print_int = 2
let b_words = 3
let b_multi_new = 4
let b_multi_push = 5
let b_multi_get = 6
let b_count = 7
let b_latest = 8
let b_map_new = 9
let b_map_set = 10
let b_map_get = 11
let b_map_has = 12
(* haxe-parity Task 2: the one fenced VM addition this task takes —
   string interpolation's Int-to-Text conversion (`"${count} lines"`).
   runtime/src/wob.h WO_B_INT_TO_TEXT = 13. *)
let b_int_to_text = 13

(* haxe-parity Task 4: the one fenced VM addition of the enum-payload
   work — reads a variant object's tag (the object header's own
   class_id; docs/plan/oop-vm/00-wob-format.md "enum payload variants")
   so a switch over a payload union can compare tags without a per-arm
   allocation. runtime/src/wob.h WO_B_VARIANT_TAG = 14. Compiler-
   internal: never a source-callable name (not in is_builtin_name /
   types.ml's builtin_signatures — 08-builtin-surface.md is unchanged). *)
let b_variant_tag = 14

(* haxe-parity Task 5: fills the catch arm's freshly allocated `Error`
   record from the trap the VM landed with (field order 0 code, 1 line,
   2 method, 3 msg — Types.error_record_fields). runtime/src/wob.h
   WO_B_ERR_FILL = 15. Compiler-internal, like b_variant_tag: never a
   source-callable name. *)
let b_err_fill = 15

(* systems stdlib (runtime/src/wob.h WO_B_LEN..WO_B_MAP_VAL_AT) *)
let b_len = 16
let b_byte_at = 17
let b_print_err = 18
let b_starts_with = 19
let b_ends_with = 20
let b_index_of = 21
let b_last_index_of = 22
let b_substr = 23
let b_trim = 24
let b_to_lower = 25
let b_char_of = 26
let b_parse_int = 27
let b_split = 28
let b_split_ws = 29
let b_join = 30
let b_slice = 31
let b_pop = 32
let b_shift = 33
let b_sort = 34
let b_reverse = 35
let b_map_remove = 36
let b_map_key_at = 37
let b_map_val_at = 38
let b_multi_set = 39
let b_map_get_opt = 59

(* json (runtime/src/json.c): encode takes the value's static kind as its
   second argument, decode the class id to build as its second. *)
let b_json_encode = 57
let b_json_decode = 58

let ins_abc op a b c = op lor (a lsl 8) lor (b lsl 16) lor (c lsl 24)
let ins_abx op a bx = op lor (a lsl 8) lor (bx lsl 16)
let ins_asbx op a sbx = ins_abx op a (sbx + 32768)

(* ============================================================
   Byte buffer
   ============================================================ *)

module Buf = struct
  type t = {
    mutable b : Bytes.t;
    mutable len : int;
  }

  let create () = { b = Bytes.create 1024; len = 0 }

  let room (t : t) (n : int) : unit =
    if t.len + n > Bytes.length t.b then begin
      let cap = ref (Bytes.length t.b) in
      while t.len + n > !cap do
        cap := !cap * 2
      done;
      let nb = Bytes.create !cap in
      Bytes.blit t.b 0 nb 0 t.len;
      t.b <- nb
    end

  let u8 (t : t) (v : int) : unit =
    room t 1;
    Bytes.set_uint8 t.b t.len (v land 0xFF);
    t.len <- t.len + 1

  let u16 (t : t) (v : int) : unit =
    room t 2;
    Bytes.set_uint16_le t.b t.len (v land 0xFFFF);
    t.len <- t.len + 2

  (* u32 via Int32: 0xFFFFFFFF (WOB_NONE) truncates to -1l, whose
     little-endian bytes are FF FF FF FF — exactly what the loader
     compares against. *)
  let u32 (t : t) (v : int) : unit =
    room t 4;
    Bytes.set_int32_le t.b t.len (Int32.of_int v);
    t.len <- t.len + 4

  let i64 (t : t) (v : int64) : unit =
    room t 8;
    Bytes.set_int64_le t.b t.len v;
    t.len <- t.len + 8

  let str (t : t) (s : string) : unit =
    let n = String.length s in
    room t n;
    Bytes.blit_string s 0 t.b t.len n;
    t.len <- t.len + n

  let contents (t : t) : string = Bytes.sub_string t.b 0 t.len
end

(* growable instruction array (jumps are patched after the fact) *)
type code = {
  mutable a : int array;
  mutable n : int;
}

let code_create () = { a = Array.make 64 0; n = 0 }

let code_push (c : code) (v : int) : unit =
  if c.n = Array.length c.a then begin
    let na = Array.make (2 * c.n) 0 in
    Array.blit c.a 0 na 0 c.n;
    c.a <- na
  end;
  c.a.(c.n) <- v;
  c.n <- c.n + 1

(* ============================================================
   Program-level tables
   ============================================================ *)

type clsrec = {
  cr_name : string;
  cr_gc : bool;
  cr_fields : (string * Ast.field_ty) array;
  cr_methods : string list; (* method names, declaration order *)
}

type ifacerec = {
  ir_name : string;
  ir_slot_base : int;
  ir_methods : (string * int) list; (* name, parameter count *)
}

type methrec = {
  mr_name : string;
  mr_class : int option;
  mr_argc : int;
  mutable mr_regc : int;
  mutable mr_code : int array;
  mutable mr_lines : (int * int) list;
  mutable mr_drops : (int * int64 * int64) list;
}

(* one input unit: a discovered file with its own program and its own
   owner tables. Node ids are minted per parse, so they are unique
   within a file and NOT across files — every side table keyed on a node
   id is therefore built per unit. *)
type input = {
  file : string;
  prog : Ast.program;
  tables : Owner.tables;
}

type pctx = {
  p_syms : Types.symbols;
  p_coll : Diag.Collector.t;
  p_classes : clsrec array;
  p_class_id : int SM.t;
  p_ifaces : ifacerec array;
  p_iface_id : int SM.t;
  (* method index by ("Class.method") for methods and ("name") for free
     fns — free fns and classes share a namespace only through this
     lookup, never in the emitted table *)
  p_method_id : int SM.t;
  p_methods : methrec array;
  (* haxe-parity Task 1 (modules): file -> that file's own `use` edges.
     Front-door module visibility is already fully decided by
     types.ml's check_modules before the emitter ever runs — this
     exists only for the two things emission itself still needs to
     know: (1) a stdlib-reserved alias reaching a real call site here
     is WO-E406 (nothing to lower it to, see that code's doc comment),
     and (2) which alias names a project module at all (as opposed to a
     receiver expression), so a qualified call can be resolved against
     that module's own symbols below rather than p_syms' flat merge. *)
  p_uses : (string, Types.use_edge list) Hashtbl.t;
  (* haxe-parity Task 1 (modules), CRITICAL 1 review fix: p_syms.free_fns
     is one flat table, first-wins merged across the *whole* discovered
     tree regardless of module (main.ml's merge_symbols, unchanged by
     this task) — exactly right for `main`/every other pre-Task-1
     lookup, and exactly wrong the instant two different modules declare
     a same-named `pub fn` and a qualified call means to pick between
     them: the flat merge already silently dropped one, so looking a
     qualified call's callee up there can return the *wrong module's*
     body no matter how carefully the call site names its alias. A
     qualified call therefore resolves through *this* table instead —
     module id -> that module's own, unmerged-with-anyone-else `symbols`
     (built by Types.module_symbols, the same per-module grouping
     types.ml's own resolver uses) — never through p_syms for that one
     purpose. `p_module_of` (file -> module id) is what turns a bare
     call's *own* file into the same key, so an own-module bare call to
     a name that happens to collide with some other module's same-named
     `pub fn` resolves correctly too (see free_fn_key's doc comment). *)
  p_module_syms : (string, Types.symbols) Hashtbl.t;
  p_module_of : string -> string;
  (* every free-fn name declared by more than one distinct module — see
     free_fn_key's doc comment. *)
  p_colliding : (string, unit) Hashtbl.t;
  (* constant pool, deduplicated *)
  p_kints : (int, int) Hashtbl.t;
  p_ktexts : (string, int) Hashtbl.t;
  mutable p_consts : [ `Int of int | `Text of string ] list; (* rev *)
  mutable p_nconsts : int;
}

let const_int (p : pctx) (v : int) : int =
  match Hashtbl.find_opt p.p_kints v with
  | Some i -> i
  | None ->
    let i = p.p_nconsts in
    Hashtbl.replace p.p_kints v i;
    p.p_consts <- `Int v :: p.p_consts;
    p.p_nconsts <- i + 1;
    i

let const_text (p : pctx) (s : string) : int =
  match Hashtbl.find_opt p.p_ktexts s with
  | Some i -> i
  | None ->
    let i = p.p_nconsts in
    Hashtbl.replace p.p_ktexts s i;
    p.p_consts <- `Text s :: p.p_consts;
    p.p_nconsts <- i + 1;
    i

(* ============================================================
   Per-method lowering state
   ============================================================ *)

(* haxe-parity Task 2: one enclosing loop's own backpatch lists.
   `break`/`continue` inside the loop's body emit a blind `JMP 0` at
   their own site (their own drops already ran, from Owner's
   v_break/v_continue tables) and record that instruction's pc here;
   the loop that pushed this frame patches every recorded pc once it
   knows the real target — `lf_breaks` always to "right after the whole
   loop" (same target `JZ`'s own exit uses), `lf_continues` to wherever
   *that* loop shape re-enters its own condition check (`while`: the
   top; `for`: right before the increment; `do...while`: right before
   the condition). *)
type loop_frame = {
  lf_node : int;
  mutable lf_breaks : int list;
  mutable lf_continues : int list;
}

type fstate = {
  f_file : string;
  f_fn : string;
  (* the enclosing method's declared return type — what gives a contextual
     value in tail position its type (`return []` / `return nil` /
     `return {}`), the same role a `let`'s annotation plays *)
  f_ret : Ast.field_ty option;
  f_code : code;
  mutable f_cur_line : int; (* line of the construct being lowered *)
  mutable f_line : int; (* last line written to the table *)
  mutable f_lines : (int * int) list; (* rev *)
  mutable f_owned : int64; (* running owned-register mask *)
  mutable f_gc : int64; (* running @gc-register mask *)
  mutable f_last_owned : int64;
  mutable f_last_gc : int64;
  mutable f_drops : (int * int64 * int64) list; (* rev *)
  mutable f_nlocals : int;
  mutable f_temp : int;
  mutable f_max : int;
  mutable f_env : (string * (int * Ast.field_ty)) list; (* innermost first *)
  f_decl : (int, int) Hashtbl.t; (* declaring node id -> register *)
  (* declaring nodes in declaration order, innermost last — a stack, so
     the nodes a block declared are the ones pushed since it opened.
     Needed because owner.ml anchors a scope's @gc releases on the
     scope's own node, and an `if`'s THEN and ELSE scopes share that
     node: which release belongs to which arm is answered by which
     block declared the local. *)
  mutable f_declared : int list;
  f_node : (int, int) Hashtbl.t; (* expression node id -> register *)
  (* register -> the kind of holder living in it, for the one case the
     declaration site cannot answer: a whole-local assignment
     re-initializes a local that had been moved out of, so its bit goes
     back into the live mask *)
  f_kind : (int, Owner.local_kind) Hashtbl.t;
  (* this path has returned, so it contributes no state to a merge —
     the same rule owner.ml's own `diverged` follows, for the same
     reason: a branch that returned never reaches the join, and letting
     its (empty) live set intersect the other branch's would strip a
     still-live register out of the drop map *)
  mutable f_div : bool;
  (* the farthest pc any jump was patched to. A forward jump out of the
     last `if`/`while` of a body targets the position *after* the last
     instruction, which the loader reads as "jump out of code" — so the
     implicit return has to be appended for that reason too, not only
     when the last instruction is not a terminator. *)
  mutable f_maxjmp : int;
  mutable f_over : bool; (* WO-E401 already reported for this method *)
  (* haxe-parity Task 2: innermost-first stack of enclosing loop
     backpatch frames — see loop_frame's own doc comment. Empty outside
     any loop, which is exactly how emit_break/emit_continue detect a
     `break`/`continue` with no legal target (WO-E403, "cannot lower" —
     the same convention as every other construct nothing upstream
     tracks loop nesting to reject earlier). *)
  mutable f_loops : loop_frame list;
}

(* ---- per-unit views of the four owner tables ---- *)

type views = {
  v_move : (int, string) Hashtbl.t; (* place-expr node -> moved place text *)
  v_scope : (int * string, Owner.drop_item list) Hashtbl.t;
  v_join : (int * string, Owner.drop_item list) Hashtbl.t;
  v_return : (int, Owner.drop_item list) Hashtbl.t;
  (* haxe-parity Task 2: DBreak/DContinue's own views, by the
     break/continue statement's own node id — mirrors v_return exactly,
     just bounded to the enclosing loop instead of the whole function
     (owner.ml's live_holders_upto). *)
  v_break : (int, Owner.drop_item list) Hashtbl.t;
  v_continue : (int, Owner.drop_item list) Hashtbl.t;
  v_overwrite : (int, unit) Hashtbl.t;
  v_mask : (int, Owner.drop_item list) Hashtbl.t;
  (* holder decl nodes: every declaring node the DROPS table ever names.
     This is how the emitter learns which locals the frame destroys
     without re-deriving owner.ml's own "holds" decision. *)
  v_holder : (int, Owner.local_kind) Hashtbl.t;
  v_rc : (int, Owner.rc_site list) Hashtbl.t; (* by rc_node *)
  (* region node -> the region's position and its per-operand coalesced
     guards *)
  v_res : (int, Ast.pos * (int * Owner.acc_kind) list) Hashtbl.t;
  (* regions the emitter actually wrapped. owner.ml anchors a residual
     region on four different node kinds (a call expression, an
     assignment statement, a moved place); a region nobody consumed
     would silently ship unguarded, which is the one failure mode a
     residual site exists to prevent — so what is left over at the end of
     the unit is WO-E404, never silence. *)
  v_res_used : (int, unit) Hashtbl.t;
}

let build_views (t : Owner.tables) : views =
  let v =
    { v_move = Hashtbl.create 16; v_scope = Hashtbl.create 16; v_join = Hashtbl.create 16;
      v_return = Hashtbl.create 16; v_break = Hashtbl.create 16; v_continue = Hashtbl.create 16;
      v_overwrite = Hashtbl.create 16; v_mask = Hashtbl.create 16;
      v_holder = Hashtbl.create 16; v_rc = Hashtbl.create 16; v_res = Hashtbl.create 16;
      v_res_used = Hashtbl.create 16 }
  in
  List.iter
    (fun (m : Owner.move_site) -> Hashtbl.replace v.v_move m.Owner.mv_node m.Owner.mv_place)
    t.Owner.moves;
  List.iter
    (fun (d : Owner.drop_site) ->
      let items = d.Owner.dr_items in
      List.iter
        (fun (i : Owner.drop_item) -> Hashtbl.replace v.v_holder i.Owner.di_node i.Owner.di_kind)
        items;
      match d.Owner.dr_kind with
      | Owner.DScope label -> Hashtbl.replace v.v_scope (d.Owner.dr_node, label) items
      | Owner.DBranchJoin label -> Hashtbl.replace v.v_join (d.Owner.dr_node, label) items
      | Owner.DReturn -> Hashtbl.replace v.v_return d.Owner.dr_node items
      | Owner.DBreak -> Hashtbl.replace v.v_break d.Owner.dr_node items
      | Owner.DContinue -> Hashtbl.replace v.v_continue d.Owner.dr_node items
      | Owner.DOverwrite -> Hashtbl.replace v.v_overwrite d.Owner.dr_node ()
      | Owner.DLiveMask -> Hashtbl.replace v.v_mask d.Owner.dr_node items)
    t.Owner.drops;
  List.iter
    (fun (r : Owner.rc_site) ->
      let prev = try Hashtbl.find v.v_rc r.Owner.rc_node with Not_found -> [] in
      Hashtbl.replace v.v_rc r.Owner.rc_node (prev @ [ r ]))
    t.Owner.rcs;
  (* Guard coalescing, per dump.ml's normative note: one entry per
     (region, operand) with the strongest access kind, never one pair per
     table entry. AExcl outranks AShared; a move is never a residual
     side. *)
  (* AMove is defensive: a move names a whole local, and relate answers
     Overlap or Disjoint for a place with no projections, so a move can
     never be a residual side (dump.ml says the same). If one ever
     appeared, an exclusive guard is the conservative reading. *)
  let stronger (a : Owner.acc_kind) (b : Owner.acc_kind) =
    match (a, b) with
    | Owner.AExcl, _ | _, Owner.AExcl -> Owner.AExcl
    | Owner.AMove, _ | _, Owner.AMove -> Owner.AExcl
    | Owner.AShared, Owner.AShared -> Owner.AShared
  in
  List.iter
    (fun (r : Owner.residual_site) ->
      let cur = try snd (Hashtbl.find v.v_res r.Owner.rs_node) with Not_found -> [] in
      let add acc (node, kind) =
        match List.assoc_opt node acc with
        | None -> acc @ [ (node, kind) ]
        | Some k0 -> List.map (fun (n, k) -> if n = node then (n, stronger k0 kind) else (n, k)) acc
      in
      let cur = add cur (r.Owner.rs_a_node, r.Owner.rs_a_kind) in
      let cur = add cur (r.Owner.rs_b_node, r.Owner.rs_b_kind) in
      Hashtbl.replace v.v_res r.Owner.rs_node (r.Owner.rs_pos, cur))
    t.Owner.residuals;
  v

(* ============================================================
   Diagnostic helpers
   ============================================================ *)

let err (p : pctx) ~code ~(file : string) ~(pos : Ast.pos) ~message : unit =
  Diag.Collector.add p.p_coll
    (Diag.error ~code ~file ~line:pos.line ~col:pos.col ~message ())

(* ============================================================
   Registers
   ============================================================ *)

let bump (f : fstate) (r : int) : unit = if r > f.f_max then f.f_max <- r

let over_budget (p : pctx) (f : fstate) (pos : Ast.pos) : unit =
  if not f.f_over then begin
    f.f_over <- true;
    err p ~code:over_budget_code ~file:f.f_file ~pos
      ~message:
        (Printf.sprintf
           "`%s` needs more than %d registers — the VM's register window is %d slots; split the \
            method or reduce the number of live locals"
           f.f_fn max_regs max_regs)
  end

(* Past the budget the allocation is clamped so the instruction encoding
   (one byte per register operand) stays well-formed: WO-E401 has already
   failed the compile, and a malformed instruction array would only
   crash the serializer on the way out. *)
let alloc_local (p : pctx) (f : fstate) (pos : Ast.pos) : int =
  let r = f.f_nlocals in
  if r >= max_regs then begin
    over_budget p f pos;
    max_regs - 1
  end
  else begin
    f.f_nlocals <- r + 1;
    if f.f_temp < f.f_nlocals then f.f_temp <- f.f_nlocals;
    bump f r;
    r
  end

let alloc_temp (p : pctx) (f : fstate) (pos : Ast.pos) : int =
  let r = f.f_temp in
  if r >= max_regs then begin
    over_budget p f pos;
    max_regs - 1
  end
  else begin
    f.f_temp <- r + 1;
    bump f r;
    r
  end

let alloc_temps (p : pctx) (f : fstate) (pos : Ast.pos) (n : int) : int =
  let base = f.f_temp in
  for _ = 1 to n do
    ignore (alloc_temp p f pos)
  done;
  if base >= max_regs then max_regs - 1 else base

let stmt_reset (f : fstate) : unit = f.f_temp <- f.f_nlocals

(* ============================================================
   Instruction emission (line + drop tables ride along)
   ============================================================ *)

let put (f : fstate) (ins : int) : unit =
  let pc = f.f_code.n in
  if f.f_cur_line <> f.f_line then begin
    f.f_lines <- (pc, f.f_cur_line) :: f.f_lines;
    f.f_line <- f.f_cur_line
  end;
  if f.f_owned <> f.f_last_owned || f.f_gc <> f.f_last_gc then begin
    f.f_drops <- (pc, f.f_owned, f.f_gc) :: f.f_drops;
    f.f_last_owned <- f.f_owned;
    f.f_last_gc <- f.f_gc
  end;
  code_push f.f_code ins

let here (f : fstate) : int = f.f_code.n

let patch_jump (p : pctx) (f : fstate) ~(file : string) ~(pos : Ast.pos) (at : int) (target : int)
    : unit =
  let d = target - (at + 1) in
  if d < -32768 || d > 32767 then
    err p ~code:limit_code ~file ~pos
      ~message:
        (Printf.sprintf "jump displacement %d does not fit the 16-bit signed field" d)
  else begin
    if target > f.f_maxjmp then f.f_maxjmp <- target;
    let ins = f.f_code.a.(at) in
    f.f_code.a.(at) <- ins_asbx (ins land 0xFF) ((ins lsr 8) land 0xFF) d
  end

(* ---- live-mask bookkeeping ---- *)

let mask_set (f : fstate) (kind : Owner.local_kind) (r : int) : unit =
  Hashtbl.replace f.f_kind r kind;
  let bit = Int64.shift_left 1L r in
  match kind with
  | Owner.LOwned -> f.f_owned <- Int64.logor f.f_owned bit
  | Owner.LGc -> f.f_gc <- Int64.logor f.f_gc bit

let mask_clear (f : fstate) (r : int) : unit =
  let bit = Int64.lognot (Int64.shift_left 1L r) in
  f.f_owned <- Int64.logand f.f_owned bit;
  f.f_gc <- Int64.logand f.f_gc bit

(* Control-flow merge: keep only what is live on *every* incoming path.
   Over-approximating liveness at a merge is the dangerous direction — a
   register the other path already moved out of would be dropped a second
   time during unwinding — so the merge intersects. *)
let mask_meet (f : fstate) (o : int64) (g : int64) : unit =
  f.f_owned <- Int64.logand f.f_owned o;
  f.f_gc <- Int64.logand f.f_gc g

(* ============================================================
   Types (the small local resolver — see this file's module doc)
   ============================================================ *)

let rec unwrap (ft : Ast.field_ty) : Ast.field_ty =
  match ft with Nullable inner -> unwrap inner | t -> t

let kind_byte : Types.wob_kind -> int = function
  | Types.WO_K_SCALAR -> 0
  | Types.WO_K_OWNED -> 1
  | Types.WO_K_GCREF -> 2
  | Types.WO_K_TEXT -> 3
  | Types.WO_K_MULTI -> 4
  | Types.WO_K_MAP -> 5
  (* `?T` has no kind byte of its own in the v1 format (kinds run 0..5;
     the loader rejects 6). It needs none: a nullable field stores what
     T stores and spells nil as 0, and every drop plan in
     runtime/src/gc.c already ignores a 0 slot for every kind. So the
     kind of `?T` is the kind of T — resolved by unwrapping before the
     call, this arm is unreachable. *)
  | Types.WO_K_NULLABLE -> 0

let field_kind (p : pctx) (ft : Ast.field_ty) : int =
  kind_byte (Types.wob_kind_of_typ p.p_syms (Types.typ_of_field_ty (unwrap ft)))

let class_of_name (p : pctx) (n : string) : int option = SM.find_opt n p.p_class_id

let field_of (p : pctx) (cid : int) (fname : string) : (int * Ast.field_ty) option =
  let fs = p.p_classes.(cid).cr_fields in
  let rec go i = if i >= Array.length fs then None else
    let n, ty = fs.(i) in
    if n = fname then Some (i, ty) else go (i + 1)
  in
  go 0

let free_fn (p : pctx) (n : string) : Types.free_fn_info option =
  Types.StringMap.find_opt n p.p_syms.Types.free_fns

(* haxe-parity Task 1 (modules): is `alias` one of file `f_file`'s own
   `use` edges? Returns the edge so the caller can tell stdlib apart
   from a project module (Types.use_edge.ue_is_stdlib). *)
let use_edge_for (p : pctx) ~(file : string) (alias : string) : Types.use_edge option =
  match Hashtbl.find_opt p.p_uses file with
  | None -> None
  | Some edges -> List.find_opt (fun (u : Types.use_edge) -> u.Types.ue_alias = alias) edges

(* haxe-parity Task 1 (modules), CRITICAL 1 review fix: computed once per
   `emit` call (below) and threaded through pctx (p_colliding) rather
   than kept as an `emit`-local, since free_fn_key (right below) is
   needed by emit_call, part of the emit_expr/emit_call mutually
   recursive group defined further down this file — a plain local to
   `emit` (itself defined at the bottom of the file) would not be in
   scope there. A name collides when more than one *distinct* module
   declares a free fn with that exact name; only possible now that
   modules exist at all (pre-Task-1, every discovered file was one flat
   namespace, so this table is always empty for any program that
   predates `use`). *)
let compute_colliding_fn_names ~(module_of : string -> string) (units : input list) :
    (string, unit) Hashtbl.t =
  let fn_modules : (string, (string, unit) Hashtbl.t) Hashtbl.t = Hashtbl.create 32 in
  List.iter
    (fun u ->
      List.iter
        (function
          | Ast.Fn (m : Ast.method_decl) ->
            let mid = module_of u.file in
            let mods =
              match Hashtbl.find_opt fn_modules m.name with
              | Some t -> t
              | None ->
                let t = Hashtbl.create 2 in
                Hashtbl.replace fn_modules m.name t;
                t
            in
            Hashtbl.replace mods mid ()
          | Ast.Class _ | Ast.Interface _ | Ast.Use _ | Ast.Const _ | Ast.Union _ -> ())
        u.prog.decls)
    units;
  let colliding : (string, unit) Hashtbl.t = Hashtbl.create 8 in
  Hashtbl.iter (fun name mods -> if Hashtbl.length mods >= 2 then Hashtbl.replace colliding name ()) fn_modules;
  colliding

(* A free fn's method-table key: its plain name, unless that name
   collides across modules, in which case it's `"<module>#<name>"` — the
   same composite-key idea class methods already use (`"Class.method"`).
   Every free-fn method-table key, on both the build side (emit's own
   pass 2, before `p : pctx` even exists yet — hence taking the raw
   `colliding` table rather than `p`) and the lookup side (emit_call, via
   `p.p_colliding`) goes through this now, so a non-colliding name
   (everything before this task, and the overwhelming majority after it)
   is completely unaffected — same plain key as always. *)
let free_fn_key (colliding : (string, unit) Hashtbl.t) (mid : string) (name : string) : string =
  if Hashtbl.mem colliding name then mid ^ "#" ^ name else name

let class_method (p : pctx) (cname : string) (m : string) : Types.method_info option =
  match Types.StringMap.find_opt cname p.p_syms.Types.classes with
  | None -> None
  | Some (c : Types.class_info) ->
    List.find_opt (fun (mi : Types.method_info) -> mi.Types.name = m) c.Types.methods

(* haxe-parity Task 7: the `static fn` behind `Flock.held(path)`. Kept
   separate from class_method so an instance method is never callable
   through a class name, and a static never through an instance. *)
let static_method (p : pctx) (cname : string) (m : string) : Types.method_info option =
  match class_method p cname m with Some mi when mi.Types.is_static -> Some mi | _ -> None

let iface_method (p : pctx) (iname : string) (m : string) : (int * Types.method_sig_info) option =
  match SM.find_opt iname p.p_iface_id with
  | None -> None
  | Some iid -> (
    let ir = p.p_ifaces.(iid) in
    let rec go i = function
      | [] -> None
      | (n, _) :: tl -> if n = m then Some (ir.ir_slot_base + i) else go (i + 1) tl
    in
    match go 0 ir.ir_methods with
    | None -> None
    | Some slot -> (
      match Types.StringMap.find_opt iname p.p_syms.Types.interfaces with
      | None -> None
      | Some (ii : Types.interface_info) -> (
        match List.find_opt (fun (s : Types.method_sig_info) -> s.Types.name = m) ii.Types.methods with
        | None -> None
        | Some sg -> Some (slot, sg))))

(* Types.typ -> this file's own Ast.field_ty view. Needed for the one table
   that is stated in the typechecker's language and consumed here: the
   systems stdlib's declared return shapes (Types.stdlib_members). Container
   element types beyond one scalar level cannot be spelled as a field_ty
   (`Multi of string`), so a nested container yields None — nothing in the
   stdlib returns one. *)
let rec field_ty_of_typ (t : Types.typ) : Ast.field_ty option =
  match t with
  | Types.TScalar n -> Some (Scalar n)
  | Types.TRef n -> Some (Ref n)
  | Types.TMulti (Types.TScalar n) -> Some (Multi n)
  | Types.TMap (Types.TScalar k, Types.TScalar v) -> Some (Map (k, v))
  | Types.TNullable inner -> ( match field_ty_of_typ inner with Some ft -> Some (Nullable ft) | None -> None)
  | Types.TMulti _ | Types.TMap _ | Types.TVoid -> None

let builtin_ret (name : string) (argty : Ast.field_ty option) : Ast.field_ty option =
  match name with
  | "int_to_text" -> Some (Scalar "Text")
  | "now" -> Some (Scalar "Timestamp")
  | "print" | "print_int" | "push" | "set" -> Some (Scalar "Int")
  | "words" | "count" -> Some (Scalar "Int")
  | "has" -> Some (Scalar "Bool")
  | "latest" -> ( match argty with Some t -> ( match unwrap t with Multi e -> Some (Scalar e) | _ -> None) | None -> None)
  | "get" -> (
    match argty with
    | Some t -> ( match unwrap t with Multi e -> Some (Scalar e) | Map (_, v) -> Some (Scalar v) | _ -> None)
    | None -> None)
  (* systems stdlib — kept in sync with Types.builtin_confident_ret (both
     tables, same contract, different type languages). This is also what
     classifies a `let` holding a fresh Text or a fresh `multi` as owned, so
     a missing entry here is a leak, not just a lost type. *)
  | "len" | "byte_at" | "index_of" | "last_index_of" -> Some (Scalar "Int")
  | "print_err" | "sort" | "reverse" -> Some (Scalar "Int")
  | "starts_with" | "ends_with" | "remove" -> Some (Scalar "Bool")
  | "substr" | "trim" | "to_lower" | "char_of" | "join" -> Some (Scalar "Text")
  | "parse_int" -> Some (Nullable (Scalar "Int"))
  | "split" | "split_ws" -> Some (Multi "Text")
  | "slice" -> (
    match argty with Some t -> ( match unwrap t with Multi e -> Some (Multi e) | _ -> None) | None -> None)
  | "pop" | "shift" -> (
    match argty with Some t -> ( match unwrap t with Multi e -> Some (Scalar e) | _ -> None) | None -> None)
  | "key_at" -> (
    match argty with Some t -> ( match unwrap t with Map (k, _) -> Some (Scalar k) | _ -> None) | None -> None)
  | "val_at" -> (
    match argty with Some t -> ( match unwrap t with Map (_, v) -> Some (Scalar v) | _ -> None) | None -> None)
  | _ -> None

let is_builtin_name (n : string) =
  List.mem n
    [ "now"; "print"; "print_int"; "words"; "multi_new"; "push"; "get"; "count"; "latest";
      "map_new"; "set"; "has"; "int_to_text";
      (* systems stdlib *)
      "len"; "byte_at"; "print_err"; "starts_with"; "ends_with"; "index_of"; "last_index_of";
      "substr"; "trim"; "to_lower"; "char_of"; "parse_int"; "split"; "split_ws"; "join"; "slice";
      "pop"; "shift"; "sort"; "reverse"; "remove"; "key_at"; "val_at" ]

(* ---- unions and variants (haxe-parity Task 4) ------------------------

   All read straight off p_syms (types.ml's own tables) — the emitter
   derives nothing types.ml already knows. A payload union's variants
   each have a compiler-generated class-table entry keyed
   "<Union>.<Variant>" in p_class_id (built in emit's pass 1, below;
   idents can never contain a dot, so the composite key cannot collide
   with a source-declared class — the same mangling convention
   "Class.method" already uses in the method table). An all-bare union
   has no entries at all: its variants ARE their ordinals. *)
let find_union (p : pctx) (n : string) : Types.union_info option =
  Types.StringMap.find_opt n p.p_syms.Types.unions

let find_variant (p : pctx) (n : string) : (Types.union_info * Types.variant_info) option =
  Types.find_variant p.p_syms n

let variant_class_key (u : Types.union_info) (vi : Types.variant_info) : string =
  u.Types.u_name ^ "." ^ vi.Types.vi_name

(* the runtime tag a case pattern / bare reference compares or loads:
   the class-table id for a payload union's variant (the object header's
   class_id — the format doc's variant convention), the ordinal for an
   all-bare union's. *)
let variant_tag_value (p : pctx) (u : Types.union_info) (vi : Types.variant_info) : int =
  if u.Types.u_has_payload then
    match SM.find_opt (variant_class_key u vi) p.p_class_id with
    | Some cid -> cid
    | None -> 0 (* unreachable: pass 1 registers every payload-union variant *)
  else vi.Types.vi_tag

let rec ty_of_expr (p : pctx) (f : fstate) (e : Ast.expr) : Ast.field_ty option =
  match e.kind with
  | IntLit _ -> Some (Scalar "Int")
  | StrLit _ -> Some (Scalar "Text")
  | BoolLit _ -> Some (Scalar "Bool")
  (* Same rule as owner.ml's expr_ty: a non-empty list literal knows its
     element type; an empty `[]`/`{}` is contextual on its destination. *)
  | ListLit (first :: _) -> (
    match ty_of_expr p f first with Some (Scalar n) -> Some (Multi n) | _ -> None)
  | ListLit [] | MapLit -> None
  (* haxe-parity Task 6: contextual on its destination (see owner.ml). *)
  | NilLit -> None
  | As (_, ty) -> Some (Nullable ty)
  (* haxe-parity Task 5: a `try` yields its try arm's type — types.ml has
     already required the catch arm to agree. *)
  | Try t -> ty_of_expr p f t.body
  | Ident n -> (
    match List.assoc_opt n f.f_env with
    | Some (_, t) -> Some t
    | None -> (
      (* haxe-parity Task 4: a bare variant reference types as its
         union; locals/params always won above, so a shadowing binding
         is never mistaken for a variant. *)
      match find_variant p n with
      | Some (u, _) -> Some (Scalar u.Types.u_name)
      | None -> None))
  | Field (base, fname) -> (
    match ty_of_expr p f base with
    | Some bt -> (
      match unwrap bt with
      | Scalar cn -> (
        match class_of_name p cn with
        | Some cid -> ( match field_of p cid fname with Some (_, t) -> Some t | None -> None)
        | None -> None)
      | _ -> None)
    | None -> None)
  | Index (base, _) -> (
    match ty_of_expr p f base with
    | Some bt -> ( match unwrap bt with Multi e' -> Some (Scalar e') | Map (_, v) -> Some (Scalar v) | _ -> None)
    | None -> None)
  | Call (callee, args) -> (
    match callee.kind with
    | Ident n -> (
      (* haxe-parity Task 1 (modules), CRITICAL 1 review fix: own module
         first — see emit_call's identical fix for why p_syms' flat merge
         is the wrong table once two modules can share a free-fn name. *)
      let own_mid = p.p_module_of f.f_file in
      let own_fi =
        match Hashtbl.find_opt p.p_module_syms own_mid with
        | Some msyms -> Types.StringMap.find_opt n msyms.Types.free_fns
        | None -> None
      in
      match own_fi with
      | Some fi -> fi.Types.ret
      | None -> (
        match free_fn p n with
        | Some fi -> fi.Types.ret
        | None -> (
          (* haxe-parity Task 4: variant construction — the call's value
             is the union's own type (a fresh variant object). *)
          match find_variant p n with
          | Some (u, _) -> Some (Scalar u.Types.u_name)
          | None ->
            if is_builtin_name n then
              builtin_ret n (match args with a :: _ -> ty_of_expr p f a | [] -> None)
            else None)))
    | Field (base, mname) -> (
      match ty_of_expr p f base with
      | Some bt -> (
        match unwrap bt with
        | Scalar cn -> (
          match class_method p cn mname with
          | Some mi -> mi.Types.ret
          | None -> ( match iface_method p cn mname with Some (_, sg) -> sg.Types.ret | None -> None))
        | _ -> None)
      | None -> (
        (* base didn't resolve as a value at all (no local/param/self
           named that) — a qualified free-fn call through a `use` alias
           (haxe-parity Task 1) has exactly this shape; a genuine
           receiver is always caught by the `Some bt` arm above, so
           locals/params still shadow a same-named alias here too. *)
        match base.kind with
        (* haxe-parity Task 7: a static call's base names a class, which is
           never a value — checked before the `use`-alias reading, since a
           class name and a module alias are both bare Idents here. *)
        | Ident cls_name when static_method p cls_name mname <> None -> (
          match static_method p cls_name mname with Some mi -> mi.Types.ret | None -> None)
        | Ident alias -> (
          match use_edge_for p ~file:f.f_file alias with
          (* the systems stdlib's own declared return shapes (Types.
             stdlib_members) — the source of `st.size` resolving at all *)
          | Some u when u.Types.ue_is_stdlib -> (
            match Types.stdlib_member alias mname with
            | Some sm -> ( match sm.Types.sm_ret with Some t -> field_ty_of_typ t | None -> None)
            | None -> None)
          | Some u -> (
            let target_mid = Types.path_str u.Types.ue_segments in
            match Hashtbl.find_opt p.p_module_syms target_mid with
            | Some msyms -> (
              match Types.StringMap.find_opt mname msyms.Types.free_fns with
              | Some fi -> fi.Types.ret
              | None -> None)
            | None -> None)
          | None -> None)
        | _ -> None))
    | _ -> None)
  | Unary (Neg, o) -> ty_of_expr p f o
  | Binary (op, l, _) -> (
    match op with
    | Concat -> Some (Scalar "Text")
    | Eq | Ne | Lt | Le | Gt | Ge | And | Or -> Some (Scalar "Bool")
    | Add | Sub | Mul | Div | Mod -> ( match ty_of_expr p f l with Some t -> Some t | None -> Some (Scalar "Int")))
  | Ctor (cn, _) -> Some (Scalar cn)
  | Interp _ -> Some (Scalar "Text")
  | DbStub _ -> None
  | Switch (subject, arms) -> (
    (* haxe-parity Task 3: mirrors types.ml's own `typecheck_switch` —
       the first arm's type wins (types.ml already proved every other
       arm agrees, or reported WO-E201 if not) — but derived through
       this file's own, narrower `ty_of_expr` rather than duplicating
       that pass. Needed for real, not a "not chased" placeholder like
       `Index`/`Binary` above: this is what tells `Let`'s own emission
       (below) whether a `let v = switch ... { case ...: "text"; ... }`
       with no `: Type` annotation holds `Text` or `Int` — get it
       wrong and a later `${v}` interpolation calls `int_to_text` on a
       Text register, or an `==` picks EQ over EQS.

       Task 4 fix round 1 (review Critical 1b): an arm yielding its own
       payload BINDING (`case Boxed(b): b;`) types as the binding's
       declared field type — the binding is not in f_env at derivation
       time (it exists only while the arm's own body is emitted), so
       the plain recursive call returned None and the Int fallback
       broke legal code downstream (`field access on Int`). Mirrors
       owner.ml's binding_ty_of_arm. *)
    match arms with
    | [] -> None
    | first :: _ -> (
      match List.rev first.body with
      | { s_kind = ExprStmt ve; _ } :: _ -> (
        match ve.kind with
        | Ident n -> (
          match emit_binding_ty_of_arm p f subject first n with
          | Some fty -> Some fty
          | None -> ty_of_expr p f ve)
        | _ -> ty_of_expr p f ve)
      | _ -> None))

(* The declared type of payload binding [n], when [arm]'s pattern binds
   it off [subject]'s union — None whenever this is not that shape.
   Mirrors owner.ml's binding_ty_of_arm (same convention as every other
   mirrored deriver pair between these two files). *)
and emit_binding_ty_of_arm (p : pctx) (f : fstate) (subject : Ast.expr) (arm : Ast.switch_arm)
    (n : string) : Ast.field_ty option =
  match ty_of_expr p f subject with
  | Some (Scalar sn) -> (
    match find_union p sn with
    | Some u when u.Types.u_has_payload -> (
      match arm.values with
      | [ { kind = Call ({ kind = Ident vname; _ }, bargs); _ } ] -> (
        match
          List.find_opt (fun (vi : Types.variant_info) -> vi.Types.vi_name = vname)
            u.Types.u_variants
        with
        | Some vi when List.length bargs = List.length vi.Types.vi_fields ->
          let rec zip (args : Ast.expr list) fields =
            match (args, fields) with
            | { kind = Ident bn; _ } :: _, (_, fty) :: _ when bn = n -> Some fty
            | _ :: ta, _ :: tf -> zip ta tf
            | _ -> None
          in
          zip bargs vi.Types.vi_fields
        | _ -> None)
      | _ -> None)
    | _ -> None)
  | _ -> None

let is_text (p : pctx) (f : fstate) (e : Ast.expr) : bool =
  match ty_of_expr p f e with Some t -> ( match unwrap t with Scalar "Text" -> true | _ -> false) | None -> false

(* ============================================================
   Lowering
   ============================================================ *)

let lookup_local (f : fstate) (n : string) : (int * Ast.field_ty) option = List.assoc_opt n f.f_env

let reg_of_item (p : pctx) (f : fstate) (i : Owner.drop_item) : int option =
  match Hashtbl.find_opt f.f_decl i.Owner.di_node with
  | Some r -> Some r
  | None -> (
    match lookup_local f i.Owner.di_name with
    | Some (r, _) -> Some r
    | None ->
      err p ~code:unguardable_code ~file:f.f_file ~pos:{ line = 0; col = 0 }
        ~message:
          (Printf.sprintf "ownership table names `%s`, which has no register in `%s`"
             i.Owner.di_name f.f_fn);
      None)

(* DROP for every item of a drop set, in the table's own order (the
   tables already list them innermost-scope-first, reverse declaration
   order inside a scope — that is destruction order). *)
let emit_drops (p : pctx) (f : fstate) (items : Owner.drop_item list) : unit =
  List.iter
    (fun (i : Owner.drop_item) ->
      match reg_of_item p f i with
      | None -> ()
      | Some r -> (
        match i.Owner.di_kind with
        | Owner.LOwned ->
          put f (ins_abc op_drop r 0 0);
          mask_clear f r
        | Owner.LGc ->
          (* a @gc handle's release is an rc site, never a DROP: the RC
             table carries it (with its own ELIDED decision) *)
          ()))
    items

let emit_scope_drops (p : pctx) (f : fstate) (v : views) ~(node : int) ~(label : string) : unit =
  match Hashtbl.find_opt v.v_scope (node, label) with
  | Some items -> emit_drops p f items
  | None -> ()

(* the declaring nodes a block introduced: everything pushed onto the
   declaration stack since it opened *)
let declared_since (f : fstate) (saved : int list) : int list =
  let rec take n l =
    if n <= 0 then [] else match l with [] -> [] | x :: tl -> x :: take (n - 1) tl
  in
  take (List.length f.f_declared - List.length saved) f.f_declared

let emit_join_drops (p : pctx) (f : fstate) (v : views) ~(node : int) ~(label : string) : unit =
  match Hashtbl.find_opt v.v_join (node, label) with
  | Some items -> emit_drops p f items
  | None -> ()

(* RC sites, minus the ELIDED ones — the spec's zero-cost promise lives
   here and in the residual-only borrow rule. `which` selects acquires or
   releases; a return site carries both plus the returned value's own
   escape increment, and acquires must precede releases or a balanced
   pair could momentarily reach rc 0. *)
let emit_rc (p : pctx) (f : fstate) (v : views) ~(node : int) ~(acquire : bool)
    ?(groups : int list option) () : unit =
  match Hashtbl.find_opt v.v_rc node with
  | None -> ()
  | Some sites ->
    List.iter
      (fun (r : Owner.rc_site) ->
        let want = match r.Owner.rc_op with Owner.RcAcquire -> true | Owner.RcRelease -> false in
        let in_scope =
          match groups with None -> true | Some gs -> List.mem r.Owner.rc_group gs
        in
        if want = acquire && in_scope && not r.Owner.rc_elided then
          let reg =
            if r.Owner.rc_group >= 0 then Hashtbl.find_opt f.f_decl r.Owner.rc_group
            else Hashtbl.find_opt f.f_node r.Owner.rc_node
          in
          match reg with
          | Some g ->
            put f (ins_abc (if acquire then op_rc_inc else op_rc_dec) g 0 0);
            if not acquire then mask_clear f g
          | None ->
            err p ~code:unguardable_code ~file:f.f_file ~pos:r.Owner.rc_pos
              ~message:
                (Printf.sprintf "rc site for `%s` has no register in `%s`" r.Owner.rc_place f.f_fn))
      sites

(* The frame's drop map at a call / DB_STUB site. The owner table is
   authoritative here (it is taken after the call's own argument
   transfers, so a value moved in is the callee's responsibility); an
   absent entry means nothing is live. *)
let sync_mask (p : pctx) (f : fstate) (v : views) (node : int) : unit =
  f.f_owned <- 0L;
  f.f_gc <- 0L;
  match Hashtbl.find_opt v.v_mask node with
  | None -> ()
  | Some items ->
    List.iter
      (fun (i : Owner.drop_item) ->
        match reg_of_item p f i with Some r -> mask_set f i.Owner.di_kind r | None -> ())
      items

(* Residual borrow guards around one region, coalesced per operand.
   [gbase] names a run of registers reserved *below* the call window: a
   guard may not live in the window itself, because the callee's frame
   overlaps the window (it may assign to its own parameters, and it
   zeroes the slots above them) and the call's return value lands on the
   window base. Releasing such a register after the call would hand the
   borrow word of whatever now sits there — in the worst case an
   integer result — to wo_release_excl. So each guarded operand is
   copied out of the window into its own stable slot first. *)
let residual_guards (p : pctx) (f : fstate) (v : views) (node : int) (gbase : int option) :
    (int * Owner.acc_kind) list =
  match Hashtbl.find_opt v.v_res node with
  | None -> []
  | Some (pos, ops) ->
    Hashtbl.replace v.v_res_used node ();
    List.concat
      (List.mapi
         (fun i (onode, kind) ->
           match Hashtbl.find_opt f.f_node onode with
           | Some r -> (
             match gbase with
             | None -> [ (r, kind) ] (* no call in the region: guard in place *)
             | Some gb ->
               let g = gb + i in
               if g <> r then put f (ins_abc op_move g r 0);
               [ (g, kind) ])
           | None ->
             err p ~code:unguardable_code ~file:f.f_file ~pos
               ~message:
                 (Printf.sprintf
                    "residual borrow site in `%s` names an operand with no live register — the \
                     runtime guard cannot be placed"
                    f.f_fn);
             [])
         ops)

let residual_count (v : views) (node : int) : int =
  match Hashtbl.find_opt v.v_res node with None -> 0 | Some (_, ops) -> List.length ops

let acquire_guards (f : fstate) (gs : (int * Owner.acc_kind) list) : unit =
  List.iter
    (fun (r, k) ->
      match k with
      | Owner.AShared -> put f (ins_abc op_borrow_s r 0 0)
      | Owner.AExcl | Owner.AMove -> put f (ins_abc op_borrow_x r 0 0))
    gs

let release_guards (f : fstate) (gs : (int * Owner.acc_kind) list) : unit =
  List.iter
    (fun (r, k) ->
      match k with
      | Owner.AShared -> put f (ins_abc op_release_s r 0 0)
      | Owner.AExcl | Owner.AMove -> put f (ins_abc op_release_x r 0 0))
    (List.rev gs)

let check_bx (p : pctx) (f : fstate) (pos : Ast.pos) (what : string) (v : int) : int =
  if v > 0xFFFF then begin
    err p ~code:limit_code ~file:f.f_file ~pos
      ~message:(Printf.sprintf "%s index %d exceeds the 16-bit instruction field" what v);
    0
  end
  else v

let check_field_idx (p : pctx) (f : fstate) (pos : Ast.pos) (v : int) : int =
  if v > 0xFF then begin
    err p ~code:limit_code ~file:f.f_file ~pos
      ~message:(Printf.sprintf "field index %d exceeds the 8-bit GETF/SETF field" v);
    0
  end
  else v

(* Kind immediates for a fresh container, taken from the type the value is
   expected to have at its destination (a constructor field, an
   assignment target, a declared parameter type). There is deliberately
   no default: the kinds are the container's drop plan
   (runtime/src/gc.c), so guessing SCALAR for a `multi Item` would leak
   every element and guessing it for a `map<Text, _>` would leak every
   key. Milestone-1 `let` has no container type annotation (parser.ml
   takes a bare identifier), so a fresh container must be created
   somewhere its type is declared — otherwise WO-E403 at the creation
   site, where the reader can act on it. *)
(* v2 class-table metadata for one field (runtime/src/wob.h):
   - field_class: the class a field refers to — its own class for an
     owned/@gc field, its ELEMENT's class for a container of records — or the
     json-raw marker for a `json.Value` field, else "none".
   - field_elem: a container field's element kinds (a multi's element kind; a
     map's key kind and value kind packed as the container_imm immediate),
     0 for everything else.
   Both exist so json.encode/json.decode can work off metadata instead of
   per-type generated code. *)
let wob_field_json_raw = 0xFFFFFFFE
let wob_field_nil_scalar = 0xFFFFFFFD

(* nil for a nullable SCALAR is not the zero word: `0` is a real Int, and the
   driving workload stores it in a `?Int` (a cron `*` field expands to `0`), so
   absence needs a value no plain Int will ever hold. runtime/src/wob.h's
   WO_NIL_SCALAR = INT64_MIN. Heap-shaped optionals keep the zero word — a null
   pointer is unambiguous. *)
(* -(2^62). NOT INT64_MIN: OCaml's native int is 63-bit, so INT64_MIN cannot be
   written here at all (and `min_int * 2` silently wraps to 0 — the bug this
   comment exists to prevent recurring). Mirrors runtime/src/wob.h's
   WO_NIL_SCALAR exactly. *)
let nil_scalar_word = -4611686018427387904

(* is this a `?scalar` — an optional whose representation is a plain register,
   so its nil has to be the sentinel rather than the zero word? *)
let is_nullable_scalar (p : pctx) (ty : Ast.field_ty) : bool =
  match ty with
  | Ast.Nullable inner -> field_kind p inner = 0 (* WO_K_SCALAR *)
  | _ -> false

let field_class_meta (p : pctx) (ty : Ast.field_ty) : int =
  let name_of t = match t with Ast.Scalar n -> Some n | _ -> None in
  if is_nullable_scalar p ty then wob_field_nil_scalar
  else
  match unwrap ty with
  | Ast.Scalar n when n = Types.json_value_type -> wob_field_json_raw
  | Ast.Scalar n -> ( match class_of_name p n with Some cid -> cid | None -> wob_none)
  | Ast.Multi e | Ast.Map (_, e) -> (
    match name_of (Ast.Scalar e) with
    | Some n -> ( match class_of_name p n with Some cid -> cid | None -> wob_none)
    | None -> wob_none)
  | Ast.Ref _ | Ast.Nullable _ -> wob_none

let field_elem_meta (p : pctx) (ty : Ast.field_ty) : int =
  match unwrap ty with
  | Ast.Multi e -> field_kind p (Ast.Scalar e)
  | Ast.Map (k, v) -> field_kind p (Ast.Scalar k) lor (field_kind p (Ast.Scalar v) lsl 4)
  | _ -> 0

let container_imm (p : pctx) (expected : Ast.field_ty option) (map : bool) : int option =
  match expected with
  | Some t -> (
    match unwrap t with
    | Multi e when not map -> Some (field_kind p (Scalar e))
    | Map (k, v) when map -> Some (field_kind p (Scalar k) lor (field_kind p (Scalar v) lsl 4))
    | _ -> None)
  | None -> None

(* `nil` written literally on either side of a comparison — see emit_binary's
   Eq/Ne cases for why the distinction matters. *)
let is_nil_lit (e : Ast.expr) : bool = match e.Ast.kind with Ast.NilLit -> true | _ -> false

let rec emit_expr (p : pctx) (f : fstate) (v : views) ~(dst : int) ?expected (e : Ast.expr) : unit =
  f.f_cur_line <- e.pos.line;
  (match e.kind with
  | IntLit n -> put f (ins_abx op_loadk dst (check_bx p f e.pos "constant" (const_int p n)))
  | BoolLit b -> put f (ins_abx op_loadk dst (check_bx p f e.pos "constant" (const_int p (if b then 1 else 0))))
  | StrLit s -> put f (ins_abx op_loadk dst (check_bx p f e.pos "constant" (const_text p s)))
  (* haxe-parity Task 6: `nil` is the zero word for a heap-shaped `?T` and the
     WO_NIL_SCALAR sentinel for a `?scalar` — see nil_scalar_word. The
     destination decides: a written annotation, the field being built, or the
     enclosing method's return type. With no destination at all, the zero word
     is the safe answer (a heap slot). *)
  | NilLit ->
    let dest = match expected with Some _ -> expected | None -> f.f_ret in
    let word =
      match dest with Some t when is_nullable_scalar p t -> nil_scalar_word | _ -> 0
    in
    put f (ins_abx op_loadk dst (check_bx p f e.pos "constant" (const_int p word)))
  (* Container literals lower to exactly what `multi_new()`/`map_new()`
     lower to — the element kinds are the destination's, never guessed
     (docs/plan/oop-vm/08-builtin-surface.md) — plus one `multi_push` per
     element, in source order. *)
  | ListLit items -> (
    (* the destination decides the element kinds; when there is no declared
       destination, a non-empty literal knows its own element type and a
       tail-position literal (`return []`) takes the method's return type *)
    let dest =
      match expected with
      | Some _ -> expected
      | None -> ( match ty_of_expr p f e with Some t -> Some t | None -> f.f_ret)
    in
    match container_imm p dest false with
    | None ->
      err p ~code:cannot_lower_code ~file:f.f_file ~pos:e.pos
        ~message:
          "a list literal needs a destination of declared type `multi T` — its element kind is \
           the container's drop plan and cannot be guessed";
      put f (ins_abx op_loadk dst (const_int p 0))
    | Some imm ->
      sync_mask p f v e.id;
      put f (ins_abc op_builtin dst imm b_multi_new);
      let elem_ty =
        match dest with
        | Some t -> ( match unwrap t with Multi en -> Some (Scalar en) | _ -> None)
        | None -> None
      in
      let outer = f.f_temp in
      if f.f_temp <= dst then f.f_temp <- dst + 1;
      List.iter
        (fun (item : Ast.expr) ->
          let save = f.f_temp in
          let base = alloc_temps p f e.pos 2 in
          put f (ins_abc op_move base dst 0);
          (match elem_ty with
          | Some et -> emit_expr p f v ~dst:(base + 1) ~expected:et item
          | None -> emit_expr p f v ~dst:(base + 1) item);
          sync_mask p f v e.id;
          f.f_cur_line <- item.pos.line;
          put f (ins_abc op_builtin base base b_multi_push);
          f.f_temp <- save)
        items;
      f.f_temp <- outer)
  | MapLit -> (
    let dest = match expected with Some _ -> expected | None -> f.f_ret in
    match container_imm p dest true with
    | None ->
      err p ~code:cannot_lower_code ~file:f.f_file ~pos:e.pos
        ~message:
          "an empty map literal needs a destination of declared type `map<K, V>` — its key and \
           value kinds are the container's drop plan and cannot be guessed";
      put f (ins_abx op_loadk dst (const_int p 0))
    | Some imm ->
      sync_mask p f v e.id;
      put f (ins_abc op_builtin dst imm b_map_new))
  | Try t -> emit_try p f v ~dst ?expected e t.body t.ename t.handler
  (* `json.decode(text) as T` is the ONE `as` this language has: a checked
     decode, lowered to json_decode(text, class id of T). The decode needs
     the target class, and the target class is exactly what `as` names — so
     the two are one instruction, never separable. Any other `as` (and a
     bare `json.decode(...)` with no `as`) is WO-E403: there is no
     reinterpret cast in the doctrine. *)
  | As (inner, ty) -> (
    let target = match unwrap ty with Scalar n -> class_of_name p n | _ -> None in
    match (inner.kind, target) with
    | Call ({ kind = Field ({ kind = Ident "json"; _ }, "decode"); _ }, [ src ]), Some cid ->
      let base = alloc_temps p f e.pos 2 in
      let save = f.f_temp in
      emit_expr p f v ~dst:base src;
      f.f_temp <- save;
      put f (ins_abx op_loadk (base + 1) (check_bx p f e.pos "constant" (const_int p cid)));
      sync_mask p f v e.id;
      f.f_cur_line <- e.pos.line;
      put f (ins_abc op_builtin dst base b_json_decode)
    | Call ({ kind = Field ({ kind = Ident "json"; _ }, "decode"); _ }, _), None ->
      err p ~code:cannot_lower_code ~file:f.f_file ~pos:e.pos
        ~message:"`as` needs a declared class or record type to decode into";
      put f (ins_abx op_loadk dst (const_int p 0))
    | _ ->
      err p ~code:cannot_lower_code ~file:f.f_file ~pos:e.pos
        ~message:
          "`as` is only a checked JSON decode (`json.decode(text) as T`) — this language has no \
           reinterpret cast";
      put f (ins_abx op_loadk dst (const_int p 0)))
  | Ident n -> (
    match lookup_local f n with
    | Some (r, _) -> if r <> dst then put f (ins_abc op_move dst r 0)
    | None -> (
      (* haxe-parity Task 4: a bare variant reference. All-bare union:
         the value IS the ordinal tag — one LOADK, no heap. Payload
         union: every value of the union is a variant object, so even a
         bare variant allocates its (zero-field) class — NEW is all it
         takes, the header's class_id is the tag. *)
      match find_variant p n with
      | Some (u, vi) ->
        if u.Types.u_has_payload then
          put f (ins_abx op_new dst (check_bx p f e.pos "class" (variant_tag_value p u vi)))
        else
          put f
            (ins_abx op_loadk dst
               (check_bx p f e.pos "constant" (const_int p vi.Types.vi_tag)))
      | None ->
        err p ~code:cannot_lower_code ~file:f.f_file ~pos:e.pos
          ~message:(Printf.sprintf "`%s` is not a local, parameter, or `self` — nothing to load" n);
        put f (ins_abx op_loadk dst (const_int p 0))))
  | Field (base, fname) -> (
    match ty_of_expr p f base with
    | Some bt -> (
      match unwrap bt with
      | Scalar cn -> (
        match class_of_name p cn with
        | Some cid -> (
          match field_of p cid fname with
          | Some (idx, _) ->
            let b = emit_operand p f v base in
            put f (ins_abc op_getf dst b (check_field_idx p f e.pos idx))
          | None ->
            err p ~code:cannot_lower_code ~file:f.f_file ~pos:e.pos
              ~message:(Printf.sprintf "`%s` has no field `%s`" cn fname);
            put f (ins_abx op_loadk dst (const_int p 0)))
        | None ->
          err p ~code:cannot_lower_code ~file:f.f_file ~pos:e.pos
            ~message:(Printf.sprintf "field access on `%s`, which is not a declared class" cn);
          put f (ins_abx op_loadk dst (const_int p 0)))
      | _ ->
        err p ~code:cannot_lower_code ~file:f.f_file ~pos:e.pos
          ~message:(Printf.sprintf "field `%s` read from a value that is not a class instance" fname);
        put f (ins_abx op_loadk dst (const_int p 0)))
    | None ->
      err p ~code:cannot_lower_code ~file:f.f_file ~pos:e.pos
        ~message:
          (Printf.sprintf "cannot resolve the type of the value `%s` is read from" fname);
      put f (ins_abx op_loadk dst (const_int p 0)))
  | Index (base, idx) -> (
    let bid =
      match ty_of_expr p f base with
      (* `m[k]` on a map is the OPTIONAL read — a missing key is nil, which is
         what makes `let v = m[k]; if v != nil { ... }` the ordinary lookup
         idiom. `get(m, k)` keeps asserting (WO_B_MAP_GET traps KEY). A `multi`
         index still traps out of range: a bad index is a fault, not an
         absence. *)
      | Some bt -> ( match unwrap bt with Multi _ -> Some b_multi_get | Map _ -> Some b_map_get_opt | _ -> None)
      | None -> None
    in
    match bid with
    | None ->
      err p ~code:cannot_lower_code ~file:f.f_file ~pos:e.pos
        ~message:"indexing a value that is neither a `multi` nor a `map`";
      put f (ins_abx op_loadk dst (const_int p 0))
    | Some bid ->
      let w = alloc_temps p f e.pos 2 in
      emit_expr p f v ~dst:w base;
      emit_expr p f v ~dst:(w + 1) idx;
      put f (ins_abc op_builtin dst w bid))
  | Unary (Neg, o) ->
    let b = emit_operand p f v o in
    put f (ins_abc op_neg dst b 0)
  | Binary (op, l, r) -> emit_binary p f v ~dst op l r
  | Ctor (cn, fields) -> emit_ctor p f v ~dst e cn fields
  | Interp inner -> (
    (* haxe-parity Task 2: the type-directed half of the interpolation
       desugar (parser.ml's own doc comment on Ast.Interp) — a Text
       interpolant passes through untouched; an Int one is wrapped in
       the `int_to_text` builtin; anything else has no lowering (the
       brief's own scope: "desugar Int-typed expressions", not every
       scalar). *)
    match ty_of_expr p f inner with
    | Some t -> (
      match unwrap t with
      | Scalar "Text" -> emit_expr p f v ~dst inner
      | Scalar "Int" -> emit_builtin p f v ~dst e "int_to_text" [ inner ]
      | other ->
        err p ~code:cannot_lower_code ~file:f.f_file ~pos:e.pos
          ~message:
            (Printf.sprintf
               "cannot interpolate a value of type `%s` in \"${...}\" — only Text and Int are \
                supported"
               (Dump.field_ty_str other));
        put f (ins_abx op_loadk dst (const_int p 0)))
    | None ->
      err p ~code:cannot_lower_code ~file:f.f_file ~pos:e.pos
        ~message:"cannot resolve the interpolated expression's type";
      put f (ins_abx op_loadk dst (const_int p 0)))
  | Call (callee, args) -> emit_call p f v ~dst ?expected e callee args
  | DbStub _ ->
    sync_mask p f v e.id;
    put f (ins_abc op_db_stub 0 0 0)
  | Switch (subject, arms) -> emit_switch p f v e ~dst subject arms);
  Hashtbl.replace f.f_node e.id dst;
  (* An escaping @gc value takes its increment right where the value
     lands. owner.ml's gc_escape anchors that acquire on the *place
     expression's* own node and gives it group -1 (never elidable), and
     it fires for all four escapes alike: a constructor field, an
     assignment into a field, a `take` argument, and a return. Emitting
     it here — once, at the one place every expression passes through —
     is what keeps all four in step; anchoring it per statement kind is
     how the constructor-field case went missing. *)
  emit_rc p f v ~node:e.id ~acquire:true ()

(* An operand that only needs to *be* in some register: a place already
   living in one is used where it is, everything else lands in a fresh
   temporary. This is what keeps a proven method's disassembly free of
   pointless MOVEs. *)
and emit_operand (p : pctx) (f : fstate) (v : views) (e : Ast.expr) : int =
  match e.kind with
  | Ident n when lookup_local f n <> None ->
    let r = match lookup_local f n with Some (r, _) -> r | None -> 0 in
    Hashtbl.replace f.f_node e.id r;
    r
  | _ ->
    let t = alloc_temp p f e.pos in
    emit_expr p f v ~dst:t e;
    t

(* The last value a statement computes (a `return` operand, a discarded
   expression statement) needs no reserved slot: a place stays where it
   lives, everything else lands in the next free temporary, which a call
   then reuses as its own window base so the result needs no MOVE. *)
and emit_tail (p : pctx) (f : fstate) (v : views) (e : Ast.expr) : int =
  match e.kind with
  | Ident n when lookup_local f n <> None ->
    let r = match lookup_local f n with Some (r, _) -> r | None -> 0 in
    Hashtbl.replace f.f_node e.id r;
    emit_rc p f v ~node:e.id ~acquire:true ();
    r
  | _ ->
    (* allocate (so the register counts towards the budget and the
       method's register count) and immediately un-reserve *)
    let t = alloc_temp p f e.pos in
    f.f_temp <- t;
    emit_expr p f v ~dst:t e;
    t

and emit_binary (p : pctx) (f : fstate) (v : views) ~(dst : int) (op : Ast.binop) (l : Ast.expr)
    (r : Ast.expr) : unit =
  let pos = l.pos in
  let simple o =
    let a = emit_operand p f v l in
    let b = emit_operand p f v r in
    put f (ins_abc o dst a b)
  in
  let swapped o =
    let a = emit_operand p f v l in
    let b = emit_operand p f v r in
    put f (ins_abc o dst b a)
  in
  (* `x == nil` / `nil == x`: the literal takes ITS destination type from the
     other operand, so the sentinel-vs-zero choice matches what x actually
     holds. *)
  let nil_compare_into p f v ~dst o (l : Ast.expr) (r : Ast.expr) : unit =
    let other = if is_nil_lit l then r else l in
    let nil_e = if is_nil_lit l then l else r in
    let a = emit_operand p f v other in
    let bt = ty_of_expr p f other in
    let save = f.f_temp in
    let b = alloc_temp p f pos in
    (match bt with
    | Some t -> emit_expr p f v ~dst:b ~expected:t nil_e
    | None -> emit_expr p f v ~dst:b nil_e);
    put f (ins_abc o dst a b);
    f.f_temp <- save
  in
  let nil_compare o = nil_compare_into p f v ~dst o l r in
  match op with
  | Add -> simple op_add
  | Sub -> simple op_sub
  | Mul -> simple op_mul
  | Div -> simple op_div
  | Concat -> simple op_concat
  | Lt -> simple op_lt
  | Le -> simple op_le
  | Gt -> swapped op_lt
  | Ge -> swapped op_le
  (* A comparison against `nil` is a WORD compare, never a content compare:
     EQS would dereference nil as a `wo_str*` (the VM's str_check traps on
     that, so an `x != nil` guard would trap instead of answering). The literal
     `nil` is emitted with the OTHER side's declared type as its destination,
     so a `?scalar` compares against the sentinel and a heap optional against
     zero. Text-vs-Text still uses EQS. *)
  | Eq ->
    if is_nil_lit l || is_nil_lit r then nil_compare op_eq
    else if is_text p f l || is_text p f r then simple op_eqs
    else simple op_eq
  | Ne ->
    (* no NE opcode in the v1 set: `a != b` is `(a == b) == 0`. The
       format doc governs, so this is a lowering, not a new opcode. *)
    let t = alloc_temp p f pos in
    (if is_nil_lit l || is_nil_lit r then begin
       let save = f.f_temp in
       f.f_temp <- t + 1;
       nil_compare_into p f v ~dst:t op_eq l r;
       f.f_temp <- save
     end
     else
       let a = emit_operand p f v l in
       let b = emit_operand p f v r in
       put f (ins_abc (if is_text p f l || is_text p f r then op_eqs else op_eq) t a b));
    let z = alloc_temp p f pos in
    put f (ins_abx op_loadk z (check_bx p f pos "constant" (const_int p 0)));
    put f (ins_abc op_eq dst t z)
  | And ->
    (* haxe-parity Task 2: short-circuit -- `l`'s own value (0 or 1)
       lands directly in `dst`; if it is already false, `r` is never
       evaluated at all (the fixture's own proof: a right operand that
       would trap, e.g. division by zero, must not run) and `dst` keeps
       `l`'s value. Otherwise `r`'s value overwrites `dst`, becoming the
       result. Compare-and-jump on the existing JZ opcode, no new one. *)
    emit_expr p f v ~dst l;
    f.f_cur_line <- pos.line;
    let jz = here f in
    put f (ins_asbx op_jz dst 0);
    emit_expr p f v ~dst r;
    patch_jump p f ~file:f.f_file ~pos jz (here f)
  | Or ->
    (* same shape, the other way: `l` true short-circuits (skip `r`,
       keep `l`'s true value); `l` false falls through to `r`. JZ plus
       one JMP (to skip `r` on the true path), still no new opcode. *)
    emit_expr p f v ~dst l;
    f.f_cur_line <- pos.line;
    let jz = here f in
    put f (ins_asbx op_jz dst 0);
    let jmp = here f in
    put f (ins_asbx op_jmp 0 0);
    patch_jump p f ~file:f.f_file ~pos jz (here f);
    emit_expr p f v ~dst r;
    patch_jump p f ~file:f.f_file ~pos jmp (here f)
  | Mod ->
    (* no MOD opcode either: truncating `a % b` is `a - (a / b) * b`,
       exact for the VM's truncating DIV (which traps on 0 and on
       INT64_MIN / -1 — both correct for `%` as well). *)
    let a = emit_operand p f v l in
    let b = emit_operand p f v r in
    let q = alloc_temp p f pos in
    put f (ins_abc op_div q a b);
    put f (ins_abc op_mul q q b);
    put f (ins_abc op_sub dst a q)

(* haxe-parity Task 3: compare-and-jump chain on the existing EQ/EQS/
   JZ/JMP opcodes — no new opcode, per the brief. The subject is
   evaluated exactly once, into `subj_reg`, and every arm's comparisons
   read it; `dst` is where an arm's value lands (mirrors `Binary`'s
   own And/Or short-circuit above: the caller's own `dst` is written
   directly, no intermediate temp to move out of).

   One case's `values` (the sample's own `case "@daily", "@midnight":`
   comma form) chains: value 1's failure falls through to check value
   2; any success jumps straight to the arm body, skipping the rest of
   that arm's own checks; the LAST value's failure falls through to the
   NEXT ARM's own first check (backpatched once that arm starts emitting,
   `pending_fail`). `default` has no checks at all — it always matches
   whatever reaches it — which is also why the missing-default case
   (already a WO-E208 *error*, so this image is never written — see
   main.ml's own has_error gate) still lowers without crashing: the
   last real arm's `pending_fail` simply has nowhere to go but the
   switch's own exit, same as `end_jumps` below.

   Each arm is its own drop scope: this inlines emit_block's own
   save/restore-and-drop shape (`f_nlocals`/`f_env`/`f_declared`,
   `emit_scope_drops`, the @gc release group) rather than calling
   emit_block outright, because emit_block emits every statement
   *generically* — including the last one, which for a value-yielding
   arm must land in the caller's `dst`, not a throwaway temp the way
   emit_block's own ExprStmt handling (emit_tail) would. An arm whose
   body doesn't end in `ExprStmt` (every arm in the sample's own
   statement-position switches, which end in `return`) writes nothing
   into `dst` — correct either way: statement position discards it
   regardless, and expression position already has types.ml's own
   WO-E201 for an arm that fails to yield a value (see typecheck_switch's
   own doc comment — this file does not re-check that).

   `f.f_div` (this path has returned) mirrors emit_if's own THEN/ELSE
   bookkeeping, generalized to N arms: an arm that diverged emits no
   trailing jump to the switch's exit and contributes nothing to the
   owned/@gc mask merge below (mask_meet, folded pairwise across every
   *non*-diverging arm — the N-way generalization of emit_if's own
   2-way `mask_meet f then_owned then_gc` call, same helper, unchanged).
   `emit_join_drops` (JOIN-DROP: a value this arm kept but some *other*
   arm moved) is owner.ml's own table entry, keyed exactly the way
   emit_if's THEN/ELSE already are — `(e.id, "ARM<i>")` here vs.
   `(s.s_id, "THEN"/"ELSE")` there — so this is a lookup, not new
   logic. *)
(* `~want_value` (Task 4 fix round 1, Critical 1): true everywhere the
   switch's value is consumed (a `let`'s value, a `return`, nested in an
   expression — every emit_expr path), false only for the discarded
   statement position (emit_stmt's own ExprStmt special case, mirroring
   types.ml's want_value:false). It gates exactly one thing: the
   payload MOVE-OUT below — an arm yielding its own binding nulls the
   subject's field only when someone actually takes ownership of the
   value; a discarded yield must leave the shell intact or the payload
   would leak with nobody left to drop it. *)
and emit_switch ?(want_value = true) (p : pctx) (f : fstate) (v : views) (e : Ast.expr)
    ~(dst : int) (subject : Ast.expr) (arms : Ast.switch_arm list) : unit =
  let subj_is_text = is_text p f subject in
  f.f_cur_line <- subject.pos.line;
  let subj_reg = emit_operand p f v subject in
  (* `dst` is not always already-reserved the way an ordinary expr's
     `dst` is: in statement position (`switch {...}` alone, discarded),
     `emit_tail`'s own "allocate then immediately un-reserve" convention
     hands this a `dst` that is the *next free* temp/local slot — bug
     found by this task's own ASan fixture, not a theoretical worry: an
     arm's own `let` (`alloc_local`'s register is `f_nlocals`, entirely
     independent of `f_temp`) legitimately picked that same slot, and
     the arm's own trailing value-write into `dst` then silently
     clobbered the live local sitting there before its DROP ran — a
     real, ASan-confirmed leak (RED), not a hypothetical. Reserving
     `dst` here, for the whole switch, fixes it at the source rather
     than special-casing the discard path: every `alloc_local`/
     `alloc_temp` inside any arm is now guaranteed a register above
     `dst`. A `let`-value switch's `dst` is already `< f_nlocals`
     (`alloc_local` ran before this function was ever called), so this
     is a no-op there — restoring `saved_nlocals` afterward gives back
     exactly nothing it did not itself reserve. *)
  let saved_nlocals = f.f_nlocals in
  if f.f_nlocals <= dst then f.f_nlocals <- dst + 1;
  if f.f_temp <= dst then f.f_temp <- dst + 1;
  bump f dst;
  (* haxe-parity Task 4: a union-typed subject compares variant TAGS.
     All-bare union: the subject register already holds the ordinal —
     compare it directly, exactly the scalar chain below. Payload union:
     read the tag (the object header's class_id) once, via the
     variant_tag builtin, and compare that; the subject register itself
     stays live into the arm BODIES (payload bindings GETF from it), so
     both it and the tag temp are reserved past every arm-local for the
     switch's own duration — the same saved_nlocals guard `dst` already
     rides, restored in one place below. *)
  let subj_union =
    match ty_of_expr p f subject with
    | Some (Scalar n) -> find_union p n
    | _ -> None
  in
  (* through `?T` too (fix round 1) — for PATTERN RECOGNITION only. A
     `?Union` subject with variant cases is already a hard WO-E201
     upstream (types.ml), so no image carrying this lowering is ever
     written; recognizing the patterns anyway (tag constants instead of
     expression evaluation, bindings bound) keeps the emitter from
     cascading misleading "`m` is not a local" errors on top of the real
     diagnostic. Tag reads (variant_tag) and the payload move-out keep
     gating on the EXACT `subj_union` above — never nullable-unwrapped. *)
  let subj_union_deep =
    match subj_union with
    | Some _ as u -> u
    | None -> (
      match ty_of_expr p f subject with
      | Some t -> ( match unwrap t with Scalar n -> find_union p n | _ -> None)
      | None -> None)
  in
  let cmp_reg =
    match subj_union with
    | Some u when u.Types.u_has_payload ->
      if f.f_nlocals <= subj_reg then f.f_nlocals <- subj_reg + 1;
      if f.f_temp <= subj_reg then f.f_temp <- subj_reg + 1;
      let t = alloc_temp p f subject.pos in
      f.f_cur_line <- subject.pos.line;
      put f (ins_abc op_builtin t subj_reg b_variant_tag);
      if f.f_nlocals <= t then f.f_nlocals <- t + 1;
      t
    | _ -> subj_reg
  in
  (* the tag constant a case pattern compares against, or None for the
     plain-value path (non-union subjects; also a malformed pattern over
     a union — already diagnosed upstream, WO-E201/E203, so no image is
     ever written — which falls back to the value path rather than
     crashing the serializer). *)
  let union_case_tag (value_e : Ast.expr) : int option =
    match subj_union_deep with
    | None -> None
    | Some u -> (
      let vname =
        match value_e.Ast.kind with
        | Ast.Ident n -> Some n
        | Ast.Call ({ Ast.kind = Ast.Ident n; _ }, _) -> Some n
        | _ -> None
      in
      match vname with
      | None -> None
      | Some n -> (
        match
          List.find_opt (fun (vi : Types.variant_info) -> vi.Types.vi_name = n) u.Types.u_variants
        with
        | Some vi -> Some (variant_tag_value p u vi)
        | None -> None))
  in
  let switch_temp_base = f.f_temp in
  let entry_owned = f.f_owned and entry_gc = f.f_gc in
  let div0 = f.f_div in
  let pending_fail = ref [] in
  let end_jumps = ref [] in
  let arm_results = ref [] in
  (* review fix, Critical 1: `default` has no comparison of its own —
     it matches unconditionally — so lowering the arms in raw *source*
     order made any `case` arm written after a `default` permanently
     unreachable dead code (reviewer-reproduced: `default` first,
     `case 2` after it, `classify(2)` returned the `default` value).
     `Ast.switch_lowering_order` moves `default` to the end before the
     chain is built; owner.ml's `analyze_switch` walks the identical
     order so the "ARM<i>" labels the two files hand each other over
     the drop-scope/JOIN-DROP tables never drift apart. *)
  List.iteri
    (fun i (arm : Ast.switch_arm) ->
      let label = Printf.sprintf "ARM%d" i in
      List.iter
        (fun pc -> patch_jump p f ~file:f.f_file ~pos:arm.Ast.arm_pos pc (here f))
        !pending_fail;
      pending_fail := [];
      f.f_owned <- entry_owned;
      f.f_gc <- entry_gc;
      f.f_div <- div0;
      f.f_temp <- switch_temp_base;
      (if not arm.Ast.is_default then begin
         let n = List.length arm.Ast.values in
         let to_body = ref [] in
         List.iteri
           (fun j (value_e : Ast.expr) ->
             f.f_cur_line <- value_e.Ast.pos.line;
             let b =
               match union_case_tag value_e with
               | Some tagv ->
                 (* a variant pattern never evaluates as an expression —
                    its tag constant is loaded directly (a payload
                    pattern lowered through emit_operand would NEW a
                    fresh object and compare pointers: always false) *)
                 let tb = alloc_temp p f value_e.Ast.pos in
                 put f
                   (ins_abx op_loadk tb
                      (check_bx p f value_e.Ast.pos "constant" (const_int p tagv)));
                 tb
               | None -> emit_operand p f v value_e
             in
             let t = alloc_temp p f value_e.Ast.pos in
             put f (ins_abc (if subj_is_text then op_eqs else op_eq) t cmp_reg b);
             let jz = here f in
             put f (ins_asbx op_jz t 0);
             if j < n - 1 then begin
               let jmp = here f in
               put f (ins_asbx op_jmp 0 0);
               patch_jump p f ~file:f.f_file ~pos:value_e.Ast.pos jz (here f);
               to_body := jmp :: !to_body
             end
             else pending_fail := jz :: !pending_fail)
           arm.Ast.values;
         let body_start = here f in
         List.iter
           (fun pc -> patch_jump p f ~file:f.f_file ~pos:arm.Ast.arm_pos pc body_start)
           !to_body
       end);
      let saved_locals = f.f_nlocals in
      let saved_env = f.f_env in
      let saved_decls = f.f_declared in
      (* haxe-parity Task 4: payload bindings — GETF the variant object's
         fields into fresh arm-locals before the body runs. Plain
         registers holding borrows of the subject's own fields: never in
         a drop set (owner.ml declares them l_holds = false), undone by
         the same env/locals restore every arm already gets. *)
      let arm_binds = ref [] in
      (match subj_union_deep with
      | Some u when u.Types.u_has_payload -> (
        match arm.Ast.values with
        | [ { Ast.kind = Ast.Call ({ Ast.kind = Ast.Ident vname; _ }, bargs); _ } ] -> (
          match
            List.find_opt
              (fun (vi : Types.variant_info) -> vi.Types.vi_name = vname)
              u.Types.u_variants
          with
          | Some vi when List.length bargs = List.length vi.Types.vi_fields ->
            List.iteri
              (fun idx (barg : Ast.expr) ->
                match barg.Ast.kind with
                | Ast.Ident bn ->
                  let _, fty = List.nth vi.Types.vi_fields idx in
                  let r = alloc_local p f barg.Ast.pos in
                  f.f_cur_line <- barg.Ast.pos.line;
                  put f (ins_abc op_getf r subj_reg (check_field_idx p f barg.Ast.pos idx));
                  f.f_env <- (bn, (r, fty)) :: f.f_env;
                  arm_binds := (bn, r, idx, fty) :: !arm_binds
                | _ -> ())
              bargs
          | _ -> ())
        | _ -> ())
      | _ -> ());
      (match List.rev arm.Ast.body with
      | [] -> ()
      | last :: rev_init ->
        List.iter (emit_stmt p f v) (List.rev rev_init);
        (match last.Ast.s_kind with
        | Ast.ExprStmt ve ->
          stmt_reset f;
          f.f_cur_line <- last.Ast.s_pos.line;
          emit_expr p f v ~dst ve;
          (* Task 4 fix round 1 (Critical 1a/1c): the arm yields its own
             payload binding — a MOVE OUT of the variant object. The
             payload pointer just landed in `dst` (its new owner's
             register); null the shell's field so the shell's ordinary
             recursive drop plan — which already skips zero slots
             (runtime/src/gc.c wo_drop_kind) — frees the shell only,
             never the escaped payload. Without this, the subject's
             scope-end drop and the new owner's drop both freed the
             payload: a real reviewer-reproduced double free. Gated on
             `want_value` (a discarded yield must leave the shell whole)
             and on the name still resolving to the BINDING's own
             register (an arm-local `let` shadowing the binding is an
             ordinary yield, not an escape).

             Fix round 2: pointer-kind payload fields ONLY. Escaping a
             SCALAR field (Int/Bool/Timestamp/Id/ref, a bare-union tag)
             is a COPY — there is no ownership to move, nothing the
             shell's drop plan would double-free, and the SETF-0 was
             indistinguishable from a legitimate 0: the reviewer's m3d
             probe re-switched the same subject and read 0 where 5
             lived. Kind 0 is WO_K_SCALAR (runtime/src/wob.h). *)
          (if want_value then
             match ve.Ast.kind with
             | Ast.Ident n -> (
               match List.find_opt (fun (bn, _, _, _) -> bn = n) !arm_binds with
               | Some (_, breg, fidx, bfty)
                 when (match lookup_local f n with Some (r, _) -> r = breg | None -> false)
                      && field_kind p bfty <> 0 ->
                 let save = f.f_temp in
                 let z = alloc_temp p f ve.Ast.pos in
                 put f (ins_abx op_loadk z (check_bx p f ve.Ast.pos "constant" (const_int p 0)));
                 put f (ins_abc op_setf subj_reg (check_field_idx p f ve.Ast.pos fidx) z);
                 f.f_temp <- save
               | _ -> ())
             | _ -> ());
          (match Hashtbl.find_opt v.v_move ve.id with
          | Some place -> ( match lookup_local f place with Some (sr, _) -> mask_clear f sr | None -> ())
          | None -> ())
        | _ -> emit_stmt p f v last));
      emit_scope_drops p f v ~node:e.id ~label;
      emit_rc p f v ~node:e.id ~acquire:false ~groups:(declared_since f saved_decls) ();
      f.f_nlocals <- saved_locals;
      f.f_env <- saved_env;
      f.f_declared <- saved_decls;
      f.f_temp <- saved_locals;
      emit_join_drops p f v ~node:e.id ~label;
      (if not f.f_div then begin
         f.f_cur_line <- arm.Ast.arm_pos.line;
         let pc = here f in
         put f (ins_asbx op_jmp 0 0);
         end_jumps := pc :: !end_jumps
       end);
      arm_results := (f.f_owned, f.f_gc, f.f_div) :: !arm_results)
    (Ast.switch_lowering_order arms);
  let exit_pc = here f in
  List.iter (fun pc -> patch_jump p f ~file:f.f_file ~pos:e.pos pc exit_pc) !pending_fail;
  List.iter (fun pc -> patch_jump p f ~file:f.f_file ~pos:e.pos pc exit_pc) !end_jumps;
  f.f_nlocals <- saved_nlocals;
  match List.filter (fun (_, _, d) -> not d) (List.rev !arm_results) with
  | [] -> f.f_div <- true
  | (o0, g0, _) :: rest ->
    f.f_owned <- o0;
    f.f_gc <- g0;
    List.iter (fun (o, g, _) -> mask_meet f o g) rest;
    f.f_div <- div0

(* haxe-parity Task 5: `try body catch (e) handler`.

     TRY ereg, ->handler     register the region; ereg is where the error
     <body -> dst>           record lands if it fires
     ENDTRY                  the region completed: pop it
     JMP ->exit
   handler:
     NEW ereg, Error         the record is the compiler's allocation, so
     BUILTIN ereg, err_fill  its drop is the ordinary scope-end one
     <handler -> dst>
     <scope drops, join drops>
   exit:

   The two arms are joined exactly like a switch's: each arm's ending
   owned/gc masks meet, and each drops what the other moved
   (emit_join_drops with the labels owner.ml's analyze_try recorded). The
   VM releases whatever the body itself owned before landing — the live
   mask at the try site is what it reads — so the handler starts from the
   entry state, which is what the owner pass assumed. *)
and emit_try (p : pctx) (f : fstate) (v : views) ~(dst : int) ?expected (e : Ast.expr)
    (body : Ast.expr) (ename : string) (handler : Ast.stmt list) : unit =
  (* dst is reserved for the whole construct — same reason emit_switch
     does it: a handler-local `let` must never be handed dst's register *)
  let saved_nlocals = f.f_nlocals in
  if f.f_nlocals <= dst then f.f_nlocals <- dst + 1;
  if f.f_temp <= dst then f.f_temp <- dst + 1;
  bump f dst;
  let ereg = alloc_local p f e.pos in
  f.f_cur_line <- e.pos.line;
  let try_pc = here f in
  put f (ins_asbx op_try ereg 0);
  let entry_owned = f.f_owned and entry_gc = f.f_gc in
  let div0 = f.f_div in
  (match expected with
  | Some t -> emit_expr p f v ~dst ~expected:t body
  | None -> emit_expr p f v ~dst body);
  f.f_cur_line <- e.pos.line;
  put f (ins_abc op_endtry 0 0 0);
  emit_join_drops p f v ~node:e.id ~label:"TRYBODY";
  let body_owned = f.f_owned and body_gc = f.f_gc and body_div = f.f_div in
  let skip_pc = here f in
  put f (ins_asbx op_jmp 0 0);
  patch_jump p f ~file:f.f_file ~pos:e.pos try_pc (here f);
  f.f_owned <- entry_owned;
  f.f_gc <- entry_gc;
  f.f_div <- div0;
  let saved_env = f.f_env and saved_decls = f.f_declared in
  let saved_locals = f.f_nlocals in
  (match class_of_name p Types.error_record_name with
  | Some ecid ->
    f.f_cur_line <- e.pos.line;
    put f (ins_abx op_new ereg (check_bx p f e.pos "class" ecid));
    put f (ins_abc op_builtin ereg ereg b_err_fill)
  | None ->
    err p ~code:cannot_lower_code ~file:f.f_file ~pos:e.pos
      ~message:"no class-table entry for the `Error` record — a `try` cannot bind its error";
    put f (ins_abx op_loadk ereg (const_int p 0)));
  f.f_env <- (ename, (ereg, Ast.Scalar Types.error_record_name)) :: f.f_env;
  Hashtbl.replace f.f_decl e.id ereg;
  f.f_declared <- e.id :: f.f_declared;
  mask_set f (match Hashtbl.find_opt v.v_holder e.id with Some k -> k | None -> Owner.LOwned) ereg;
  (match List.rev handler with
  | [] -> ()
  | last :: rev_init -> (
    List.iter (emit_stmt p f v) (List.rev rev_init);
    match last.Ast.s_kind with
    | Ast.ExprStmt ve ->
      stmt_reset f;
      f.f_cur_line <- last.Ast.s_pos.line;
      (match expected with
      | Some t -> emit_expr p f v ~dst ~expected:t ve
      | None -> emit_expr p f v ~dst ve)
    | _ -> emit_stmt p f v last));
  emit_scope_drops p f v ~node:e.id ~label:"CATCH";
  emit_rc p f v ~node:e.id ~acquire:false ~groups:(declared_since f saved_decls) ();
  f.f_nlocals <- saved_locals;
  f.f_env <- saved_env;
  f.f_declared <- saved_decls;
  f.f_temp <- saved_locals;
  emit_join_drops p f v ~node:e.id ~label:"CATCHJOIN";
  patch_jump p f ~file:f.f_file ~pos:e.pos skip_pc (here f);
  f.f_nlocals <- saved_nlocals;
  (* the state after the try is what both arms agree on *)
  if body_div then ()
  else if f.f_div then begin
    f.f_owned <- body_owned;
    f.f_gc <- body_gc;
    f.f_div <- div0
  end
  else begin
    mask_meet f body_owned body_gc;
    f.f_div <- div0
  end

and emit_ctor (p : pctx) (f : fstate) (v : views) ~(dst : int) (e : Ast.expr) (cn : string)
    (fields : (string * Ast.expr) list) : unit =
  match class_of_name p cn with
  | None ->
    err p ~code:cannot_lower_code ~file:f.f_file ~pos:e.pos
      ~message:(Printf.sprintf "constructor of `%s`, which is not a declared class" cn);
    put f (ins_abx op_loadk dst (const_int p 0))
  | Some cid ->
    put f (ins_abx op_new dst (check_bx p f e.pos "class" cid));
    (* dst holds the live object pointer every SETF below reads -- but
       in tail position (`return Box{...}`, emit_tail's allocate-then-
       un-reserve convention) dst sits AT f_temp, so the field-value
       temp would be dst itself and the value-write would clobber the
       pointer before SETF reads it (NEW r0; LOADK r0; SETF r0,f0,r0 --
       an int64 stored through as a heap pointer). Reserve dst past the
       field loop; same guard emit_switch uses for its placeholder dst. *)
    let outer = f.f_temp in
    if f.f_temp <= dst then f.f_temp <- dst + 1;
    List.iter
      (fun ((fname : string), (fe : Ast.expr)) ->
        match field_of p cid fname with
        | None ->
          err p ~code:cannot_lower_code ~file:f.f_file ~pos:e.pos
            ~message:(Printf.sprintf "`%s` has no field `%s`" cn fname)
        | Some (idx, fty) ->
          (* one temporary, reused for every field: each field's value is
             dead the instant its SETF retires, so holding a slot per
             field would make a wide class (the normal shape of a @table
             class) burn the register budget for nothing. Same
             save/restore the call and builtin windows use. *)
          let save = f.f_temp in
          let t = alloc_temp p f fe.pos in
          emit_expr p f v ~dst:t ~expected:fty fe;
          f.f_cur_line <- fe.pos.line;
          put f (ins_abc op_setf dst (check_field_idx p f e.pos idx) t);
          f.f_temp <- save)
      fields;
    (* haxe-parity Task 4: fields the literal omitted. A declared
       default is emitted and stored (this is what makes `TailState {}`
       construct — the sample's own defaults-fill-in pattern); a
       `?`-typed field with no default stays the zero word NEW left
       (nil-by-shape). Appended AFTER the provided-field loop, never
       interleaved, so a literal that provides every field emits
       byte-identical code to before this task. A field that is neither
       provided, defaulted, nor nullable was already WO-E206 upstream —
       no image is written, so no arm is needed here. *)
    (match Types.StringMap.find_opt cn p.p_syms.Types.classes with
    | None -> ()
    | Some (ci : Types.class_info) ->
      let provided = List.map fst fields in
      List.iter
        (fun (fname, _, fdefault, _) ->
          match fdefault with
          | Some d when not (List.mem fname provided) -> (
            match field_of p cid fname with
            | None -> ()
            | Some (idx, fty) ->
              let save = f.f_temp in
              let t = alloc_temp p f e.pos in
              emit_default_value p f ~dst:t ~fty ~pos:e.pos d;
              f.f_cur_line <- e.pos.line;
              put f (ins_abc op_setf dst (check_field_idx p f e.pos idx) t);
              f.f_temp <- save)
          | _ -> ())
        ci.Types.fields);
    f.f_temp <- outer

(* The default expressions the emitter can lower (haxe-parity Task 4):
   the literal shapes the sample's own typedefs use — Int (optionally
   negated), Text, Bool, `now()` (parse_default_expr's own recognized
   case), and `[]` (an empty container, kinds from the field's declared
   type — the same container_imm rule multi_new/map_new already
   follow). A default is an opaque token span by design (ast.ml), so
   anything richer is WO-E403 — a diagnostic, never invented bytecode. *)
and emit_default_value (p : pctx) (f : fstate) ~(dst : int) ~(fty : Ast.field_ty)
    ~(pos : Ast.pos) (d : Ast.default_expr) : unit =
  let bad () =
    err p ~code:cannot_lower_code ~file:f.f_file ~pos
      ~message:
        "cannot lower this field default — only Int/Text/Bool literals, `nil`, `now()`, `[]`, \
         `{}` and `Rec {}` are supported";
    put f (ins_abx op_loadk dst (const_int p 0))
  in
  match d with
  | Ast.DefaultNow -> put f (ins_abc op_builtin dst dst b_now)
  | Ast.DefaultOpaque toks -> (
    match List.map (fun (t : Token.t) -> t.Token.kind) toks with
    (* `= nil` — the field's own nil word (zero for a heap shape, the sentinel
       for a `?scalar`) *)
    | [ Token.KwNil ] ->
      put f
        (ins_abx op_loadk dst
           (check_bx p f pos "constant"
              (const_int p (if is_nullable_scalar p fty then nil_scalar_word else 0))))
    (* `= {}` — a fresh empty container of the field's own declared type,
       the same rule `[]` above follows *)
    | [ Token.LBrace; Token.RBrace ] -> (
      match container_imm p (Some fty) (match unwrap fty with Map _ -> true | _ -> false) with
      | Some imm ->
        put f
          (ins_abc op_builtin dst imm (match unwrap fty with Map _ -> b_map_new | _ -> b_multi_new))
      | None -> bad ())
    (* `= Rec {}` — a record built from ITS own field defaults, which is how
       the workload's `st: TailState = TailState {}` self-initializes. Only
       the empty literal: a default with real field values is a general
       expression, and defaults are an opaque token span by design. *)
    | [ Token.Ident cn; Token.LBrace; Token.RBrace ] -> (
      match class_of_name p cn with
      | None -> bad ()
      | Some cid ->
        put f (ins_abx op_new dst (check_bx p f pos "class" cid));
        let outer = f.f_temp in
        if f.f_temp <= dst then f.f_temp <- dst + 1;
        (match Types.StringMap.find_opt cn p.p_syms.Types.classes with
        | None -> ()
        | Some (ci : Types.class_info) ->
          List.iter
            (fun (fname, _, fdefault, _) ->
              match fdefault with
              | Some d2 -> (
                match field_of p cid fname with
                | None -> ()
                | Some (idx, fty2) ->
                  let save = f.f_temp in
                  let t = alloc_temp p f pos in
                  emit_default_value p f ~dst:t ~fty:fty2 ~pos d2;
                  f.f_cur_line <- pos.line;
                  put f (ins_abc op_setf dst (check_field_idx p f pos idx) t);
                  f.f_temp <- save)
              | None -> ())
            ci.Types.fields);
        f.f_temp <- outer)
    | [ Token.Int n ] -> put f (ins_abx op_loadk dst (check_bx p f pos "constant" (const_int p n)))
    | [ Token.Dash; Token.Int n ] ->
      put f (ins_abx op_loadk dst (check_bx p f pos "constant" (const_int p (-n))))
    | [ Token.Str s ] -> put f (ins_abx op_loadk dst (check_bx p f pos "constant" (const_text p s)))
    | [ Token.KwTrue ] -> put f (ins_abx op_loadk dst (const_int p 1))
    | [ Token.KwFalse ] -> put f (ins_abx op_loadk dst (const_int p 0))
    | [ Token.LBracket; Token.RBracket ] -> (
      match container_imm p (Some fty) (match unwrap fty with Map _ -> true | _ -> false) with
      | Some imm ->
        put f
          (ins_abc op_builtin dst imm
             (match unwrap fty with Map _ -> b_map_new | _ -> b_multi_new))
      | None -> bad ())
    | _ -> bad ())

(* haxe-parity Task 4: variant construction (`Failed("boom")`). NEW of
   the variant's own class (its id IS the tag — the header carries it,
   nothing else to set), then positional SETFs, exactly a constructor
   literal with positional instead of named fields. Same dst-reservation
   guard as emit_ctor (tail position hands this a dst at f_temp).
   Arity was already WO-E203 upstream (types.ml); this keeps the
   emitter's own belt-and-braces WO-E403 like every call path does. *)
and emit_variant_ctor (p : pctx) (f : fstate) (v : views) ~(dst : int) (e : Ast.expr)
    (u : Types.union_info) (vi : Types.variant_info) (args : Ast.expr list) : unit =
  if
    not
      (check_arity p f e
         ~what:(Printf.sprintf "variant `%s` of `%s`" vi.Types.vi_name u.Types.u_name)
         ~want:(List.length vi.Types.vi_fields) ~got:(List.length args))
  then put f (ins_abx op_loadk dst (const_int p 0))
  else if not u.Types.u_has_payload then
    (* a bare-union variant "called" with its zero arguments is the same
       value as the bare reference: the ordinal tag *)
    put f (ins_abx op_loadk dst (check_bx p f e.pos "constant" (const_int p vi.Types.vi_tag)))
  else begin
    put f (ins_abx op_new dst (check_bx p f e.pos "class" (variant_tag_value p u vi)));
    let outer = f.f_temp in
    if f.f_temp <= dst then f.f_temp <- dst + 1;
    List.iteri
      (fun idx (ae : Ast.expr) ->
        match List.nth_opt vi.Types.vi_fields idx with
        | None -> ()
        | Some (_, fty) ->
          let save = f.f_temp in
          let t = alloc_temp p f ae.pos in
          emit_expr p f v ~dst:t ~expected:fty ae;
          f.f_cur_line <- ae.pos.line;
          put f (ins_abc op_setf dst (check_field_idx p f e.pos idx) t);
          f.f_temp <- save)
      args;
    f.f_temp <- outer
  end

(* ---- calls ---------------------------------------------------------

   The window convention (runtime/src/vm.c's CALL / ICALL): the callee's
   r0 is the caller's slot A, arguments occupy A..A+argc-1, and the
   return value lands back in A. `arg_cnt` counts `self`, so a method
   call places the receiver at A and its arguments from A+1.

   The whole window is reserved before any argument is evaluated: a
   nested call inside argument i then gets a window above the outer one
   and cannot clobber an already-filled slot. *)
and emit_call (p : pctx) (f : fstate) (v : views) ~(dst : int) ?expected (e : Ast.expr)
    (callee : Ast.expr) (args : Ast.expr list) : unit =
  ignore expected;
  match callee.kind with
  | Ident name -> (
    (* haxe-parity Task 1 (modules), CRITICAL 1 review fix: this file's
       own module's own free_fns first — never p_syms' flat merge — so
       an own-module bare call to a name that happens to collide with
       some other module's same-named `pub fn` still resolves to *this*
       module's declaration, not whichever one the flat merge kept.
       Falls back to p_syms only when the own module doesn't declare it
       at all (a bare call resolving through a single `use`d module, or
       a builtin) — every pre-existing, non-colliding case behaves
       exactly as before: a name with only one declaration anywhere is
       never mangled either way, so free_fn_key returns it unchanged. *)
    let own_mid = p.p_module_of f.f_file in
    let own_fi =
      match Hashtbl.find_opt p.p_module_syms own_mid with
      | Some msyms -> Types.StringMap.find_opt name msyms.Types.free_fns
      | None -> None
    in
    match own_fi with
    | Some fi ->
      emit_direct p f v ~dst e ~key:(free_fn_key p.p_colliding own_mid name) ~recv:None
        ~params:fi.Types.params args
    | None -> (
      match free_fn p name with
      | Some fi -> emit_direct p f v ~dst e ~key:name ~recv:None ~params:fi.Types.params args
      | None -> (
        (* haxe-parity Task 4: variant construction by name. A declared
           free fn of the same name already won above (the same
           shadowing rule builtins follow). *)
        match find_variant p name with
        | Some (u, vi) -> emit_variant_ctor p f v ~dst e u vi args
        | None ->
          if is_builtin_name name then emit_builtin p f v ~dst ?expected e name args
          else begin
            err p ~code:cannot_lower_code ~file:f.f_file ~pos:e.pos
              ~message:(Printf.sprintf "call to `%s`, which is not a declared fn or a builtin" name);
            put f (ins_abx op_loadk dst (const_int p 0))
          end)))
  | Field (base, mname) -> (
    match ty_of_expr p f base with
    | Some bt -> (
      match unwrap bt with
      | Scalar cn -> (
        match class_method p cn mname with
        | Some mi when mi.Types.is_static ->
          (* haxe-parity Task 7: a static has no receiver — reaching it
             through an instance is an error, not an implicit `Cls.` *)
          err p ~code:cannot_lower_code ~file:f.f_file ~pos:e.pos
            ~message:
              (Printf.sprintf "`%s` is a `static fn` — call it as `%s.%s(...)`, not on an instance"
                 mname cn mname);
          put f (ins_abx op_loadk dst (const_int p 0))
        | Some mi ->
          emit_direct p f v ~dst e ~key:(cn ^ "." ^ mname) ~recv:(Some base) ~params:mi.Types.params
            args
        | None -> (
          match iface_method p cn mname with
          | Some (slot, sg) -> emit_iface p f v ~dst e ~slot ~base ~params:sg.Types.params args
          | None ->
            err p ~code:cannot_lower_code ~file:f.f_file ~pos:e.pos
              ~message:(Printf.sprintf "`%s` has no method `%s`" cn mname);
            put f (ins_abx op_loadk dst (const_int p 0))))
      | _ ->
        err p ~code:cannot_lower_code ~file:f.f_file ~pos:e.pos
          ~message:(Printf.sprintf "method `%s` called on a value that is not a class instance" mname);
        put f (ins_abx op_loadk dst (const_int p 0)))
    | None -> (
      (* haxe-parity Task 1 (modules): before falling to the generic
         "cannot resolve the receiver" error, check whether `base` is
         actually a `use` alias for this file rather than an
         unresolvable receiver expression — a genuine local/param/self
         receiver is always caught by the `Some bt` arm above (ty_of_expr
         resolves it), so this is reached only when `base` names nothing
         ty_of_expr knows, which is exactly the shape a bare module alias
         has. types.ml's check_modules already proved the reference
         legitimate (own module unconditionally, a used module only its
         `pub` names) before the compile got this far — that front-door
         check decided whether this call was even allowed, not anything
         here. Two shapes reach emission:
         - a stdlib-reserved alias (`fs`/`proc`/...): nothing to lower to
           yet (member signatures arrive in plan 9) — WO-E406, the one
           place this milestone still says so, and only because this
           call actually survived to emission (an unused `use fs` never
           reaches this code at all).
         - a project-module alias: resolves through *that module's own*
           symbols (p_module_syms), never p_syms' flat merge (CRITICAL 1
           review fix) — p_syms may have silently dropped this exact
           module's declaration in favor of some other module's
           same-named `pub fn` (main.ml's merge_symbols is first-wins
           across the whole discovered tree, oblivious to modules). This
           is what makes `a.thing()` and `b.thing()` genuinely distinct
           once qualification disambiguates them, not two spellings of
           whichever one the flat merge happened to keep. *)
      match base.kind with
      (* haxe-parity Task 7: `Flock.held(path)` — the base names a class,
         so there is no receiver to pass and the window holds parameters
         only (`recv:None`, exactly like a free fn). Checked before the
         `use`-alias reading: both are bare Idents at this point. *)
      | Ident cls_name when static_method p cls_name mname <> None -> (
        match static_method p cls_name mname with
        | Some mi ->
          emit_direct p f v ~dst e ~key:(cls_name ^ "." ^ mname) ~recv:None ~params:mi.Types.params
            args
        | None -> ())
      | Ident alias -> (
        match use_edge_for p ~file:f.f_file alias with
        | Some u when u.Types.ue_is_stdlib -> (
          (* the systems stdlib: one builtin per member. A member whose
             result is a record takes that record's class id as its last
             argument, so the VM allocates what it fills (sysio.c). *)
          match (if alias = "json" then None else Types.stdlib_member alias mname) with
          | None when alias = "json" && mname = "encode" -> (
            (* json.encode(x): the VM needs x's STATIC kind, since a register
               alone cannot say whether it holds an i64 or a pointer.
               Everything below the top level comes from object headers and
               the class table (runtime/src/json.c). *)
            match args with
            | [ a ] ->
              let base = alloc_temps p f e.pos 2 in
              let save = f.f_temp in
              emit_expr p f v ~dst:base a;
              f.f_temp <- save;
              let kind =
                match ty_of_expr p f a with
                (* a `json.Value` is raw JSON already: kind 255 tells the
                   builtin to emit it verbatim instead of quoting it *)
                | Some t when (match unwrap t with Scalar n -> n = Types.json_value_type | _ -> false)
                  -> 255
                | Some t -> field_kind p t
                | None -> 3 (* Text *)
              in
              put f (ins_abx op_loadk (base + 1) (check_bx p f e.pos "constant" (const_int p kind)));
              sync_mask p f v e.id;
              f.f_cur_line <- e.pos.line;
              put f (ins_abc op_builtin dst base b_json_encode)
            | _ ->
              err p ~code:cannot_lower_code ~file:f.f_file ~pos:e.pos
                ~message:"`json.encode` takes exactly one argument";
              put f (ins_abx op_loadk dst (const_int p 0)))
          | None when alias = "json" && mname = "decode" ->
            err p ~code:cannot_lower_code ~file:f.f_file ~pos:e.pos
              ~message:
                "`json.decode(text)` needs a target type — write `json.decode(text) as T`, whose \
                 result is `?T`";
            put f (ins_abx op_loadk dst (const_int p 0))
          | None ->
            err p ~code:stdlib_not_linked_code ~file:f.f_file ~pos:e.pos
              ~message:(Printf.sprintf "stdlib module `%s` has no member `%s`" alias mname);
            put f (ins_abx op_loadk dst (const_int p 0))
          | Some sm ->
            if List.length args <> sm.Types.sm_arity then begin
              err p ~code:cannot_lower_code ~file:f.f_file ~pos:e.pos
                ~message:
                  (Printf.sprintf "`%s.%s` takes %d argument(s), given %d" alias mname
                     sm.Types.sm_arity (List.length args));
              put f (ins_abx op_loadk dst (const_int p 0))
            end
            else begin
              let extra = match sm.Types.sm_record with Some _ -> 1 | None -> 0 in
              let n = sm.Types.sm_arity + extra in
              let base = alloc_temps p f e.pos (max n 1) in
              List.iteri
                (fun i (a : Ast.expr) ->
                  let save = f.f_temp in
                  emit_expr p f v ~dst:(base + i) a;
                  f.f_temp <- save)
                args;
              (match sm.Types.sm_record with
              | Some rec_name -> (
                match class_of_name p rec_name with
                | Some cid ->
                  put f
                    (ins_abx op_loadk (base + sm.Types.sm_arity)
                       (check_bx p f e.pos "constant" (const_int p cid)))
                | None ->
                  err p ~code:cannot_lower_code ~file:f.f_file ~pos:e.pos
                    ~message:
                      (Printf.sprintf "no class-table entry for the `%s` record — `%s.%s` cannot \
                                       build its result"
                         rec_name alias mname))
              | None -> ());
              sync_mask p f v e.id;
              f.f_cur_line <- e.pos.line;
              put f (ins_abc op_builtin dst base sm.Types.sm_builtin)
            end)
        | Some u -> (
          let target_mid = Types.path_str u.Types.ue_segments in
          let target_fi =
            match Hashtbl.find_opt p.p_module_syms target_mid with
            | Some msyms -> Types.StringMap.find_opt mname msyms.Types.free_fns
            | None -> None
          in
          match target_fi with
          | Some fi ->
            emit_direct p f v ~dst e ~key:(free_fn_key p.p_colliding target_mid mname) ~recv:None
              ~params:fi.Types.params args
          | None ->
            err p ~code:cannot_lower_code ~file:f.f_file ~pos:e.pos
              ~message:
                (Printf.sprintf "call to `%s.%s`, which is not a declared fn in module `%s`" alias mname
                   alias);
            put f (ins_abx op_loadk dst (const_int p 0)))
        | None ->
          err p ~code:cannot_lower_code ~file:f.f_file ~pos:e.pos
            ~message:(Printf.sprintf "cannot resolve the receiver's type for the call to `%s`" mname);
          put f (ins_abx op_loadk dst (const_int p 0)))
      | _ ->
        err p ~code:cannot_lower_code ~file:f.f_file ~pos:e.pos
          ~message:(Printf.sprintf "cannot resolve the receiver's type for the call to `%s`" mname);
        put f (ins_abx op_loadk dst (const_int p 0))))
  | _ ->
    err p ~code:cannot_lower_code ~file:f.f_file ~pos:e.pos
      ~message:"only a name or a `receiver.method` form can be called";
    put f (ins_abx op_loadk dst (const_int p 0))

(* The call-site argument count has to match the callee's, or the window
   the caller reserves is not the window the callee reads: the loader's
   own "call window exceeds frame" check rejects the image, and a
   too-wide call would hand the callee whatever the caller happened to
   leave in those registers. Nothing upstream catches this — types.ml
   declares WO-E203 for bad arity and never raises it (see the error
   catalog) — so the emitter is where it stops. *)
and check_arity (p : pctx) (f : fstate) (e : Ast.expr) ~(what : string) ~(want : int)
    ~(got : int) : bool =
  if want = got then true
  else begin
    err p ~code:cannot_lower_code ~file:f.f_file ~pos:e.pos
      ~message:
        (Printf.sprintf "%s takes %d argument(s), given %d" what want got);
    false
  end

(* The third return component (Task 4 fix rounds 1-2): stable registers
   holding OWNED HEAP temporaries passed by borrow — a construction or
   an owned-returning call as an argument (`get(Boxed(Pay{}))`,
   `peek(Pay{})` — the reviewer's h5/m5c per-iteration leaks) is a
   fresh owned value NOBODY owned before this fix: it is not a place,
   so owner.ml never tracks it, and no scope ever dropped it. The
   borrow convention means the callee never takes it either (a callee
   that stores or returns the borrow is already WO-E304), so the CALLER
   reaps it: each qualifying argument's pointer is copied to a slot
   below the window (the callee's frame overlaps the window and may
   overwrite the arg slot itself — same reasoning as the residual-guard
   gbase) and DROPped by emit_direct/emit_iface once the call returns.
   Recursive drop is correct both ways: a payload the callee moved out
   (the switch escape above) left the field nulled, which the drop plan
   skips; an untouched temp frees payload and shell together.
   Emitter-only, no owner table: the temp never exists in owner.ml's
   world, so there is no entry to conflict with; the one imprecision is
   a trap DURING the call (the frame's drop mask cannot name a
   register owner.ml never saw — the temp leaks on that path, the same
   pre-existing behavior every expression temporary has). *)
and call_window (p : pctx) (f : fstate) (v : views) (e : Ast.expr) ~(recv : Ast.expr option)
    ~(params : (string * Ast.field_ty * Ast.param_conv) list) (args : Ast.expr list) :
    int * int * int list =
  let nrecv = match recv with Some _ -> 1 | None -> 0 in
  let argc = nrecv + List.length args in
  (* Fix round 2 widened this from payload-union temps to ANY owned heap
     temporary: the sibling leak (`peek(Pay{})`, a plain record ctor in
     a loop — reviewer's m5c, 10,780 KB vs a 1,532 KB control) was the
     same fresh-value-nobody-owns shape with a class instead of a
     union. Qualifies: a Ctor literal, a variant construction, or a
     call whose result is a payload union or a declared NON-@gc class
     (records included — same table). Excluded, each for its own
     reason: `take` params (a real transfer — the callee owns it,
     m6a/m6c prove that path flat); places (their scope drops them);
     @gc classes (their creating reference is the rc system's, and DROP
     is the wrong op for a counted handle); Text (owner.ml classes it
     Copy, so a callee may legitimately STORE a Text argument — classes
     and unions can't, WO-E304 polices those borrows); interface-typed
     call results (ty_of_expr names the interface, not the concrete
     class — still leak, disclosed). *)
  let owned_heap_temp (a : Ast.expr) : bool =
    (match a.kind with
    | Ident n -> lookup_local f n = None (* a local is a place, never a temp *)
    | Call _ | Ctor _ -> true
    | _ -> false)
    && (match ty_of_expr p f a with
       | Some t -> (
         match unwrap t with
         | Scalar n -> (
           match find_union p n with
           | Some u -> u.Types.u_has_payload
           | None -> (
             match Types.StringMap.find_opt n p.p_syms.Types.classes with
             | Some (ci : Types.class_info) -> not ci.Types.is_gc
             | None -> false))
         | _ -> false)
       | None -> false)
  in
  let temp_idx =
    List.mapi
      (fun i a ->
        let conv = match List.nth_opt params i with Some (_, _, c) -> c | None -> Ast.Borrow in
        if conv = Ast.Borrow && owned_heap_temp a then Some i else None)
      args
    |> List.filter_map Fun.id
  in
  let tbase = alloc_temps p f e.pos (List.length temp_idx) in
  let base = alloc_temps p f e.pos (max argc 1) in
  (match recv with None -> () | Some r -> emit_expr p f v ~dst:base r);
  List.iteri
    (fun i a ->
      let expected = match List.nth_opt params i with Some (_, t, _) -> Some t | None -> None in
      let slot = base + nrecv + i in
      let save = f.f_temp in
      (match expected with
      | Some t -> emit_expr p f v ~dst:slot ~expected:t a
      | None -> emit_expr p f v ~dst:slot a);
      f.f_temp <- save)
    args;
  let temp_drops =
    List.mapi
      (fun j i ->
        let g = tbase + j in
        put f (ins_abc op_move g (base + nrecv + i) 0);
        g)
      temp_idx
  in
  (base, argc, temp_drops)

and emit_direct (p : pctx) (f : fstate) (v : views) ~(dst : int) (e : Ast.expr) ~(key : string)
    ~(recv : Ast.expr option) ~(params : (string * Ast.field_ty * Ast.param_conv) list)
    (args : Ast.expr list) : unit =
  match SM.find_opt key p.p_method_id with
  | None ->
    err p ~code:cannot_lower_code ~file:f.f_file ~pos:e.pos
      ~message:(Printf.sprintf "no emitted method for `%s`" key);
    put f (ins_abx op_loadk dst (const_int p 0))
  | Some midx when check_arity p f e ~what:(Printf.sprintf "`%s`" key)
                     ~want:(List.length params) ~got:(List.length args) ->
    let gbase = alloc_temps p f e.pos (residual_count v e.id) in
    let base, _, temp_drops = call_window p f v e ~recv ~params args in
    let guards = residual_guards p f v e.id (Some gbase) in
    acquire_guards f guards;
    (* the frame's drop map, from the owner table, effective at the CALL *)
    sync_mask p f v e.id;
    f.f_cur_line <- e.pos.line;
    put f (ins_abx op_call base (check_bx p f e.pos "method" midx));
    release_guards f guards;
    (* owned variant temporaries this call borrowed — reaped here, see
       call_window's own doc comment *)
    List.iter (fun g -> put f (ins_abc op_drop g 0 0)) temp_drops;
    if dst <> base then put f (ins_abc op_move dst base 0)
  | Some _ -> put f (ins_abx op_loadk dst (const_int p 0))

and emit_iface (p : pctx) (f : fstate) (v : views) ~(dst : int) (e : Ast.expr) ~(slot : int)
    ~(base : Ast.expr) ~(params : (string * Ast.field_ty * Ast.param_conv) list)
    (args : Ast.expr list) : unit =
  if not (check_arity p f e ~what:"the interface method" ~want:(List.length params)
            ~got:(List.length args)) then
    put f (ins_abx op_loadk dst (const_int p 0))
  else begin
  let gbase = alloc_temps p f e.pos (residual_count v e.id) in
  let w, _, temp_drops = call_window p f v e ~recv:(Some base) ~params args in
  let guards = residual_guards p f v e.id (Some gbase) in
  acquire_guards f guards;
  sync_mask p f v e.id;
  f.f_cur_line <- e.pos.line;
  put f (ins_abx op_icall w (check_bx p f e.pos "interface slot" slot));
  release_guards f guards;
  (* owned variant temporaries this call borrowed — see call_window *)
  List.iter (fun g -> put f (ins_abc op_drop g 0 0)) temp_drops;
  if dst <> w then put f (ins_abc op_move dst w 0)
  end

(* ---- builtins ------------------------------------------------------

   Source spellings of the format doc's BUILTIN ids. `get`/`set` and
   `push`/`count`/`latest`/`has` read on the container they are given, so
   one source name covers a `multi` and a `map` where the runtime has two
   ids. A user-declared free fn of the same name wins (checked before
   this function is reached) — a declared name is never shadowed by a
   builtin. *)
and emit_builtin (p : pctx) (f : fstate) (v : views) ~(dst : int) ?expected (e : Ast.expr)
    (name : string) (args : Ast.expr list) : unit =
  let bad msg =
    err p ~code:cannot_lower_code ~file:f.f_file ~pos:e.pos ~message:msg;
    put f (ins_abx op_loadk dst (const_int p 0))
  in
  let arity_of id =
    if id = b_now || id = b_multi_new || id = b_map_new then 0
    else if
      id = b_print || id = b_print_int || id = b_words || id = b_count || id = b_latest
      || id = b_int_to_text
      (* systems stdlib, one argument *)
      || id = b_len || id = b_print_err || id = b_trim || id = b_to_lower || id = b_char_of
      || id = b_parse_int || id = b_split_ws || id = b_pop || id = b_shift || id = b_sort
      || id = b_reverse
    then 1
    else if
      id = b_multi_push || id = b_multi_get || id = b_map_get || id = b_map_has
      (* systems stdlib, two arguments *)
      || id = b_byte_at || id = b_starts_with || id = b_ends_with || id = b_index_of
      || id = b_last_index_of || id = b_split || id = b_join || id = b_map_remove
      || id = b_map_key_at || id = b_map_val_at
    then 2
    else 3
  in
  let container_id first_arg on_multi on_map =
    match ty_of_expr p f first_arg with
    | Some t -> ( match unwrap t with Multi _ -> Some on_multi | Map _ -> Some on_map | _ -> None)
    | None -> None
  in
  let fixed id =
    let n = arity_of id in
    if List.length args <> n then
      bad (Printf.sprintf "builtin `%s` takes %d argument(s), given %d" name n (List.length args))
    else begin
      let base = alloc_temps p f e.pos (max n 1) in
      List.iteri
        (fun i a ->
          let save = f.f_temp in
          emit_expr p f v ~dst:(base + i) a;
          f.f_temp <- save)
        args;
      sync_mask p f v e.id;
      f.f_cur_line <- e.pos.line;
      put f (ins_abc op_builtin dst base id)
    end
  in
  match name with
  | "now" -> fixed b_now
  | "print" -> fixed b_print
  | "print_int" -> fixed b_print_int
  | "words" -> fixed b_words
  | "count" -> fixed b_count
  | "latest" -> fixed b_latest
  | "int_to_text" -> fixed b_int_to_text
  (* systems stdlib: every one of these resolves to a single id — no
     container-kind branching, no destination immediate (the ones that
     return a fresh container fix their own element kind: `split`/`split_ws`
     are always `multi Text`, `slice` copies its source's kind). *)
  | "len" -> fixed b_len
  | "byte_at" -> fixed b_byte_at
  | "print_err" -> fixed b_print_err
  | "starts_with" -> fixed b_starts_with
  | "ends_with" -> fixed b_ends_with
  | "index_of" -> fixed b_index_of
  | "last_index_of" -> fixed b_last_index_of
  | "substr" -> fixed b_substr
  | "trim" -> fixed b_trim
  | "to_lower" -> fixed b_to_lower
  | "char_of" -> fixed b_char_of
  | "parse_int" -> fixed b_parse_int
  | "split" -> fixed b_split
  | "split_ws" -> fixed b_split_ws
  | "join" -> fixed b_join
  | "slice" -> fixed b_slice
  | "pop" -> fixed b_pop
  | "shift" -> fixed b_shift
  | "sort" -> fixed b_sort
  | "reverse" -> fixed b_reverse
  | "remove" -> fixed b_map_remove
  | "key_at" -> fixed b_map_key_at
  | "val_at" -> fixed b_map_val_at
  | "multi_new" | "map_new" ->
    let is_map = name = "map_new" in
    if args <> [] then bad (Printf.sprintf "builtin `%s` takes no arguments" name)
    else (
      match container_imm p expected is_map with
      | None ->
        bad
          (Printf.sprintf
             "`%s` needs a destination of declared type `%s` — its element kinds are the               container's drop plan and cannot be guessed; build it into a field of that type"
             name
             (if is_map then "map<K, V>" else "multi T"))
      | Some imm ->
        sync_mask p f v e.id;
        f.f_cur_line <- e.pos.line;
        put f (ins_abc op_builtin dst imm (if is_map then b_map_new else b_multi_new)))
  | "push" -> (
    match args with
    | a :: _ -> (
      match container_id a b_multi_push b_multi_push with
      | Some id -> fixed id
      | None -> bad "builtin `push` needs a `multi` as its first argument")
    | [] -> bad "builtin `push` takes 2 arguments, given 0")
  | "get" -> (
    match args with
    | a :: _ -> (
      match container_id a b_multi_get b_map_get with
      | Some id -> fixed id
      | None -> bad "builtin `get` needs a `multi` or a `map` as its first argument")
    | [] -> bad "builtin `get` takes 2 arguments, given 0")
  | "set" -> (
    match args with
    | a :: _ -> (
      match container_id a b_map_set b_map_set with
      | Some id -> fixed id
      | None -> bad "builtin `set` needs a `map` as its first argument")
    | [] -> bad "builtin `set` takes 3 arguments, given 0")
  | "has" -> (
    match args with
    | a :: _ -> (
      match container_id a b_map_has b_map_has with
      | Some id -> fixed id
      | None -> bad "builtin `has` needs a `map` as its first argument")
    | [] -> bad "builtin `has` takes 2 arguments, given 0")
  | _ -> bad (Printf.sprintf "unknown builtin `%s`" name)

(* ---- statements ---------------------------------------------------- *)

and emit_stmt (p : pctx) (f : fstate) (v : views) (s : Ast.stmt) : unit =
  stmt_reset f;
  f.f_cur_line <- s.s_pos.line;
  match s.s_kind with
  | Let { name; ty; value } ->
    let declared = ty in
    let vty =
      match declared with
      | Some t -> t
      | None -> ( match ty_of_expr p f value with Some t -> t | None -> Scalar "Int")
    in
    let r = alloc_local p f s.s_pos in
    (match declared with
    | Some t -> emit_expr p f v ~dst:r ~expected:t value
    | None -> emit_expr p f v ~dst:r value);
    f.f_env <- (name, (r, vty)) :: f.f_env;
    Hashtbl.replace f.f_decl s.s_id r;
    f.f_declared <- s.s_id :: f.f_declared;
    (* a real transfer at this `let` retires the source: the VM's MOVE is
       the move, so the only thing left to do is stop calling the source
       live (the format doc's own words) *)
    (match Hashtbl.find_opt v.v_move value.id with
    | Some place -> ( match lookup_local f place with Some (sr, _) -> mask_clear f sr | None -> ())
    | None -> ());
    (match Hashtbl.find_opt v.v_holder s.s_id with
    | Some kind -> mask_set f kind r
    | None -> ());
    emit_rc p f v ~node:s.s_id ~acquire:true ()
  | Assign { target; value } -> emit_assign p f v s target value
  | ExprStmt ({ kind = Ast.Switch (subj, arms); _ } as e) ->
    (* Task 4 fix round 1: the one place a switch's value is DISCARDED —
       mirrors types.ml's own want_value:false special case. The flag
       gates the payload move-out (see emit_switch's doc comment): a
       discarded binding yield must leave the shell intact. The dst
       convention and the emit_expr epilogue (node register, escape
       increments) are replicated from emit_tail's non-place path so
       this stays byte-identical to the generic path for everything but
       the flag. *)
    let t = alloc_temp p f e.pos in
    f.f_temp <- t;
    f.f_cur_line <- e.pos.line;
    emit_switch ~want_value:false p f v e ~dst:t subj arms;
    Hashtbl.replace f.f_node e.id t;
    emit_rc p f v ~node:e.id ~acquire:true ();
    (match Hashtbl.find_opt v.v_move e.id with
    | Some place -> ( match lookup_local f place with Some (sr, _) -> mask_clear f sr | None -> ())
    | None -> ())
  | ExprStmt e ->
    (* a discarded value lands in the next free temporary *without*
       reserving it: a call then places its own window at that same slot
       and needs no MOVE to hand the result back, and nothing later in
       this statement can want the register (the statement ends here) *)
    ignore (emit_tail p f v e);
    (match Hashtbl.find_opt v.v_move e.id with
    | Some place -> ( match lookup_local f place with Some (sr, _) -> mask_clear f sr | None -> ())
    | None -> ())
  | Return opt -> emit_return p f v s opt
  | If { cond; then_body; else_body } -> emit_if p f v s cond then_body else_body
  | While { cond; body } -> emit_while p f v s cond body
  | For { var; var2; iter; body } -> emit_for p f v s var var2 iter body
  | Break -> emit_break p f v s
  | Continue -> emit_continue p f v s
  | DoWhile { body; cond } -> emit_do_while p f v s body cond

and emit_assign (p : pctx) (f : fstate) (v : views) (s : Ast.stmt) (target : Ast.expr)
    (value : Ast.expr) : unit =
  let overwrite = Hashtbl.mem v.v_overwrite s.s_id in
  (* a @gc value the assignment displaces is released, not dropped: the
     RC table carries that RELEASE at the assignment's own node *)
  let releases =
    match Hashtbl.find_opt v.v_rc s.s_id with
    | None -> false
    | Some sites ->
      List.exists
        (fun (r : Owner.rc_site) ->
          r.Owner.rc_op = Owner.RcRelease && not r.Owner.rc_elided)
        sites
  in
  match target.kind with
  | Ident n -> (
    match lookup_local f n with
    | None ->
      err p ~code:cannot_lower_code ~file:f.f_file ~pos:target.pos
        ~message:(Printf.sprintf "assignment to `%s`, which is not a local or parameter" n)
    | Some (r, ty) ->
      Hashtbl.replace f.f_node target.id r;
      if overwrite || releases then begin
        (* the replaced value dies here (the owner table's OVERWRITE or
           RELEASE entry); compute the new one into a temporary first so
           destroying the old one cannot destroy what is about to be
           stored *)
        let t = alloc_temp p f value.pos in
        emit_expr p f v ~dst:t ~expected:ty value;
        f.f_cur_line <- s.s_pos.line;
        if overwrite then begin
          put f (ins_abc op_drop r 0 0);
          mask_clear f r
        end;
        if releases then begin
          Hashtbl.replace f.f_node s.s_id r;
          emit_rc p f v ~node:s.s_id ~acquire:false ()
        end;
        put f (ins_abc op_move r t 0)
      end
      else emit_expr p f v ~dst:r ~expected:ty value;
      (* a whole-local target cannot be an unprovable alias of anything
         (relate answers Overlap or Disjoint for a place with no
         projections), so this normally finds nothing; consumed anyway so
         the end-of-unit backstop stays exact rather than special-cased *)
      let guards = residual_guards p f v s.s_id None in
      acquire_guards f guards;
      release_guards f guards;
      (match Hashtbl.find_opt v.v_move value.id with
      | Some place -> ( match lookup_local f place with Some (sr, _) -> mask_clear f sr | None -> ())
      | None -> ());
      (* a whole-local assignment re-initializes it: whatever kind of
         holder lived in that register holds again *)
      match Hashtbl.find_opt f.f_kind r with Some kind -> mask_set f kind r | None -> ())
  | Field (base, fname) -> (
    match ty_of_expr p f base with
    | Some bt -> (
      match unwrap bt with
      | Scalar cn -> (
        match class_of_name p cn with
        | None ->
          err p ~code:cannot_lower_code ~file:f.f_file ~pos:target.pos
            ~message:(Printf.sprintf "assignment into `%s`, which is not a declared class" cn)
        | Some cid -> (
          match field_of p cid fname with
          | None ->
            err p ~code:cannot_lower_code ~file:f.f_file ~pos:target.pos
              ~message:(Printf.sprintf "`%s` has no field `%s`" cn fname)
          | Some (idx, fty) ->
            let b = emit_operand p f v base in
            Hashtbl.replace f.f_node target.id b;
            let idx = check_field_idx p f target.pos idx in
            if overwrite || releases then begin
              (* SETF never auto-drops (format doc): the compiler emits
                 the destruction of the field's previous value — a DROP
                 for an owned field, an rc release for a @gc one *)
              let old = alloc_temp p f target.pos in
              f.f_cur_line <- s.s_pos.line;
              put f (ins_abc op_getf old b idx);
              if overwrite then put f (ins_abc op_drop old 0 0);
              if releases then begin
                Hashtbl.replace f.f_node s.s_id old;
                emit_rc p f v ~node:s.s_id ~acquire:false ()
              end
            end;
            let t = alloc_temp p f value.pos in
            emit_expr p f v ~dst:t ~expected:fty value;
            f.f_cur_line <- s.s_pos.line;
            (* an assignment is a region of its own: owner.ml anchors the
               residual sites it produces on the *statement*, not on any
               call expression. Guarded in place — no call intervenes, so
               no operand register can be clobbered — and around the
               mutation only, which is the narrowest correct window. *)
            let guards = residual_guards p f v s.s_id None in
            acquire_guards f guards;
            put f (ins_abc op_setf b idx t);
            release_guards f guards;
            match Hashtbl.find_opt v.v_move value.id with
            | Some place -> ( match lookup_local f place with Some (sr, _) -> mask_clear f sr | None -> ())
            | None -> ()))
      | _ ->
        err p ~code:cannot_lower_code ~file:f.f_file ~pos:target.pos
          ~message:(Printf.sprintf "assignment into field `%s` of a non-class value" fname))
    | None ->
      err p ~code:cannot_lower_code ~file:f.f_file ~pos:target.pos
        ~message:(Printf.sprintf "cannot resolve the type of the value `%s` is written to" fname))
  | Index (base, idx) -> (
    match ty_of_expr p f base with
    | Some bt -> (
      match unwrap bt with
      | Map _ ->
        (* `m[k] = x` is the map_set builtin: container, key, value in
           three consecutive registers *)
        let w = alloc_temps p f s.s_pos 3 in
        emit_expr p f v ~dst:w base;
        Hashtbl.replace f.f_node target.id w;
        emit_expr p f v ~dst:(w + 1) idx;
        emit_expr p f v ~dst:(w + 2) value;
        let sink = alloc_temp p f s.s_pos in
        f.f_cur_line <- s.s_pos.line;
        let guards = residual_guards p f v s.s_id None in
        acquire_guards f guards;
        put f (ins_abc op_builtin sink w b_map_set);
        release_guards f guards
      | Multi _ ->
        (* `m[i] = x` is the multi_set builtin — container, index, value in
           three consecutive registers, exactly the map_set shape above.
           The element it replaces is the container's, so the VM drops it. *)
        let w = alloc_temps p f s.s_pos 3 in
        emit_expr p f v ~dst:w base;
        Hashtbl.replace f.f_node target.id w;
        emit_expr p f v ~dst:(w + 1) idx;
        emit_expr p f v ~dst:(w + 2) value;
        let sink = alloc_temp p f s.s_pos in
        f.f_cur_line <- s.s_pos.line;
        let guards = residual_guards p f v s.s_id None in
        acquire_guards f guards;
        put f (ins_abc op_builtin sink w b_multi_set);
        release_guards f guards
      | _ ->
        err p ~code:cannot_lower_code ~file:f.f_file ~pos:target.pos
          ~message:"element assignment into a value that is neither a `multi` nor a `map`")
    | None ->
      err p ~code:cannot_lower_code ~file:f.f_file ~pos:target.pos
        ~message:"cannot resolve the container's type for an element assignment")
  | _ ->
    err p ~code:cannot_lower_code ~file:f.f_file ~pos:target.pos
      ~message:"assignment target must be a local, a field, or an element"

and emit_return (p : pctx) (f : fstate) (v : views) (s : Ast.stmt) (opt : Ast.expr option) : unit =
  match opt with
  | None ->
    emit_rc p f v ~node:s.s_id ~acquire:true ();
    (match Hashtbl.find_opt v.v_return s.s_id with Some items -> emit_drops p f items | None -> ());
    emit_rc p f v ~node:s.s_id ~acquire:false ();
    f.f_cur_line <- s.s_pos.line;
    put f (ins_abc op_ret0 0 0 0);
    f.f_div <- true
  | Some e ->
    let t = emit_tail p f v e in
    (match Hashtbl.find_opt v.v_move e.id with
    | Some place -> ( match lookup_local f place with Some (sr, _) -> mask_clear f sr | None -> ())
    | None -> ());
    (* the escaping value's own increment was emitted where the value
       landed (emit_expr / emit_tail), which is before the frame's
       releases below — a balanced pair must never reach rc 0 in between *)
    emit_rc p f v ~node:s.s_id ~acquire:true ();
    (match Hashtbl.find_opt v.v_return s.s_id with Some items -> emit_drops p f items | None -> ());
    emit_rc p f v ~node:s.s_id ~acquire:false ();
    f.f_cur_line <- s.s_pos.line;
    put f (ins_abc op_ret t 0 0);
    f.f_div <- true

(* haxe-parity Task 2: `break`/`continue` mirror emit_return's own shape
   exactly (drops from the owner table at this node, then jump) — the
   only difference is *where* the jump lands, which this frame does not
   know yet (the enclosing loop patches it once it does — loop_frame's
   own doc comment). `f.f_div <- true` afterward is the same call
   emit_return makes for the same reason: the rest of this block is
   unreachable, and emit_if's own THEN/ELSE merge already knows how to
   fold that into a branch join (unchanged by this task — a `break`
   inside an `if` inside a loop takes the exact path a `return` there
   already does). Empty `f_loops` (no enclosing loop) is WO-E403: there
   is no legal jump target to lower onto, the same "cannot lower"
   convention every other construct with nothing to lower to uses. *)
and emit_break (p : pctx) (f : fstate) (v : views) (s : Ast.stmt) : unit =
  match f.f_loops with
  | [] ->
    err p ~code:cannot_lower_code ~file:f.f_file ~pos:s.s_pos ~message:"`break` outside of a loop"
  | lf :: _ ->
    emit_rc p f v ~node:s.s_id ~acquire:true ();
    (match Hashtbl.find_opt v.v_break s.s_id with Some items -> emit_drops p f items | None -> ());
    emit_rc p f v ~node:s.s_id ~acquire:false ();
    f.f_cur_line <- s.s_pos.line;
    let pc = here f in
    put f (ins_asbx op_jmp 0 0);
    lf.lf_breaks <- pc :: lf.lf_breaks;
    f.f_div <- true

and emit_continue (p : pctx) (f : fstate) (v : views) (s : Ast.stmt) : unit =
  match f.f_loops with
  | [] ->
    err p ~code:cannot_lower_code ~file:f.f_file ~pos:s.s_pos
      ~message:"`continue` outside of a loop"
  | lf :: _ ->
    emit_rc p f v ~node:s.s_id ~acquire:true ();
    (match Hashtbl.find_opt v.v_continue s.s_id with Some items -> emit_drops p f items | None -> ());
    emit_rc p f v ~node:s.s_id ~acquire:false ();
    f.f_cur_line <- s.s_pos.line;
    let pc = here f in
    put f (ins_asbx op_jmp 0 0);
    lf.lf_continues <- pc :: lf.lf_continues;
    f.f_div <- true

and emit_block (p : pctx) (f : fstate) (v : views) ~(node : int) ~(label : string)
    (body : Ast.stmt list) : unit =
  let saved_locals = f.f_nlocals in
  let saved_env = f.f_env in
  let saved_decls = f.f_declared in
  List.iter (emit_stmt p f v) body;
  (* scope end: the owner table's DROPs first, then the @gc releases for
     the handles this block declared — owner.ml's own pop_scope order *)
  emit_scope_drops p f v ~node ~label;
  emit_rc p f v ~node ~acquire:false ~groups:(declared_since f saved_decls) ();
  f.f_nlocals <- saved_locals;
  f.f_env <- saved_env;
  f.f_declared <- saved_decls;
  f.f_temp <- saved_locals

and emit_if (p : pctx) (f : fstate) (v : views) (s : Ast.stmt) (cond : Ast.expr)
    (then_body : Ast.stmt list) (else_body : (Ast.pos * Ast.stmt list) option) : unit =
  let t = alloc_temp p f s.s_pos in
  emit_expr p f v ~dst:t cond;
  f.f_cur_line <- s.s_pos.line;
  let jz = here f in
  put f (ins_asbx op_jz t 0);
  let entry_owned = f.f_owned and entry_gc = f.f_gc in
  let div0 = f.f_div in
  emit_block p f v ~node:s.s_id ~label:"THEN" then_body;
  emit_join_drops p f v ~node:s.s_id ~label:"THEN";
  let has_else_code =
    (match else_body with Some (_, b) -> b <> [] | None -> false)
    || Hashtbl.mem v.v_join (s.s_id, "ELSE")
    || Hashtbl.mem v.v_scope (s.s_id, "ELSE")
  in
  (* the jump over the else arm still belongs to the then path, so it is
     emitted before the mask goes back to the branch point *)
  let jmp = if has_else_code then Some (here f) else None in
  (match jmp with None -> () | Some _ -> put f (ins_asbx op_jmp 0 0));
  let then_owned = f.f_owned and then_gc = f.f_gc in
  let div_then = f.f_div in
  f.f_owned <- entry_owned;
  f.f_gc <- entry_gc;
  f.f_div <- div0;
  patch_jump p f ~file:f.f_file ~pos:s.s_pos jz (here f);
  (match jmp with
  | None -> ()
  | Some jmp ->
    (match else_body with
    | Some (epos, b) ->
      f.f_cur_line <- epos.line;
      emit_block p f v ~node:s.s_id ~label:"ELSE" b
    | None -> emit_scope_drops p f v ~node:s.s_id ~label:"ELSE");
    emit_join_drops p f v ~node:s.s_id ~label:"ELSE";
    patch_jump p f ~file:f.f_file ~pos:s.s_pos jmp (here f));
  let div_else = f.f_div in
  if div_then && div_else then f.f_div <- true
  else if div_then then f.f_div <- div0 (* the else arm's state stands *)
  else begin
    f.f_div <- div0;
    if not div_else then mask_meet f then_owned then_gc
    else begin
      f.f_owned <- then_owned;
      f.f_gc <- then_gc
    end
  end

and emit_while (p : pctx) (f : fstate) (v : views) (s : Ast.stmt) (cond : Ast.expr)
    (body : Ast.stmt list) : unit =
  let top = here f in
  let entry_owned = f.f_owned and entry_gc = f.f_gc in
  let div0 = f.f_div in
  let t = alloc_temp p f s.s_pos in
  emit_expr p f v ~dst:t cond;
  f.f_cur_line <- s.s_pos.line;
  let jz = here f in
  put f (ins_asbx op_jz t 0);
  let lf = { lf_node = s.s_id; lf_breaks = []; lf_continues = [] } in
  f.f_loops <- lf :: f.f_loops;
  emit_block p f v ~node:s.s_id ~label:"WHILE" body;
  f.f_loops <- List.tl f.f_loops;
  f.f_cur_line <- s.s_pos.line;
  let back = here f in
  put f (ins_asbx op_jmp 0 0);
  patch_jump p f ~file:f.f_file ~pos:s.s_pos back top;
  (* haxe-parity Task 2: `continue` re-enters at the condition check —
     `top`, the exact pc the back-edge above already jumps to; never a
     second copy of the condition. *)
  List.iter (fun pc -> patch_jump p f ~file:f.f_file ~pos:s.s_pos pc top) lf.lf_continues;
  let exit_pc = here f in
  patch_jump p f ~file:f.f_file ~pos:s.s_pos jz exit_pc;
  (* `break` exits to the exact same place the condition's own JZ does. *)
  List.iter (fun pc -> patch_jump p f ~file:f.f_file ~pos:s.s_pos pc exit_pc) lf.lf_breaks;
  (* a loop may run zero times, so the exit state always includes the
     entry state; a body that returned/broke/continued contributes
     nothing. Disclosed, secondary-mechanism approximation (haxe-parity
     Task 2): a `break`'s own mask at the moment it jumped is not
     separately folded into this meet — f_owned/f_gc feed only the
     per-pc trap-unwind table (this file's own `put`, above), never an
     emission decision (every DROP/RC instruction break/continue itself
     needs is already emitted at emit_break/emit_continue's own site,
     from the owner table, unconditionally) — so the only thing this
     could under/over-track is which registers a *trap during the
     narrow window right after this loop* would additionally destroy,
     not whether break/continue's own owned value is dropped at all. *)
  if f.f_div then begin
    f.f_owned <- entry_owned;
    f.f_gc <- entry_gc
  end
  else mask_meet f entry_owned entry_gc;
  f.f_div <- div0

(* `for it in c` over a `multi`: the container, cursor index and length
   are loop-carried, so they are locals of the loop's own scope (below
   every statement temporary), and the container/index pair is adjacent
   because BUILTIN's argument window is consecutive. A `map` has no
   key-enumeration builtin in v1, so iterating one is WO-E403 rather
   than invented bytecode. *)
and emit_for (p : pctx) (f : fstate) (v : views) (s : Ast.stmt) (var : string)
    (var2 : string option) (iter : Ast.expr) (body : Ast.stmt list) : unit =
  match ty_of_expr p f iter with
  | Some t
    when (match (unwrap t, var2) with Map _, Some _ -> true | _ -> false) ->
    (* `for k, v in m` — slot-ordered enumeration over the map's parallel
       arrays: `len` bounds it, `key_at`/`val_at` read slot i. Both cursors
       are borrows of what the map owns, so nothing is dropped per
       iteration (owner.ml declares them l_holds = false). *)
    let kt, vt = match unwrap t with Map (k, v') -> (Scalar k, Scalar v') | _ -> (Scalar "Int", Scalar "Int") in
    let v2 = match var2 with Some n -> n | None -> "_" in
    let saved_locals = f.f_nlocals in
    let saved_env = f.f_env in
    let saved_decls = f.f_declared in
    let div0 = f.f_div in
    let rc = alloc_local p f s.s_pos in
    let ri = alloc_local p f s.s_pos in
    let rn = alloc_local p f s.s_pos in
    let rk = alloc_local p f s.s_pos in
    let rv = alloc_local p f s.s_pos in
    f.f_temp <- f.f_nlocals;
    emit_expr p f v ~dst:rc iter;
    f.f_cur_line <- s.s_pos.line;
    put f (ins_abc op_builtin rn rc b_len);
    put f (ins_abx op_loadk ri (check_bx p f s.s_pos "constant" (const_int p 0)));
    f.f_env <- (v2, (rv, vt)) :: (var, (rk, kt)) :: f.f_env;
    Hashtbl.replace f.f_decl s.s_id rk;
    f.f_declared <- s.s_id :: f.f_declared;
    let top = here f in
    let entry_owned = f.f_owned and entry_gc = f.f_gc in
    f.f_temp <- f.f_nlocals;
    let tc = alloc_temp p f s.s_pos in
    put f (ins_abc op_lt tc ri rn);
    let jz = here f in
    put f (ins_asbx op_jz tc 0);
    (* the builtin window is (container, index) in two consecutive slots *)
    let w = alloc_temps p f s.s_pos 2 in
    put f (ins_abc op_move w rc 0);
    put f (ins_abc op_move (w + 1) ri 0);
    put f (ins_abc op_builtin rk w b_map_key_at);
    put f (ins_abc op_builtin rv w b_map_val_at);
    let lf = { lf_node = s.s_id; lf_breaks = []; lf_continues = [] } in
    f.f_loops <- lf :: f.f_loops;
    List.iter (emit_stmt p f v) body;
    f.f_loops <- List.tl f.f_loops;
    emit_scope_drops p f v ~node:s.s_id ~label:"FOR";
    emit_rc p f v ~node:s.s_id ~acquire:false ~groups:(declared_since f saved_decls) ();
    let continue_target = here f in
    List.iter (fun pc -> patch_jump p f ~file:f.f_file ~pos:s.s_pos pc continue_target) lf.lf_continues;
    f.f_temp <- f.f_nlocals;
    f.f_cur_line <- s.s_pos.line;
    let one = alloc_temp p f s.s_pos in
    put f (ins_abx op_loadk one (check_bx p f s.s_pos "constant" (const_int p 1)));
    put f (ins_abc op_add ri ri one);
    let back = here f in
    put f (ins_asbx op_jmp 0 0);
    patch_jump p f ~file:f.f_file ~pos:s.s_pos back top;
    let exit_pc = here f in
    patch_jump p f ~file:f.f_file ~pos:s.s_pos jz exit_pc;
    List.iter (fun pc -> patch_jump p f ~file:f.f_file ~pos:s.s_pos pc exit_pc) lf.lf_breaks;
    if f.f_div then begin
      f.f_owned <- entry_owned;
      f.f_gc <- entry_gc
    end
    else mask_meet f entry_owned entry_gc;
    f.f_div <- div0;
    f.f_nlocals <- saved_locals;
    f.f_env <- saved_env;
    f.f_declared <- saved_decls;
    f.f_temp <- saved_locals
  | Some t when (match unwrap t with Multi _ -> true | _ -> false) ->
    let elem = match unwrap t with Multi e -> Scalar e | other -> other in
    let saved_locals = f.f_nlocals in
    let saved_env = f.f_env in
    let saved_decls = f.f_declared in
    let div0 = f.f_div in
    let rc = alloc_local p f s.s_pos in
    let ri = alloc_local p f s.s_pos in
    let rn = alloc_local p f s.s_pos in
    let rv = alloc_local p f s.s_pos in
    f.f_temp <- f.f_nlocals;
    emit_expr p f v ~dst:rc iter;
    f.f_cur_line <- s.s_pos.line;
    put f (ins_abc op_builtin rn rc b_count);
    put f (ins_abx op_loadk ri (check_bx p f s.s_pos "constant" (const_int p 0)));
    f.f_env <- (var, (rv, elem)) :: f.f_env;
    Hashtbl.replace f.f_decl s.s_id rv;
    f.f_declared <- s.s_id :: f.f_declared;
    let top = here f in
    let entry_owned = f.f_owned and entry_gc = f.f_gc in
    f.f_temp <- f.f_nlocals;
    let tc = alloc_temp p f s.s_pos in
    put f (ins_abc op_lt tc ri rn);
    let jz = here f in
    put f (ins_asbx op_jz tc 0);
    put f (ins_abc op_builtin rv rc b_multi_get);
    let lf = { lf_node = s.s_id; lf_breaks = []; lf_continues = [] } in
    f.f_loops <- lf :: f.f_loops;
    List.iter (emit_stmt p f v) body;
    f.f_loops <- List.tl f.f_loops;
    emit_scope_drops p f v ~node:s.s_id ~label:"FOR";
    emit_rc p f v ~node:s.s_id ~acquire:false ~groups:(declared_since f saved_decls) ();
    (* haxe-parity Task 2: `continue` re-enters right here — after this
       iteration's own scope-end cleanup (a `continue` already ran the
       equivalent of it at its own site, from v_continue — see
       emit_continue — so landing after the *normal* cleanup above
       never double-drops), and before the increment, so the next
       iteration still advances. *)
    let continue_target = here f in
    List.iter
      (fun pc -> patch_jump p f ~file:f.f_file ~pos:s.s_pos pc continue_target)
      lf.lf_continues;
    f.f_temp <- f.f_nlocals;
    f.f_cur_line <- s.s_pos.line;
    let one = alloc_temp p f s.s_pos in
    put f (ins_abx op_loadk one (check_bx p f s.s_pos "constant" (const_int p 1)));
    put f (ins_abc op_add ri ri one);
    let back = here f in
    put f (ins_asbx op_jmp 0 0);
    patch_jump p f ~file:f.f_file ~pos:s.s_pos back top;
    let exit_pc = here f in
    patch_jump p f ~file:f.f_file ~pos:s.s_pos jz exit_pc;
    List.iter (fun pc -> patch_jump p f ~file:f.f_file ~pos:s.s_pos pc exit_pc) lf.lf_breaks;
    if f.f_div then begin
      f.f_owned <- entry_owned;
      f.f_gc <- entry_gc
    end
    else mask_meet f entry_owned entry_gc;
    f.f_div <- div0;
    f.f_nlocals <- saved_locals;
    f.f_env <- saved_env;
    f.f_declared <- saved_decls;
    f.f_temp <- saved_locals
  | _ ->
    err p ~code:cannot_lower_code ~file:f.f_file ~pos:s.s_pos
      ~message:
        "`for` can only iterate a `multi` — the v1 builtins expose no key enumeration for a `map`"

(* haxe-parity Task 2: `do { body } while cond` — body first, condition
   after, otherwise the exact same JZ/JMP shape `while` uses (no new
   opcode). `continue` re-enters at the condition check (the one point
   every iteration passes through, whichever way it got there); `break`
   exits to the same place the condition's own JZ does. Unlike
   while/for, the body always runs at least once — there is no
   zero-iteration path to merge the exit mask with, so (unlike
   emit_while/emit_for) the non-diverged case simply keeps whatever
   mask the condition check left, no `mask_meet` needed. The
   `f.f_div`/entry-restore branch below is kept anyway, for consistency
   with owner.ml's `fixpoint` (shared, unmodified, across all three loop
   shapes, and always resets `diverged` to its pre-loop value) — a
   disclosed, documented approximation for the one shape neither pass
   chases precisely: a body that unconditionally returns/breaks on
   every path, making the condition dead code. No fixture in this task
   has that shape. *)
and emit_do_while (p : pctx) (f : fstate) (v : views) (s : Ast.stmt) (body : Ast.stmt list)
    (cond : Ast.expr) : unit =
  let top = here f in
  let entry_owned = f.f_owned and entry_gc = f.f_gc in
  let div0 = f.f_div in
  let lf = { lf_node = s.s_id; lf_breaks = []; lf_continues = [] } in
  f.f_loops <- lf :: f.f_loops;
  emit_block p f v ~node:s.s_id ~label:"DO" body;
  f.f_loops <- List.tl f.f_loops;
  let cond_pc = here f in
  List.iter (fun pc -> patch_jump p f ~file:f.f_file ~pos:s.s_pos pc cond_pc) lf.lf_continues;
  let t = alloc_temp p f s.s_pos in
  emit_expr p f v ~dst:t cond;
  f.f_cur_line <- s.s_pos.line;
  let jz = here f in
  put f (ins_asbx op_jz t 0);
  let back = here f in
  put f (ins_asbx op_jmp 0 0);
  patch_jump p f ~file:f.f_file ~pos:s.s_pos back top;
  let exit_pc = here f in
  patch_jump p f ~file:f.f_file ~pos:s.s_pos jz exit_pc;
  List.iter (fun pc -> patch_jump p f ~file:f.f_file ~pos:s.s_pos pc exit_pc) lf.lf_breaks;
  if f.f_div then begin
    f.f_owned <- entry_owned;
    f.f_gc <- entry_gc
  end
  else mask_meet f entry_owned entry_gc;
  f.f_div <- div0

(* ============================================================
   One method
   ============================================================ *)

let emit_method (p : pctx) (v : views) ~(file : string) ~(self_class : (int * string) option)
    (m : Ast.method_decl) (rec_ : methrec) : unit =
  let f =
    { f_file = file; f_fn = m.name; f_ret = m.ret; f_code = code_create (); f_cur_line = m.pos.line;
      f_line = -1; f_lines = []; f_owned = 0L; f_gc = 0L; f_last_owned = 0L; f_last_gc = 0L;
      f_drops = []; f_nlocals = 0; f_temp = 0; f_max = 0; f_env = []; f_decl = Hashtbl.create 16;
      f_node = Hashtbl.create 64; f_kind = Hashtbl.create 16; f_declared = []; f_div = false; f_maxjmp = 0;
      f_over = false; f_loops = [] }
  in
  (match self_class with
  | None -> ()
  | Some (_, cn) ->
    let r = alloc_local p f m.pos in
    f.f_env <- ("self", (r, Scalar cn)) :: f.f_env;
    Hashtbl.replace f.f_decl m.id r;
    f.f_declared <- m.id :: f.f_declared);
  List.iter
    (fun (pa : Ast.param) ->
      let r = alloc_local p f pa.pos in
      f.f_env <- (pa.name, (r, pa.ty)) :: f.f_env;
      Hashtbl.replace f.f_decl pa.id r;
      f.f_declared <- pa.id :: f.f_declared;
      match Hashtbl.find_opt v.v_holder pa.id with
      | Some kind -> mask_set f kind r
      | None -> (
        (* The one holder the tables cannot always name: a `take`
           parameter holds from the first instruction, but if it is moved
           on before the function's first call site no LIVE-MASK entry
           ever mentions it (the owner pass records masks only at call /
           DB_STUB sites) and a trap in between would leak it. The kind
           comes from types.ml's own field-kind mapping, which agrees
           with owner.ml's parameter rule by construction: OWNED/MULTI/
           MAP hold as owned values, GCREF as a counted handle, and
           SCALAR/TEXT are copies that never drop. *)
        match pa.conv with
        | Borrow | Mut -> ()
        | Take -> (
          match field_kind p pa.ty with
          | 1 | 4 | 5 -> mask_set f Owner.LOwned r
          | 2 -> mask_set f Owner.LGc r
          | _ -> ())))
    m.params;
  List.iter (emit_stmt p f v) m.body;
  stmt_reset f;
  f.f_cur_line <- m.pos.line;
  emit_scope_drops p f v ~node:m.id ~label:"BODY";
  emit_rc p f v ~node:m.id ~acquire:false ~groups:(declared_since f []) ();
  (* the terminator rule: the loader rejects a method whose last
     instruction is not one, and the implicit void return is what
     control falling off the end means *)
  let need_ret =
    f.f_code.n = 0
    || f.f_maxjmp >= f.f_code.n
    ||
    let last = f.f_code.a.(f.f_code.n - 1) land 0xFF in
    not (last = op_ret || last = op_ret0)
  in
  if need_ret then put f (ins_abc op_ret0 0 0 0);
  rec_.mr_regc <- max 1 (f.f_max + 1);
  if rec_.mr_regc > max_regs then begin
    over_budget p f m.pos;
    rec_.mr_regc <- max_regs
  end;
  rec_.mr_code <- Array.sub f.f_code.a 0 f.f_code.n;
  rec_.mr_lines <- List.rev f.f_lines;
  rec_.mr_drops <- List.rev f.f_drops

(* ============================================================
   Program assembly
   ============================================================ *)

(* haxe-parity Task 5: does this program catch anywhere? Only then does the
   `Error` record earn a class-table entry — so no image that never writes
   `try` gains a class it does not use. *)
let program_uses_try (prog : Ast.program) : bool =
  let found = ref false in
  Types.walk_program
    (fun _ (e : Ast.expr) -> match e.Ast.kind with Ast.Try _ -> found := true | _ -> ())
    prog;
  !found

(* Which predeclared records does this program actually need a class-table
   entry for? `Error` when it catches; a stdlib result record when it calls
   the member that fills one. Nothing else gains a class it never uses, so
   every image that predates this surface keeps its exact class table. *)
let needed_records (prog : Ast.program) : string list =
  let want = ref [] in
  let add n = if not (List.mem n !want) then want := n :: !want in
  Types.walk_program
    (fun _ (e : Ast.expr) ->
      match e.Ast.kind with
      | Ast.Try _ -> add Types.error_record_name
      | Ast.Call ({ Ast.kind = Ast.Field ({ Ast.kind = Ast.Ident head; _ }, mname); _ }, _) -> (
        match Types.stdlib_member head mname with
        | Some { Types.sm_record = Some r; _ } -> add r
        | _ -> ())
      | _ -> ())
    prog;
  !want

(* Structural satisfaction, Go-style (spec section 2): a class satisfies
   an interface when it has a method of the same name and parameter count
   for every method the interface declares. There is no `implements`
   keyword by doctrine, so this is the whole rule — and it is also why
   the satisfying set is computed here: types.ml declares WO-E205 but,
   per the error catalog, never raises it (the check has no legal call
   site in the milestone grammar). *)
let satisfies (p : pctx) (cid : int) (ir : ifacerec) : int list option =
  let cr = p.p_classes.(cid) in
  let rec go acc = function
    | [] -> Some (List.rev acc)
    | (mname, nparams) :: tl -> (
      if not (List.mem mname cr.cr_methods) then None
      else
        match class_method p cr.cr_name mname with
        (* A `static fn` has no receiver to dispatch on, so it can never
           satisfy an interface method however well its name and arity
           line up (haxe-parity Task 7). *)
        | Some mi when mi.Types.is_static -> None
        | Some mi when List.length mi.Types.params = nparams -> (
          match SM.find_opt (cr.cr_name ^ "." ^ mname) p.p_method_id with
          | Some midx -> go (midx :: acc) tl
          | None -> None)
        | _ -> None)
  in
  go [] ir.ir_methods

let emit ~(syms : Types.symbols) ~(module_of : string -> string)
    ~(module_syms : (string, Types.symbols) Hashtbl.t) (coll : Diag.Collector.t) (units : input list) :
    string =
  let colliding = compute_colliding_fn_names ~module_of units in
  (* ---- pass 1: declarations, in discovery then declaration order ---- *)
  let classes = ref [] and class_id = ref SM.empty and nclasses = ref 0 in
  let ifaces = ref [] and iface_id = ref SM.empty and nifaces = ref 0 and nslots = ref 0 in
  let methods = ref [] and method_id = ref SM.empty and nmethods = ref 0 in
  let entry = ref wob_none in
  (* (file, self class option, method_decl, unit) in method-table order *)
  let bodies = ref [] in
  (* haxe-parity Task 4: typedef records are STRUCTURAL — two records
     with the same shape share ONE class-table entry, keyed by this
     rendering of the ordered field list. Defaults are part of the key
     deliberately: aliases whose defaults differ get their own entries
     (identical layout either way, so interchangeability is unaffected —
     class ids only decide layout and drop plan), because sharing one
     entry would make emit_ctor's default-filling read whichever alias
     registered first. types.ml's typ_equal compares fields only —
     strictly wider than this key, and safe for exactly that layout
     reason. *)
  let record_shape : (string, int) Hashtbl.t = Hashtbl.create 8 in
  let record_shape_key (c : Ast.class_decl) : string =
    String.concat ";"
      (List.map
         (fun (fl : Ast.field) ->
           let dflt =
             match fl.default with
             | None -> ""
             | Some Ast.DefaultNow -> "=now()"
             | Some (Ast.DefaultOpaque toks) ->
               "=" ^ String.concat " " (List.map (fun (t : Token.t) -> Dump.kind_label t.Token.kind) toks)
           in
           fl.name ^ ":" ^ Dump.field_ty_str fl.ty ^ dflt)
         c.fields)
  in
  List.iter
    (fun u ->
      List.iter
        (function
          | Ast.Class (c : Ast.class_decl) ->
            if not (SM.mem c.name !class_id) then begin
              let shape = if c.is_record then Some (record_shape_key c) else None in
              let alias_of =
                match shape with Some key -> Hashtbl.find_opt record_shape key | None -> None
              in
              match alias_of with
              | Some cid ->
                (* structural alias: this name maps onto the shape's
                   existing entry; no new clsrec *)
                class_id := SM.add c.name cid !class_id
              | None ->
                let cid = !nclasses in
                class_id := SM.add c.name cid !class_id;
                incr nclasses;
                (match shape with
                | Some key -> Hashtbl.replace record_shape key cid
                | None -> ());
                classes :=
                  { cr_name = c.name; cr_gc = c.is_gc;
                    cr_fields =
                      Array.of_list (List.map (fun (fl : Ast.field) -> (fl.name, fl.ty)) c.fields);
                    cr_methods = List.map (fun (m : Ast.method_decl) -> m.name) c.methods }
                  :: !classes
            end
          | Ast.Union (ud : Ast.union_decl) ->
            (* haxe-parity Task 4: a payload union gets one compiler-
               generated class entry PER VARIANT (bare variants of the
               same union included — a uniform heap representation is
               what lets one register hold "any variant of this union");
               the entry's id is the variant's runtime tag, carried by
               the object header's own class_id. An all-bare union gets
               nothing here at all: its values are plain ordinals. *)
            if List.exists (fun (vd : Ast.variant_decl) -> vd.Ast.v_fields <> []) ud.variants then
              List.iter
                (fun (vd : Ast.variant_decl) ->
                  let key = ud.name ^ "." ^ vd.Ast.v_name in
                  if not (SM.mem key !class_id) then begin
                    let cid = !nclasses in
                    class_id := SM.add key cid !class_id;
                    incr nclasses;
                    classes :=
                      { cr_name = key; cr_gc = false;
                        cr_fields = Array.of_list vd.Ast.v_fields;
                        cr_methods = [] }
                      :: !classes
                  end)
                ud.variants
          | Ast.Interface (i : Ast.interface_decl) ->
            (* an interface with no methods gets no slots and no rows: the
               loader rejects a zero-method interface entry, and no ICALL
               could ever name one *)
            if i.methods <> [] && not (SM.mem i.name !iface_id) then begin
              let iid = !nifaces in
              iface_id := SM.add i.name iid !iface_id;
              incr nifaces;
              ifaces :=
                { ir_name = i.name; ir_slot_base = !nslots;
                  ir_methods =
                    List.map (fun (s : Ast.method_sig) -> (s.name, List.length s.params)) i.methods }
                :: !ifaces;
              nslots := !nslots + List.length i.methods
            end
          | Ast.Fn _ -> ()
          | Ast.Use _ -> ()
          | Ast.Const _ -> ())
        u.prog.decls)
    units;
  (* haxe-parity Task 5: the record `catch (e)` binds needs a real
     class-table entry (it is an ordinary heap object with two owned Texts,
     so the drop plan is the ordinary one). Added only for a program that
     actually catches — every existing image keeps its exact class table —
     and only when nothing already claims the name. Its field order is
     Types.error_record_fields, which is the same order the VM's
     WO_B_ERR_FILL builtin writes. *)
  List.iter
    (fun (name, fields) ->
      if
        (not (SM.mem name !class_id))
        && List.exists (fun u -> List.mem name (needed_records u.prog)) units
      then begin
        let cid = !nclasses in
        class_id := SM.add name cid !class_id;
        incr nclasses;
        classes :=
          { cr_name = name; cr_gc = false; cr_fields = Array.of_list fields; cr_methods = [] }
          :: !classes
      end)
    Types.predeclared_records;
  let class_id = !class_id in
  let p_classes = Array.of_list (List.rev !classes) in
  let p_ifaces = Array.of_list (List.rev !ifaces) in
  List.iter
    (fun u ->
      List.iter
        (function
          | Ast.Class (c : Ast.class_decl) ->
            let cid = SM.find c.name class_id in
            List.iter
              (fun (m : Ast.method_decl) ->
                let key = c.name ^ "." ^ m.name in
                if not (SM.mem key !method_id) then begin
                  method_id := SM.add key !nmethods !method_id;
                  (* haxe-parity Task 7: a `static fn` has no receiver, so
                     its window holds parameters only and its body binds
                     no `self` (self_class = None below) — otherwise it is
                     an ordinary method record, keyed `Class.name` like
                     any other. *)
                  methods :=
                    { mr_name = m.name; mr_class = Some cid;
                      mr_argc = (if m.is_static then 0 else 1) + List.length m.params;
                      mr_regc = 1; mr_code = [||]; mr_lines = []; mr_drops = [] }
                    :: !methods;
                  bodies :=
                    (u, (if m.is_static then None else Some (cid, c.name)), m, !nmethods) :: !bodies;
                  incr nmethods
                end)
              c.methods
          | Ast.Interface _ -> ()
          | Ast.Fn (m : Ast.method_decl) ->
            let key = free_fn_key colliding (module_of u.file) m.name in
            if not (SM.mem key !method_id) then begin
              method_id := SM.add key !nmethods !method_id;
              methods :=
                { mr_name = m.name; mr_class = None; mr_argc = List.length m.params; mr_regc = 1;
                  mr_code = [||]; mr_lines = []; mr_drops = [] }
                :: !methods;
              bodies := (u, None, m, !nmethods) :: !bodies;
              (* Program mode: the entry is the free fn `main`, taking either
                 nothing or one `multi Text` of command-line arguments (the
                 runtime builds it — runtime/src/main.c). One fixed name, so
                 `wovm image.wob` needs no flag, and its return value is the
                 process exit code. *)
              let entry_shaped =
                m.name = "main"
                && match m.params with
                   | [] -> true
                   | [ (pa : Ast.param) ] -> ( match pa.Ast.ty with Ast.Multi "Text" -> true | _ -> false)
                   | _ -> false
              in
              if entry_shaped then begin
                entry := !nmethods;
                (match m.ret with
                 | Some ty when ty <> Ast.Scalar "Int" ->
                   Diag.Collector.add coll
                     (Diag.error ~code:entry_return_code ~file:u.file ~line:m.pos.line
                        ~col:m.pos.col
                        ~message:
                          (Printf.sprintf
                             "entry `main` declares return type `%s` — the entry's return \
                              value is the process exit code, so it must return `Int`"
                             (Dump.field_ty_str ty))
                        ())
                 | Some _ | None -> ())
              end;
              incr nmethods
            end
          | Ast.Use _ -> ()
          | Ast.Const _ -> ()
          | Ast.Union _ -> () (* haxe-parity Task 4: no methods to emit *))
        u.prog.decls)
    units;
  let p_methods = Array.of_list (List.rev !methods) in
  let p_uses : (string, Types.use_edge list) Hashtbl.t = Hashtbl.create 8 in
  List.iter (fun u -> Hashtbl.replace p_uses u.file (Types.uses_of_program u.prog)) units;
  let p =
    { p_syms = syms; p_coll = coll; p_classes; p_class_id = class_id; p_ifaces;
      p_iface_id = !iface_id; p_method_id = !method_id; p_methods; p_uses;
      p_module_syms = module_syms; p_module_of = module_of; p_colliding = colliding;
      p_kints = Hashtbl.create 32;
      p_ktexts = Hashtbl.create 32; p_consts = []; p_nconsts = 0 }
  in
  (* names are constants; interning them first keeps the pool's low
     indexes stable and readable in a disassembly *)
  let class_name_k = Array.map (fun c -> const_text p c.cr_name) p_classes in
  (* Field-name constants are interned HERE, with every other constant, and
     never during serialization: the constant pool is written before the
     class table, so a name interned later would be missing from the image
     (found the hard way — the loader rejected every class). *)
  let class_field_names =
    Array.map (fun c -> Array.map (fun ((fname : string), _) -> const_text p fname) c.cr_fields) p_classes
  in
  let iface_name_k = Array.map (fun i -> const_text p i.ir_name) p_ifaces in
  let method_name_k = Array.map (fun m -> const_text p m.mr_name) p_methods in
  (* ---- pass 2: method bodies ---- *)
  let views_of = Hashtbl.create 8 in
  List.iter (fun u -> Hashtbl.replace views_of u.file (build_views u.tables)) units;
  List.iter
    (fun (u, self_class, m, idx) ->
      let v = Hashtbl.find views_of u.file in
      emit_method p v ~file:u.file ~self_class m p_methods.(idx))
    (List.rev !bodies);
  (* The residual table is the ONLY licence to emit a borrow op, and it
     is also an obligation: every region in it must end up wrapped. The
     regions are anchored on several different node kinds (a call
     expression, an assignment statement, a moved place), so a lowering
     that forgets one would ship the aliasing check silently disabled —
     the exact hazard the site exists for. Anything unconsumed is
     WO-E404 here, reported at the region's own position. *)
  List.iter
    (fun u ->
      let v = Hashtbl.find views_of u.file in
      let leftover =
        Hashtbl.fold
          (fun node (pos, _) acc ->
            if Hashtbl.mem v.v_res_used node then acc else (pos, node) :: acc)
          v.v_res []
      in
      List.iter
        (fun ((pos : Ast.pos), _) ->
          err p ~code:unguardable_code ~file:u.file ~pos
            ~message:
              "residual borrow site was never wrapped in runtime guards — the emitter has no \
               lowering for this region, and leaving it unguarded would disable the aliasing \
               check it exists for")
        (List.sort (fun ((a : Ast.pos), _) ((b : Ast.pos), _) ->
             compare (a.line, a.col) (b.line, b.col))
           leftover))
    units;
  (* ---- pass 3: vtable rows ---- *)
  let rows = ref [] in
  Array.iteri
    (fun cid _ ->
      Array.iteri
        (fun iid ir ->
          match satisfies p cid ir with
          | Some ms -> rows := (cid, iid, ms) :: !rows
          | None -> ())
        p_ifaces)
    p_classes;
  let rows = List.rev !rows in
  (* ---- pass 4: serialize ---- *)
  let consts = Buf.create () in
  List.iter
    (fun c ->
      match c with
      | `Int n ->
        Buf.u8 consts k_int;
        Buf.i64 consts (Int64.of_int n)
      | `Text s ->
        Buf.u8 consts k_text;
        Buf.u32 consts (String.length s);
        Buf.str consts s)
    (List.rev p.p_consts);
  let cls = Buf.create () in
  Array.iteri
    (fun cid (c : clsrec) ->
      Buf.u32 cls class_name_k.(cid);
      Buf.u32 cls (if c.cr_gc then classf_gc else 0);
      Buf.u32 cls (Array.length c.cr_fields);
      Array.iter (fun (_, ty) -> Buf.u8 cls (field_kind p ty)) c.cr_fields;
      let pad = (4 - (Array.length c.cr_fields mod 4)) mod 4 in
      for _ = 1 to pad do
        Buf.u8 cls 0
      done;
      (* v2 per-field metadata (wob.h's "class-table field metadata"): the
         names json.encode renders as keys, the classes json.decode has to
         build for a nested field, and the element kinds a container field
         needs when decode creates one. *)
      Array.iter (fun kidx -> Buf.u32 cls kidx) class_field_names.(cid);
      Array.iter (fun (_, ty) -> Buf.u32 cls (field_class_meta p ty)) c.cr_fields;
      Array.iter (fun (_, ty) -> Buf.u32 cls (field_elem_meta p ty)) c.cr_fields)
    p_classes;
  let ifs = Buf.create () in
  Array.iteri
    (fun iid (i : ifacerec) ->
      Buf.u32 ifs iface_name_k.(iid);
      Buf.u32 ifs (List.length i.ir_methods))
    p_ifaces;
  Buf.u32 ifs (List.length rows);
  List.iter
    (fun (cid, iid, ms) ->
      Buf.u32 ifs cid;
      Buf.u32 ifs iid;
      List.iter (fun m -> Buf.u32 ifs m) ms)
    rows;
  let mth = Buf.create () in
  Array.iteri
    (fun idx (m : methrec) ->
      Buf.u32 mth method_name_k.(idx);
      Buf.u32 mth (match m.mr_class with Some c -> c | None -> wob_none);
      Buf.u8 mth m.mr_argc;
      Buf.u8 mth m.mr_regc;
      Buf.u16 mth 0;
      Buf.u32 mth (Array.length m.mr_code * 4);
      Array.iter (fun i -> Buf.u32 mth i) m.mr_code;
      Buf.u32 mth (List.length m.mr_lines);
      List.iter
        (fun (pc, line) ->
          Buf.u32 mth pc;
          Buf.u32 mth line)
        m.mr_lines;
      Buf.u32 mth (List.length m.mr_drops);
      List.iter
        (fun (pc, owned, gc) ->
          Buf.u32 mth pc;
          Buf.i64 mth owned;
          Buf.i64 mth gc)
        m.mr_drops)
    p_methods;
  let out = Buf.create () in
  let off = ref wob_hdr_size in
  Buf.u32 out wob_magic;
  Buf.u32 out wob_version;
  Buf.u32 out !off;
  Buf.u32 out p.p_nconsts;
  off := !off + consts.Buf.len;
  Buf.u32 out !off;
  Buf.u32 out (Array.length p_classes);
  off := !off + cls.Buf.len;
  Buf.u32 out !off;
  Buf.u32 out (Array.length p_ifaces);
  off := !off + ifs.Buf.len;
  Buf.u32 out !off;
  Buf.u32 out (Array.length p_methods);
  Buf.u32 out !entry;
  Buf.str out (Buf.contents consts);
  Buf.str out (Buf.contents cls);
  Buf.str out (Buf.contents ifs);
  Buf.str out (Buf.contents mth);
  Buf.contents out
