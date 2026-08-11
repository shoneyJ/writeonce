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

(* ============================================================
   Format constants (mirror of runtime/src/wob.h — never diverge)
   ============================================================ *)

let wob_magic = 0x31424F57 (* "WOB1" read as an LE u32 *)
let wob_version = 1
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

type fstate = {
  f_file : string;
  f_fn : string;
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
}

(* ---- per-unit views of the four owner tables ---- *)

type views = {
  v_move : (int, string) Hashtbl.t; (* place-expr node -> moved place text *)
  v_scope : (int * string, Owner.drop_item list) Hashtbl.t;
  v_join : (int * string, Owner.drop_item list) Hashtbl.t;
  v_return : (int, Owner.drop_item list) Hashtbl.t;
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
      v_return = Hashtbl.create 16; v_overwrite = Hashtbl.create 16; v_mask = Hashtbl.create 16;
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

let class_method (p : pctx) (cname : string) (m : string) : Types.method_info option =
  match Types.StringMap.find_opt cname p.p_syms.Types.classes with
  | None -> None
  | Some (c : Types.class_info) ->
    List.find_opt (fun (mi : Types.method_info) -> mi.Types.name = m) c.Types.methods

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

let builtin_ret (name : string) (argty : Ast.field_ty option) : Ast.field_ty option =
  match name with
  | "now" -> Some (Scalar "Timestamp")
  | "print" | "print_int" | "push" | "set" -> Some (Scalar "Int")
  | "words" | "count" -> Some (Scalar "Int")
  | "has" -> Some (Scalar "Bool")
  | "latest" -> ( match argty with Some t -> ( match unwrap t with Multi e -> Some (Scalar e) | _ -> None) | None -> None)
  | "get" -> (
    match argty with
    | Some t -> ( match unwrap t with Multi e -> Some (Scalar e) | Map (_, v) -> Some (Scalar v) | _ -> None)
    | None -> None)
  | _ -> None

let is_builtin_name (n : string) =
  List.mem n
    [ "now"; "print"; "print_int"; "words"; "multi_new"; "push"; "get"; "count"; "latest";
      "map_new"; "set"; "has" ]

let rec ty_of_expr (p : pctx) (f : fstate) (e : Ast.expr) : Ast.field_ty option =
  match e.kind with
  | IntLit _ -> Some (Scalar "Int")
  | StrLit _ -> Some (Scalar "Text")
  | BoolLit _ -> Some (Scalar "Bool")
  | Ident n -> ( match List.assoc_opt n f.f_env with Some (_, t) -> Some t | None -> None)
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
      match free_fn p n with
      | Some fi -> fi.Types.ret
      | None ->
        if is_builtin_name n then
          builtin_ret n (match args with a :: _ -> ty_of_expr p f a | [] -> None)
        else None)
    | Field (base, mname) -> (
      match ty_of_expr p f base with
      | Some bt -> (
        match unwrap bt with
        | Scalar cn -> (
          match class_method p cn mname with
          | Some mi -> mi.Types.ret
          | None -> ( match iface_method p cn mname with Some (_, sg) -> sg.Types.ret | None -> None))
        | _ -> None)
      | None -> None)
    | _ -> None)
  | Unary (Neg, o) -> ty_of_expr p f o
  | Binary (op, l, _) -> (
    match op with
    | Concat -> Some (Scalar "Text")
    | Eq | Ne | Lt | Le | Gt | Ge -> Some (Scalar "Bool")
    | Add | Sub | Mul | Div | Mod -> ( match ty_of_expr p f l with Some t -> Some t | None -> Some (Scalar "Int")))
  | Ctor (cn, _) -> Some (Scalar cn)
  | DbStub _ -> None

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
let container_imm (p : pctx) (expected : Ast.field_ty option) (map : bool) : int option =
  match expected with
  | Some t -> (
    match unwrap t with
    | Multi e when not map -> Some (field_kind p (Scalar e))
    | Map (k, v) when map -> Some (field_kind p (Scalar k) lor (field_kind p (Scalar v) lsl 4))
    | _ -> None)
  | None -> None

let rec emit_expr (p : pctx) (f : fstate) (v : views) ~(dst : int) ?expected (e : Ast.expr) : unit =
  f.f_cur_line <- e.pos.line;
  (match e.kind with
  | IntLit n -> put f (ins_abx op_loadk dst (check_bx p f e.pos "constant" (const_int p n)))
  | BoolLit b -> put f (ins_abx op_loadk dst (check_bx p f e.pos "constant" (const_int p (if b then 1 else 0))))
  | StrLit s -> put f (ins_abx op_loadk dst (check_bx p f e.pos "constant" (const_text p s)))
  | Ident n -> (
    match lookup_local f n with
    | Some (r, _) -> if r <> dst then put f (ins_abc op_move dst r 0)
    | None ->
      err p ~code:cannot_lower_code ~file:f.f_file ~pos:e.pos
        ~message:(Printf.sprintf "`%s` is not a local, parameter, or `self` — nothing to load" n);
      put f (ins_abx op_loadk dst (const_int p 0)))
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
      | Some bt -> ( match unwrap bt with Multi _ -> Some b_multi_get | Map _ -> Some b_map_get | _ -> None)
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
  | Call (callee, args) -> emit_call p f v ~dst ?expected e callee args
  | DbStub _ ->
    sync_mask p f v e.id;
    put f (ins_abc op_db_stub 0 0 0));
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
  | Eq -> if is_text p f l || is_text p f r then simple op_eqs else simple op_eq
  | Ne ->
    (* no NE opcode in the v1 set: `a != b` is `(a == b) == 0`. The
       format doc governs, so this is a lowering, not a new opcode. *)
    let a = emit_operand p f v l in
    let b = emit_operand p f v r in
    let t = alloc_temp p f pos in
    put f (ins_abc (if is_text p f l || is_text p f r then op_eqs else op_eq) t a b);
    let z = alloc_temp p f pos in
    put f (ins_abx op_loadk z (check_bx p f pos "constant" (const_int p 0)));
    put f (ins_abc op_eq dst t z)
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

and emit_ctor (p : pctx) (f : fstate) (v : views) ~(dst : int) (e : Ast.expr) (cn : string)
    (fields : (string * Ast.expr) list) : unit =
  match class_of_name p cn with
  | None ->
    err p ~code:cannot_lower_code ~file:f.f_file ~pos:e.pos
      ~message:(Printf.sprintf "constructor of `%s`, which is not a declared class" cn);
    put f (ins_abx op_loadk dst (const_int p 0))
  | Some cid ->
    put f (ins_abx op_new dst (check_bx p f e.pos "class" cid));
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
      fields

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
    match free_fn p name with
    | Some fi -> emit_direct p f v ~dst e ~key:name ~recv:None ~params:fi.Types.params args
    | None ->
      if is_builtin_name name then emit_builtin p f v ~dst ?expected e name args
      else begin
        err p ~code:cannot_lower_code ~file:f.f_file ~pos:e.pos
          ~message:(Printf.sprintf "call to `%s`, which is not a declared fn or a builtin" name);
        put f (ins_abx op_loadk dst (const_int p 0))
      end)
  | Field (base, mname) -> (
    match ty_of_expr p f base with
    | Some bt -> (
      match unwrap bt with
      | Scalar cn -> (
        match class_method p cn mname with
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
    | None ->
      err p ~code:cannot_lower_code ~file:f.f_file ~pos:e.pos
        ~message:(Printf.sprintf "cannot resolve the receiver's type for the call to `%s`" mname);
      put f (ins_abx op_loadk dst (const_int p 0)))
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

and call_window (p : pctx) (f : fstate) (v : views) (e : Ast.expr) ~(recv : Ast.expr option)
    ~(params : (string * Ast.field_ty * Ast.param_conv) list) (args : Ast.expr list) : int * int =
  let nrecv = match recv with Some _ -> 1 | None -> 0 in
  let argc = nrecv + List.length args in
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
  (base, argc)

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
    let base, _ = call_window p f v e ~recv ~params args in
    let guards = residual_guards p f v e.id (Some gbase) in
    acquire_guards f guards;
    (* the frame's drop map, from the owner table, effective at the CALL *)
    sync_mask p f v e.id;
    f.f_cur_line <- e.pos.line;
    put f (ins_abx op_call base (check_bx p f e.pos "method" midx));
    release_guards f guards;
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
  let w, _ = call_window p f v e ~recv:(Some base) ~params args in
  let guards = residual_guards p f v e.id (Some gbase) in
  acquire_guards f guards;
  sync_mask p f v e.id;
  f.f_cur_line <- e.pos.line;
  put f (ins_abx op_icall w (check_bx p f e.pos "interface slot" slot));
  release_guards f guards;
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
    else if id = b_print || id = b_print_int || id = b_words || id = b_count || id = b_latest then 1
    else if id = b_multi_push || id = b_multi_get || id = b_map_get || id = b_map_has then 2
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
    let declared = match ty with Some t -> Some (Scalar t) | None -> None in
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
  | For { var; iter; body } -> emit_for p f v s var iter body

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
        err p ~code:cannot_lower_code ~file:f.f_file ~pos:target.pos
          ~message:
            "element assignment into a `multi` — the v1 instruction set has push/get but no \
             element write"
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
  emit_block p f v ~node:s.s_id ~label:"WHILE" body;
  f.f_cur_line <- s.s_pos.line;
  let back = here f in
  put f (ins_asbx op_jmp 0 0);
  patch_jump p f ~file:f.f_file ~pos:s.s_pos back top;
  patch_jump p f ~file:f.f_file ~pos:s.s_pos jz (here f);
  (* a loop may run zero times, so the exit state always includes the
     entry state; a body that returned contributes nothing *)
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
and emit_for (p : pctx) (f : fstate) (v : views) (s : Ast.stmt) (var : string) (iter : Ast.expr)
    (body : Ast.stmt list) : unit =
  match ty_of_expr p f iter with
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
    List.iter (emit_stmt p f v) body;
    emit_scope_drops p f v ~node:s.s_id ~label:"FOR";
    emit_rc p f v ~node:s.s_id ~acquire:false ~groups:(declared_since f saved_decls) ();
    f.f_temp <- f.f_nlocals;
    f.f_cur_line <- s.s_pos.line;
    let one = alloc_temp p f s.s_pos in
    put f (ins_abx op_loadk one (check_bx p f s.s_pos "constant" (const_int p 1)));
    put f (ins_abc op_add ri ri one);
    let back = here f in
    put f (ins_asbx op_jmp 0 0);
    patch_jump p f ~file:f.f_file ~pos:s.s_pos back top;
    patch_jump p f ~file:f.f_file ~pos:s.s_pos jz (here f);
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

(* ============================================================
   One method
   ============================================================ *)

let emit_method (p : pctx) (v : views) ~(file : string) ~(self_class : (int * string) option)
    (m : Ast.method_decl) (rec_ : methrec) : unit =
  let f =
    { f_file = file; f_fn = m.name; f_code = code_create (); f_cur_line = m.pos.line;
      f_line = -1; f_lines = []; f_owned = 0L; f_gc = 0L; f_last_owned = 0L; f_last_gc = 0L;
      f_drops = []; f_nlocals = 0; f_temp = 0; f_max = 0; f_env = []; f_decl = Hashtbl.create 16;
      f_node = Hashtbl.create 64; f_kind = Hashtbl.create 16; f_declared = []; f_div = false; f_maxjmp = 0;
      f_over = false }
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
        | Some mi when List.length mi.Types.params = nparams -> (
          match SM.find_opt (cr.cr_name ^ "." ^ mname) p.p_method_id with
          | Some midx -> go (midx :: acc) tl
          | None -> None)
        | _ -> None)
  in
  go [] ir.ir_methods

let emit ~(syms : Types.symbols) (coll : Diag.Collector.t) (units : input list) : string =
  (* ---- pass 1: declarations, in discovery then declaration order ---- *)
  let classes = ref [] and class_id = ref SM.empty and nclasses = ref 0 in
  let ifaces = ref [] and iface_id = ref SM.empty and nifaces = ref 0 and nslots = ref 0 in
  let methods = ref [] and method_id = ref SM.empty and nmethods = ref 0 in
  let entry = ref wob_none in
  (* (file, self class option, method_decl, unit) in method-table order *)
  let bodies = ref [] in
  List.iter
    (fun u ->
      List.iter
        (function
          | Ast.Class (c : Ast.class_decl) ->
            if not (SM.mem c.name !class_id) then begin
              let cid = !nclasses in
              class_id := SM.add c.name cid !class_id;
              incr nclasses;
              classes :=
                { cr_name = c.name; cr_gc = c.is_gc;
                  cr_fields =
                    Array.of_list (List.map (fun (fl : Ast.field) -> (fl.name, fl.ty)) c.fields);
                  cr_methods = List.map (fun (m : Ast.method_decl) -> m.name) c.methods }
                :: !classes
            end
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
          | Ast.Fn _ -> ())
        u.prog.decls)
    units;
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
                  methods :=
                    { mr_name = m.name; mr_class = Some cid; mr_argc = 1 + List.length m.params;
                      mr_regc = 1; mr_code = [||]; mr_lines = []; mr_drops = [] }
                    :: !methods;
                  bodies := (u, Some (cid, c.name), m, !nmethods) :: !bodies;
                  incr nmethods
                end)
              c.methods
          | Ast.Interface _ -> ()
          | Ast.Fn (m : Ast.method_decl) ->
            if not (SM.mem m.name !method_id) then begin
              method_id := SM.add m.name !nmethods !method_id;
              methods :=
                { mr_name = m.name; mr_class = None; mr_argc = List.length m.params; mr_regc = 1;
                  mr_code = [||]; mr_lines = []; mr_drops = [] }
                :: !methods;
              bodies := (u, None, m, !nmethods) :: !bodies;
              (* the entry point is the zero-argument free fn `main` —
                 the loader's own rule for an entry (a zero-arg free fn)
                 plus one fixed name so `wovm image.wob` needs no flag *)
              if m.name = "main" && m.params = [] then begin
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
            end)
        u.prog.decls)
    units;
  let p_methods = Array.of_list (List.rev !methods) in
  let p =
    { p_syms = syms; p_coll = coll; p_classes; p_class_id = class_id; p_ifaces;
      p_iface_id = !iface_id; p_method_id = !method_id; p_methods; p_kints = Hashtbl.create 32;
      p_ktexts = Hashtbl.create 32; p_consts = []; p_nconsts = 0 }
  in
  (* names are constants; interning them first keeps the pool's low
     indexes stable and readable in a disassembly *)
  let class_name_k = Array.map (fun c -> const_text p c.cr_name) p_classes in
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
      done)
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
