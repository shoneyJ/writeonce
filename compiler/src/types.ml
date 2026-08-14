(* types.ml — Typechecker for `.wo` OOP source (Task 6).
   Two-pass:
   1. Collect all declarations (classes, interfaces, free fns, typedefs)
   2. Typecheck bodies with full symbol tables.
   Produces typed AST + per-class field-kind table. *)

open Ast

module StringMap = Map.Make(String)
module StringSet = Set.Make(String)

(* ============================================================
   Type representations (internal, resolved)
   ============================================================ *)

type typ =
  | TScalar of string                    (* Int, Bool, Text, user class name *)
  | TNullable of typ                     (* ?T *)
  | TMulti of typ                        (* multi T *)
  | TMap of typ * typ                    (* map<K, V> *)
  | TRef of string                       (* ref T — ID link *)
  | TVoid                                (* no return *)

(* .wob field kinds (docs/plan/oop-vm/00-wob-format.md) *)
type wob_kind =
  | WO_K_SCALAR   (* 0 *)
  | WO_K_OWNED    (* 1 *)
  | WO_K_GCREF    (* 2 *)
  | WO_K_TEXT     (* 3 *)
  | WO_K_MULTI    (* 4 *)
  | WO_K_MAP      (* 5 *)
  | WO_K_NULLABLE (* 6 *)

(* ============================================================
   Symbol tables (Pass 1 output, Pass 2 input)
   ============================================================ *)

type class_info = {
  name : string;
  is_class : bool;
  is_record : bool; (* haxe-parity Task 4 — see Ast.class_decl.is_record *)
  is_gc : bool;
  table : table_cfg option;
  fields : (string * field_ty * default_expr option * string list) list;
  methods : method_info list;
  id : int;
  pos : pos;
  pub : bool; (* haxe-parity Task 1 (modules) — see Ast.class_decl.pub *)
}

and interface_info = {
  name : string;
  methods : method_sig_info list;
  id : int;
  pos : pos;
  pub : bool; (* haxe-parity Task 1 (modules) *)
}

and method_sig_info = {
  name : string;
  params : (string * field_ty * param_conv) list;
  ret : field_ty option;
  pos : pos;
  id : int;
}

and method_info = {
  name : string;
  params : (string * field_ty * param_conv) list;
  ret : field_ty option;
  body : stmt list;
  mutates : bool;
  is_static : bool;
  id : int;
  pos : pos;
}

and free_fn_info = {
  name : string;
  params : (string * field_ty * param_conv) list;
  ret : field_ty option;
  body : stmt list;
  mutates : bool;
  id : int;
  pos : pos;
  pub : bool; (* haxe-parity Task 1 (modules) *)
}

and typedef_info = {
  name : string;
  fields : (string * field_ty * default_expr option) list;
  id : int;
  pos : pos;
}

(* haxe-parity Task 4: one variant of a union. `vi_tag` is the variant's
   ordinal in its union's declaration order — for an all-bare union that
   IS the runtime value (a plain scalar tag); for a payload-carrying
   union the runtime tag is instead the variant's own class-table id
   (emit.ml assigns it; the object header's class_id field carries it —
   docs/plan/oop-vm/00-wob-format.md's variant convention), and vi_tag
   only orders exhaustiveness messages deterministically. *)
and variant_info = {
  vi_name : string;
  vi_fields : (string * field_ty) list;
  vi_tag : int;
}

and union_info = {
  u_name : string;
  u_variants : variant_info list;
  (* at least one variant carries payload fields: every value of this
     union is a heap variant object. False = all-bare: pure scalar tags,
     no class-table entries, no heap. *)
  u_has_payload : bool;
  u_id : int;
  u_pos : pos;
  u_pub : bool;
}

and symbols = {
  classes : class_info StringMap.t;
  interfaces : interface_info StringMap.t;
  free_fns : free_fn_info StringMap.t;
  typedefs : typedef_info StringMap.t;
  unions : union_info StringMap.t; (* haxe-parity Task 4 *)
  modules : string list;
}

(* Variant lookup by bare name, across every union in scope — how a
   `Pending`/`Failed(...)` reference resolves at all (variants share one
   flat namespace per program, like free fns; a duplicate within one
   file is WO-E215, below). Unions are few and small, so a fold beats
   maintaining a second, derived map that could drift. *)
let find_variant_in (unions : union_info StringMap.t) (name : string) :
    (union_info * variant_info) option =
  StringMap.fold
    (fun _ u acc ->
      match acc with
      | Some _ -> acc
      | None -> (
        match List.find_opt (fun v -> v.vi_name = name) u.u_variants with
        | Some v -> Some (u, v)
        | None -> None))
    unions None

let find_variant (syms : symbols) (name : string) : (union_info * variant_info) option =
  find_variant_in syms.unions name

(* haxe-parity Task 7: the static member behind `Flock.held(path)` — a
   qualified name whose head is a class, not a value. Instance methods are
   deliberately excluded: `Cls.method(...)` on a non-static method is not
   a call with an implicit receiver, it is an error, and returning None
   here is what lets the caller say so. *)
let static_method_of (syms : symbols) (cls_name : string) (m_name : string) : method_info option =
  match StringMap.find_opt cls_name syms.classes with
  | Some cls -> List.find_opt (fun (m : method_info) -> m.name = m_name && m.is_static) cls.methods
  | None -> None

(* Builtin scalars *)
let builtin_scalars = ["Int"; "Bool"; "Text"; "Timestamp"; "Id"]

let is_builtin_scalar name = List.mem name builtin_scalars

(* haxe-parity Task 1 (modules) / gap-closure amendment: six reserved
   stdlib namespaces, not the plan text's five — `use fs`/`proc`/`net`/
   `time`/`json`/`env` must resolve now (their members arrive in plan
   9); a project module can never legally shadow one of these six
   single-segment names (`check_use_edges` below treats any one-segment
   `use` path whose name is in this list as stdlib, unconditionally,
   never as a project directory search). *)
let stdlib_modules = [ "fs"; "proc"; "net"; "time"; "json"; "env" ]

let is_stdlib_module (name : string) : bool = List.mem name stdlib_modules

(* haxe-parity Task 5: the record `catch (e)` binds — the VM's structured
   trap error, one shape forever (spec §6). Predeclared rather than
   written: no source declares it, every program that catches gets it, and
   the field ORDER here is the contract with the VM's WO_B_ERR_FILL
   builtin (runtime/src/wob.h), which writes fields 0..3 by index. *)
let error_record_name = "Error"

let error_record_fields : (string * field_ty) list =
  [ ("code", Scalar "Int"); ("line", Scalar "Int"); ("method", Scalar "Text");
    ("msg", Scalar "Text") ]

(* Adds the predeclared records to a symbol table. Applied to the MERGED
   table only (bin/main.ml), never to a per-file one: one entry per file
   would read as a cross-file duplicate declaration (WO-E214). A source
   that declares its own `Error` keeps it — its own fields are then the
   ones `catch (e)` binds, which is either what it wanted or a type error
   it will hear about at the use site. *)
let with_builtin_records (syms : symbols) : symbols =
  if StringMap.mem error_record_name syms.classes then syms
  else
    let info =
      { name = error_record_name; is_class = false; is_record = true; is_gc = false; table = None;
        fields = List.map (fun (n, t) -> (n, t, None, [])) error_record_fields; methods = [];
        id = -1; pos = { line = 0; col = 0 }; pub = true }
    in
    { syms with classes = StringMap.add error_record_name info syms.classes }

let rec has_recursive_structure (cls : class_info) : bool =
  List.exists (fun (_, ty, _, _) ->
    match ty with
    | Ast.Scalar name -> name = cls.name  (* direct self-reference *)
    | Ast.Ref name -> name = cls.name
    | Ast.Multi name -> name = cls.name  (* multi Self *)
    | Ast.Map (k, v) -> k = cls.name || v = cls.name  (* map<_, Self> / map<Self, _> *)
    | Ast.Nullable inner -> has_recursive_structure_type inner cls.name
  ) cls.fields

and has_recursive_structure_type (ty : Ast.field_ty) (cls_name : string) : bool =
  match ty with
  | Ast.Scalar name -> name = cls_name
  | Ast.Ref name -> name = cls_name
  | Ast.Multi name -> name = cls_name
  | Ast.Map (k, v) -> k = cls_name || v = cls_name
  | Ast.Nullable inner -> has_recursive_structure_type inner cls_name

(* @unique field -> persistent identity (plan's "When NOT to emit": a
   class with a @unique field should not get the @gc suggestion even if
   it also has recursive/shared structure). Annotation *names* only, per
   Ast.field's own doc comment -- "unique" is what parse_field stores for
   a bare `@unique`. *)
let has_unique_field (cls : class_info) : bool =
  List.exists (fun (_, _, _, anns) -> List.mem "unique" anns) cls.fields

let is_gc_class (syms : symbols) name =
  try
    let cls = StringMap.find name syms.classes in
    cls.is_gc
  with Not_found -> false

let gc_suggestion_code = Diag.warning_prefix ^ "201"    (* WO-W201 *)

let suggest_gc_annotation ~file (cls : class_info) (collector : Diag.Collector.t) : unit =
  if not cls.is_gc && Option.is_none cls.table && not (has_unique_field cls)
     && has_recursive_structure cls then
    Diag.Collector.add collector
      (Diag.warning ~code:gc_suggestion_code ~file ~line:cls.pos.line ~col:cls.pos.col
         ~message:(Printf.sprintf "%s has recursive/shared structure that borrow checker cannot prove. Consider adding @gc if this is an ephemeral in-memory cache. If this maps to a database table, keep owned (default)." cls.name) ())

(* Ast.field_ty -> the internal resolved typ. Hoisted out of
   typecheck_program (where it was a local closure) so the .wob emitter
   can reach the same mapping instead of keeping a second copy of it;
   the check pass still calls it under its old local name. *)
let rec typ_of_field_ty (ft : field_ty) : typ =
  match ft with
  | Scalar name -> TScalar name
  | Ref name -> TRef name
  | Multi inner_name -> TMulti (TScalar inner_name)
  | Map (k_name, v_name) -> TMap (TScalar k_name, TScalar v_name)
  | Nullable inner -> TNullable (typ_of_field_ty inner)

(* wob_kind_of_typ: maps internal typ to .wob field kind *)
let wob_kind_of_typ (syms : symbols) (t : typ) : wob_kind =
  let kind_of = function
    | TScalar name ->
        (* Text is NOT a plain scalar slot: a Text field holds a heap
           string, and the runtime's per-kind drop plan
           (runtime/src/gc.c wo_drop_kind) only frees it under
           WO_K_TEXT. Emitting WO_K_SCALAR here leaked every string a
           class owned. Found by the emitter, this function's first
           caller. *)
        if name = "Text" then WO_K_TEXT
        else if is_builtin_scalar name then WO_K_SCALAR
        else if StringMap.mem name syms.unions then
          (* haxe-parity Task 4: an all-bare union value is a plain
             integer tag — SCALAR, or the drop plan would chase the tag
             as a pointer (the exact int-as-pointer segfault family this
             codebase keeps re-finding). A payload union value is a heap
             variant object the slot owns — OWNED, its own class-table
             kinds free the payload recursively. *)
          (if (StringMap.find name syms.unions).u_has_payload then WO_K_OWNED
           else WO_K_SCALAR)
        else if is_gc_class syms name then WO_K_GCREF
        else WO_K_OWNED
    | TNullable _inner -> WO_K_NULLABLE
    | TMulti _ -> WO_K_MULTI
    | TMap _ -> WO_K_MAP
    | TRef _ -> WO_K_SCALAR
    | TVoid -> WO_K_SCALAR
  in
  kind_of t

(* ============================================================
   Diagnostics (WO-E2xx)
   ============================================================ *)

let type_mismatch_code = Diag.types_prefix ^ "01"
let unknown_field_code = Diag.types_prefix ^ "02"
let bad_arity_code = Diag.types_prefix ^ "03"
let unknown_fn_code = Diag.types_prefix ^ "04"
let unsatisfied_interface_code = Diag.types_prefix ^ "05"
let incomplete_ctor_code = Diag.types_prefix ^ "06"
let unknown_type_code = Diag.types_prefix ^ "07"
let non_exhaustive_switch_code = Diag.types_prefix ^ "08"
let invalid_builtin_code = Diag.types_prefix ^ "09"
let module_not_imported_code = Diag.types_prefix ^ "10"
let nullable_used_without_check_code = Diag.types_prefix ^ "11"
let nullable_assign_mismatch_code = Diag.types_prefix ^ "12"
let missing_nil_check_code = Diag.types_prefix ^ "13"

(* haxe-parity Task 1 (modules). module_not_imported_code (WO-E210,
   above) was already reserved by Task 6's brief for exactly this: "a
   name used from a module that was never `use`d" — genuinely blocked
   until now on there being a `use`/module concept at all
   (docs/plan/compiler/nullable-types-implementation.md's dead-code
   register). The three below are new — no earlier task named or
   reserved them, because no earlier task had a module system to need
   them for. *)
let unknown_module_code = Diag.types_prefix ^ "16"     (* WO-E216 *)
let private_name_code = Diag.types_prefix ^ "17"       (* WO-E217 *)
let use_collision_code = Diag.types_prefix ^ "18"      (* WO-E218 *)
let unused_use_code = Diag.warning_prefix ^ "202"      (* WO-W202 *)

let unknown_type_name_code = Diag.types_prefix ^ "25"  (* WO-E225 *)

(* haxe-parity Task 3, review fix (Critical 1). `default` is moved to
   the *end* of the lowering order regardless of where it sits in the
   source (Ast.switch_lowering_order) -- so a `case` arm written after
   it is not a silent, unreachable dead-code trap the way it was
   before that fix, but it is still surprising source: warn once per
   switch shaped that way, naming where `default` actually sits. *)
let switch_default_not_last_code = Diag.warning_prefix ^ "203"  (* WO-W203 *)

(* Same-file counterpart to main.ml's cross-file WO-E214 (Task 8 review):
   a duplicate class/interface/fn name declared twice *within one file*
   was silently dropped by collect_declarations's StringMap.add (Task 1
   review, "Known limitations" #6 -- no diagnostic at all). *)
let duplicate_decl_code = Diag.types_prefix ^ "15"  (* WO-E215 *)

(* ============================================================
   Pass 1: Declaration Collection
   ============================================================ *)

(* Reported at the *later* declaration, with the first as the related
   site -- exactly WO-E214's shape. The map keeps the first declaration
   (a duplicate is never added), matching WO-E214's own first-wins rule
   for the merged cross-file table. *)
let report_duplicate_decl (collector : Diag.Collector.t) ~file ~(kind : string) ~(name : string)
    ~(pos : pos) ~(first_pos : pos) : unit =
  Diag.Collector.add collector
    (Diag.error ~code:duplicate_decl_code ~file ~line:pos.line ~col:pos.col
       ~message:(Printf.sprintf "%s `%s` already declared" kind name)
       ~related:
         [ Diag.related_site ~file ~line:first_pos.line ~col:first_pos.col
             ~label:(Printf.sprintf "`%s` first declared here" name)
         ]
       ())

let collect_declarations ~file (prog : program) (collector : Diag.Collector.t) : symbols =
  let classes = ref StringMap.empty in
  let interfaces = ref StringMap.empty in
  let free_fns = ref StringMap.empty in
  let typedefs = ref StringMap.empty in
  let unions = ref StringMap.empty in
  let modules = ref [] in

  List.iter (function
    | Ast.Class c ->
        let fields = List.map (fun (f : Ast.field) ->
          (f.name, f.ty, f.default, f.annotations)
        ) c.fields in
        let methods = List.map (fun (m : Ast.method_decl) ->
          { name = m.name;
            params = List.map (fun (p : Ast.param) -> (p.name, p.ty, p.conv)) m.params;
            ret = m.ret;
            body = m.body;
            mutates = false;
            is_static = m.is_static;
            id = m.id;
            pos = m.pos; }
        ) (c.methods : Ast.method_decl list) in
        let info = {
          name = c.name;
          is_class = c.is_class;
          is_record = c.is_record;
          is_gc = c.is_gc;
          table = c.table;
          fields = fields;
          methods = methods;
          id = c.id;
          pos = c.pos;
          pub = c.pub;
        } in
        (match StringMap.find_opt c.name !classes with
         | Some (existing : class_info) ->
             report_duplicate_decl collector ~file ~kind:"class" ~name:c.name ~pos:c.pos
               ~first_pos:existing.pos
         | None ->
             classes := StringMap.add c.name info !classes;
             suggest_gc_annotation ~file info collector)
    | Ast.Interface i ->
        let methods = List.map (fun (m : Ast.method_sig) ->
          { name = m.name;
            params = List.map (fun (p : Ast.param) -> (p.name, p.ty, p.conv)) m.params;
            ret = m.ret;
            pos = m.pos;
            id = m.id; }
        ) (i.methods : Ast.method_sig list) in
        let info = {
          name = i.name;
          methods = methods;
          id = i.id;
          pos = i.pos;
          pub = i.pub;
        } in
        (match StringMap.find_opt i.name !interfaces with
         | Some (existing : interface_info) ->
             report_duplicate_decl collector ~file ~kind:"interface" ~name:i.name ~pos:i.pos
               ~first_pos:existing.pos
         | None -> interfaces := StringMap.add i.name info !interfaces)
    | Ast.Fn f ->
        let info = {
          name = f.name;
          params = List.map (fun (p : Ast.param) -> (p.name, p.ty, p.conv)) f.params;
          ret = f.ret;
          body = f.body;
          mutates = false;
          id = f.id;
          pos = f.pos;
          pub = f.pub;
        } in
        (match StringMap.find_opt f.name !free_fns with
         | Some (existing : free_fn_info) ->
             report_duplicate_decl collector ~file ~kind:"fn" ~name:f.name ~pos:f.pos
               ~first_pos:existing.pos
         | None -> free_fns := StringMap.add f.name info !free_fns)
    | Ast.Use _ -> ()
      (* module-graph concern (haxe-parity Task 1), not a declaration —
         handled by check_modules below, over the raw AST directly (it
         needs the *file's* use-edges, not a merged per-name table). *)
    | Ast.Const _ -> ()
      (* haxe-parity Task 2: consts are fully resolved by parser.ml's own
         post-parse substitution pass before typecheck ever runs — every
         reference already *is* the literal it named, so there is
         nothing left for this stage to declare or check. *)
    | Ast.Union u ->
        (* haxe-parity Task 4. Duplicate union names get WO-E215 exactly
           like classes/interfaces/fns do; a variant name reused across
           this file's unions gets it too (variants share one flat value
           namespace — a bare `Ok` reference could not otherwise pick a
           tag). Cross-file variant collisions ride the same first-wins
           merge every other kind already has (WO-E214 covers classes/
           interfaces only — disclosed, not extended here). *)
        let variants =
          List.mapi
            (fun i (v : Ast.variant_decl) ->
              { vi_name = v.v_name; vi_fields = v.v_fields; vi_tag = i })
            u.variants
        in
        let info = {
          u_name = u.name;
          u_variants = variants;
          u_has_payload = List.exists (fun v -> v.vi_fields <> []) variants;
          u_id = u.id;
          u_pos = u.pos;
          u_pub = u.pub;
        } in
        (match StringMap.find_opt u.name !unions with
         | Some (existing : union_info) ->
             report_duplicate_decl collector ~file ~kind:"union" ~name:u.name ~pos:u.pos
               ~first_pos:existing.u_pos
         | None ->
             let seen_own = ref [] in
             List.iter
               (fun (v : Ast.variant_decl) ->
                 (match List.assoc_opt v.v_name !seen_own with
                  | Some first_pos ->
                      report_duplicate_decl collector ~file ~kind:"variant" ~name:v.v_name
                        ~pos:v.v_pos ~first_pos
                  | None -> (
                    match find_variant_in !unions v.v_name with
                    | Some (other, _) ->
                        report_duplicate_decl collector ~file ~kind:"variant" ~name:v.v_name
                          ~pos:v.v_pos ~first_pos:other.u_pos
                    | None -> ()));
                 seen_own := (v.v_name, v.v_pos) :: !seen_own)
               u.variants;
             unions := StringMap.add u.name info !unions)
  ) prog.decls;

  { classes = !classes; interfaces = !interfaces; free_fns = !free_fns;
    typedefs = !typedefs; unions = !unions; modules = !modules }

(* ============================================================
   Pass 2: Body Typechecking
   ============================================================ *)

type expr_type_result = {
  typ : typ;
  is_nil : bool;
}

(* WO-E225's "known type" set: builtin, a declared class (typedef
   records included — they live in `classes`), a declared interface, or
   (haxe-parity Task 4) a declared union. A dotted name whose head is a
   reserved stdlib namespace (`json.Value` — the sample's own record
   field) is UNKNOWN-BUT-RESERVED: accepted here the same way a
   `fs.stat(...)` call is accepted by the module checker, since the six
   namespaces' members arrive in plan 9. Any other dotted name is as
   unknown as a misspelling. *)
let is_known_type_name (syms : symbols) (name : string) : bool =
  is_builtin_scalar name
  || StringMap.mem name syms.classes
  || StringMap.mem name syms.interfaces
  || StringMap.mem name syms.unions
  || (match String.index_opt name '.' with
     | Some i -> is_stdlib_module (String.sub name 0 i)
     | None -> false)

let rec scalar_name_of (ft : field_ty) : string option =
  match ft with
  | Scalar name -> Some name
  | Nullable inner -> scalar_name_of inner
  | Ref _ | Multi _ | Map _ -> None

(* Checked once per field declaration (not at every access/use site), so
   the diagnostic lands at the field's own declaration position and
   never fires more than once for the same bad field. Runs over the raw
   AST rather than `syms.classes` because class_info's fields tuple
   doesn't carry a pos (see Ast.field for that); run from Pass 2 (after
   collect_declarations has fully built `syms`) so a field typed with a
   class declared later in the same file is not a false positive. *)
let check_field_types ~file (syms : symbols) (collector : Diag.Collector.t)
    (prog : program) : unit =
  List.iter (function
    | Ast.Class c ->
        List.iter (fun (f : Ast.field) ->
          match scalar_name_of f.ty with
          | Some name when not (is_known_type_name syms name) ->
              Diag.Collector.add collector
                (Diag.error ~code:unknown_type_name_code ~file
                   ~line:f.pos.line ~col:f.pos.col
                   ~message:(Printf.sprintf "unknown type `%s`" name) ())
          | _ -> ()
        ) c.fields
    | Ast.Union u ->
        (* haxe-parity Task 4: a variant's payload field types get the
           same once-per-declaration WO-E225 check a class field's type
           does, at the variant's own position. *)
        List.iter (fun (v : Ast.variant_decl) ->
          List.iter (fun (_, fty) ->
            match scalar_name_of fty with
            | Some name when not (is_known_type_name syms name) ->
                Diag.Collector.add collector
                  (Diag.error ~code:unknown_type_name_code ~file
                     ~line:v.v_pos.line ~col:v.v_pos.col
                     ~message:(Printf.sprintf "unknown type `%s`" name) ())
            | _ -> ()
          ) v.v_fields
        ) u.variants
    | Ast.Interface _ | Ast.Fn _ | Ast.Use _ | Ast.Const _ -> ()
  ) prog.decls

(* haxe-parity Task 4: structural typedef equality — "two typedefs with
   the same shape are the SAME type" (the task brief's own words). Shape
   = the ordered (name, type) field list; defaults are construction-time
   sugar carried per alias, not part of the shape. Used wherever two
   resolved types are compared for agreement (today: a switch's arm
   unification); nominal names still win everywhere else (`A` = `A`
   short-circuits first). *)
let record_shapes_equal (syms : symbols) (a : string) (b : string) : bool =
  match (StringMap.find_opt a syms.classes, StringMap.find_opt b syms.classes) with
  | Some ca, Some cb ->
      ca.is_record && cb.is_record
      && List.length ca.fields = List.length cb.fields
      && List.for_all2
           (fun (na, ta, _, _) (nb, tb, _, _) -> na = nb && ta = tb)
           ca.fields cb.fields
  | _ -> false

let rec typ_equal (syms : symbols) (a : typ) (b : typ) : bool =
  a = b
  || (match (a, b) with
     | TScalar x, TScalar y -> record_shapes_equal syms x y
     | TNullable x, TNullable y -> typ_equal syms x y
     | TMulti x, TMulti y -> typ_equal syms x y
     | TMap (ka, va), TMap (kb, vb) -> typ_equal syms ka kb && typ_equal syms va vb
     | _ -> false)

(* ============================================================
   WO-E209 — builtin call signature checking (hotfix)
   ============================================================

   `print(7)` compiled clean and segfaulted wovm: `print` wants a
   `Text` (a heap-string pointer, .wob kind WO_K_TEXT), the VM's
   `str_check` dereferences whatever register it's handed as a
   `wo_str*` with no runtime tag to check first (registers are
   untyped by design -- the compiler is supposed to be the only gate),
   and a bare `7` is a WO_K_SCALAR int64 sitting in that register --
   a wild pointer read. Source of truth for the table below is
   docs/plan/oop-vm/08-builtin-surface.md; the arities mirror
   emit.ml's `is_builtin_name`/`arity_of` (its own, emission-time
   arity check, WO-E403, kept as-is -- this is a second, earlier gate
   over the same contract, not a replacement). *)

type builtin_arg_req =
  | ReqText
  | ReqInt
  | ReqMulti
  | ReqMap
  | ReqContainer (* multi or map, e.g. `count`/`get` resolve on either *)
  | ReqAny (* not checked here -- e.g. push/set/get's key/value args:
              their type depends on the container's own element/key/value
              kind, which this check does not chase *)

let builtin_signatures : (string * int * builtin_arg_req list) list =
  [ ("now", 0, []);
    ("print", 1, [ ReqText ]);
    ("print_int", 1, [ ReqInt ]);
    ("words", 1, [ ReqText ]);
    ("multi_new", 0, []);
    ("map_new", 0, []);
    ("push", 2, [ ReqMulti; ReqAny ]);
    ("get", 2, [ ReqContainer; ReqAny ]);
    ("count", 1, [ ReqContainer ]);
    ("latest", 1, [ ReqMulti ]);
    ("set", 3, [ ReqMap; ReqAny; ReqAny ]);
    ("has", 2, [ ReqMap; ReqAny ]);
    ("int_to_text", 1, [ ReqInt ]);
    (* systems stdlib (docs/plan/oop-vm/08-builtin-surface.md): the text and
       container vocabulary the driving workload writes. `len` resolves on a
       text OR either container, so its argument is unchecked here the same
       way `get`'s key is. *)
    ("len", 1, [ ReqAny ]);
    ("byte_at", 2, [ ReqText; ReqInt ]);
    ("print_err", 1, [ ReqText ]);
    ("starts_with", 2, [ ReqText; ReqText ]);
    ("ends_with", 2, [ ReqText; ReqText ]);
    ("index_of", 2, [ ReqText; ReqText ]);
    ("last_index_of", 2, [ ReqText; ReqText ]);
    ("substr", 3, [ ReqText; ReqInt; ReqInt ]);
    ("trim", 1, [ ReqText ]);
    ("to_lower", 1, [ ReqText ]);
    ("char_of", 1, [ ReqInt ]);
    ("parse_int", 1, [ ReqText ]);
    ("split", 2, [ ReqText; ReqText ]);
    ("split_ws", 1, [ ReqText ]);
    ("join", 2, [ ReqMulti; ReqText ]);
    ("slice", 3, [ ReqMulti; ReqInt; ReqInt ]);
    ("pop", 1, [ ReqMulti ]);
    ("shift", 1, [ ReqMulti ]);
    ("sort", 1, [ ReqMulti ]);
    ("reverse", 1, [ ReqMulti ]);
    ("remove", 2, [ ReqMap; ReqAny ]);
    ("key_at", 2, [ ReqMap; ReqInt ]);
    ("val_at", 2, [ ReqMap; ReqInt ]);
  ]

let rec unwrap_nullable (t : typ) : typ =
  match t with
  | TNullable inner -> unwrap_nullable inner
  | other -> other

(* `ReqInt` accepts any non-`Text` builtin scalar (`Int`, `Bool`,
   `Timestamp`, `Id`), not literally the string "Int" -- `wob_kind_of_typ`
   (above) maps all four to the identical runtime representation,
   `WO_K_SCALAR`, a plain int64 register. The hazard this whole check
   exists for is a *representation* mismatch (`WO_K_TEXT`'s heap-string
   pointer read where a `WO_K_SCALAR` int64 sits, or vice versa) -- and
   `has`/`now` returning `Bool`/`Timestamp` into a `print_int`
   (`docs/plan/oop-vm/08-builtin-surface.md`'s own "1/0" convention for
   `has`) is exactly as runtime-safe as an `Int` there, found as a real
   false positive against `tests/corpus/run/pricing-containers/`'s
   `print_int(has(c.by_name, "shirt"))` once `confident_typ` started
   chasing builtin return types. `Text` stays its own, narrower case:
   it is the one builtin scalar with a genuinely different
   representation. *)
let is_scalar_shaped (name : string) : bool = is_builtin_scalar name && name <> "Text"

let matches_req (req : builtin_arg_req) (t : typ) : bool =
  match req, unwrap_nullable t with
  | ReqAny, _ -> true
  | ReqText, TScalar "Text" -> true
  | ReqInt, TScalar name -> is_scalar_shaped name
  | ReqMulti, TMulti _ -> true
  | ReqMap, TMap _ -> true
  | ReqContainer, (TMulti _ | TMap _) -> true
  | (ReqText | ReqInt | ReqMulti | ReqMap | ReqContainer), _ -> false

let req_label = function
  | ReqText -> "Text"
  | ReqInt -> "Int"
  | ReqMulti -> "a `multi`"
  | ReqMap -> "a `map`"
  | ReqContainer -> "a `multi` or `map`"
  | ReqAny -> "any" (* matches_req is always true here -- never rendered *)

let rec typ_label (t : typ) : string =
  match t with
  | TScalar name -> name
  | TNullable inner -> "?" ^ typ_label inner
  | TMulti _ -> "multi"
  | TMap _ -> "map"
  | TRef name -> "ref " ^ name
  | TVoid -> "void"

(* A builtin call's own confident return type, for when it appears as
   *another* builtin's argument (`print(words(x))` -- `words` returns
   `Int`, so that's `WO-E209` too, not a pass-through). Mirrors emit.ml's
   `builtin_ret` (its own confident return-type table, used when
   lowering an expression position) -- duplicated rather than shared
   because emit.ml operates over `Ast.field_ty`/its own `pctx`/`fstate`,
   not `Types.typ`/`symbols`, and this task leaves emit.ml untouched.
   Kept deliberately in sync: a future builtin whose return type changes
   needs both tables updated together. `multi_new`/`map_new` are absent
   on purpose -- their result's element kind comes from the destination
   (08-builtin-surface.md), not from anything chaseable here. *)
let builtin_confident_ret (name : string) (arg0 : typ option) : typ option =
  let arg0 = Option.map unwrap_nullable arg0 in
  match name with
  | "int_to_text" -> Some (TScalar "Text")
  | "now" -> Some (TScalar "Timestamp")
  | "print" | "print_int" | "push" | "set" -> Some (TScalar "Int")
  | "words" | "count" -> Some (TScalar "Int")
  | "has" -> Some (TScalar "Bool")
  | "latest" -> ( match arg0 with Some (TMulti e) -> Some e | _ -> None)
  | "get" -> ( match arg0 with Some (TMulti e) -> Some e | Some (TMap (_, v)) -> Some v | _ -> None)
  (* systems stdlib. Every fresh-Text and fresh-`multi` result is an owned
     value, so these entries are what make a `let` holding one get its
     drop (owner.ml classifies through this table too). *)
  | "len" | "byte_at" | "index_of" | "last_index_of" -> Some (TScalar "Int")
  | "print_err" | "sort" | "reverse" -> Some (TScalar "Int")
  | "starts_with" | "ends_with" | "remove" -> Some (TScalar "Bool")
  | "substr" | "trim" | "to_lower" | "char_of" | "join" -> Some (TScalar "Text")
  (* `parse_int` is optional-shaped: an unparseable text is 0, which is how
     a `?Int` spells nil (08-builtin-surface.md's `?T` section). *)
  | "parse_int" -> Some (TNullable (TScalar "Int"))
  | "split" | "split_ws" -> Some (TMulti (TScalar "Text"))
  | "slice" -> ( match arg0 with Some (TMulti e) -> Some (TMulti e) | _ -> None)
  | "pop" | "shift" -> ( match arg0 with Some (TMulti e) -> Some e | _ -> None)
  | "key_at" -> ( match arg0 with Some (TMap (k, _)) -> Some k | _ -> None)
  | "val_at" -> ( match arg0 with Some (TMap (_, v)) -> Some v | _ -> None)
  | _ -> None

(* `use_edge`/`uses_of_program`/`path_str` -- relocated here (hotfix)
   from their original home in the "Modules" section, much further
   below, purely so `confident_typ`'s free-fn resolution (inside
   `typecheck_program`, next) can see them; OCaml has no forward
   reference across top-level `let`s. See the "Modules" section itself
   for the full design writeup these belong to. *)
type use_edge = {
  ue_pos : pos;
  ue_alias : string;
  ue_segments : string list;
  ue_is_stdlib : bool;
}

let uses_of_program (prog : program) : use_edge list =
  List.filter_map
    (function
      | Use u ->
        let alias = match List.rev u.segments with a :: _ -> a | [] -> "" in
        let is_stdlib =
          match u.segments with [ s ] -> is_stdlib_module s | _ -> false
        in
        Some { ue_pos = u.pos; ue_alias = alias; ue_segments = u.segments; ue_is_stdlib = is_stdlib }
      | Class _ | Interface _ | Fn _ | Const _ | Union _ -> None)
    prog.decls

let path_str (segments : string list) : string = String.concat "/" segments

(* `check_builtin_call` itself is unopinionated about *how* an
   argument's type was derived -- it just compares whatever `typ
   option` it's handed against the signature table. The derivation
   (`confident_typ`, deliberately NOT `typecheck_expr`'s own `.typ`) is
   defined inside `typecheck_program` below, next to `syms`/`env` --
   see its own doc comment there for why a separate, narrower deriver
   is load-bearing for this check's "stay silent when underivable"
   contract. *)
let check_builtin_call ~file (collector : Diag.Collector.t) (name : string) (call_pos : pos)
    (args : expr list) (confident_types : typ option list) : unit =
  match List.find_opt (fun (n, _, _) -> n = name) builtin_signatures with
  | None -> () (* not one of ours -- WO-E204's unresolved-call territory, not this check's *)
  | Some (_, arity, reqs) ->
    let given = List.length args in
    if given <> arity then
      Diag.Collector.add collector
        (Diag.error ~code:invalid_builtin_code ~file ~line:call_pos.line ~col:call_pos.col
           ~message:(Printf.sprintf "builtin `%s` takes %d argument(s), given %d" name arity given) ())
    else
      List.iter2
        (fun ((req : builtin_arg_req), (arg_e : expr)) (ct : typ option) ->
          match ct with
          | None -> () (* underivable -- stay silent, no false positives *)
          | Some t ->
            if not (matches_req req t) then
              Diag.Collector.add collector
                (Diag.error ~code:invalid_builtin_code ~file ~line:arg_e.pos.line ~col:arg_e.pos.col
                   ~message:
                     (Printf.sprintf "builtin `%s` expects %s, got `%s`" name (req_label req)
                        (typ_label t))
                   ()))
        (List.combine reqs args) confident_types

(* `~file_syms` (hotfix, multi-file double-report): this file's OWN,
   unmerged collect_declarations output — as opposed to `syms` below,
   the whole-program merged table. Before this parameter existed, the
   two StringMap.iter loops at the bottom of this function walked
   `syms.classes`/`syms.free_fns` — the flat, cross-file table —
   regardless of which file was being checked, so a program of N files
   ran every file's bodies through typecheck_method/the free-fn loop
   once per discovered file (N times total, not once), each redundant
   pass re-reporting that body's diagnostics stamped with whichever
   `~file` happened to be current. Every OTHER lookup in this function
   (confident_typ, typecheck_expr's Field/Ctor cases, resolve_free_fn,
   ...) still goes through `syms`, unchanged — cross-file field/type/
   call resolution keeps seeing the whole program; only the SET OF
   BODIES actually walked and diagnosed narrows to this file's own. See
   .superpowers/sdd/2026-08-01-haxe-parity-language/hotfix-e209-report.md's
   "Disclosed, NOT fixed" section for the original repro and diagnosis. *)
let typecheck_program ~file ~(module_of : string -> string)
    ~(module_syms : (string, symbols) Hashtbl.t) ~(file_syms : symbols) (prog : program)
    (syms : symbols) (collector : Diag.Collector.t) : unit =
  check_field_types ~file syms collector prog;
  let resolve_field_ty = typ_of_field_ty in

  (* Free-fn resolution for `confident_typ`'s `Call`-return derivation
     below, and for whether a bare `Ident` callee is a builtin at all
     (a call to it, further down). Own module's own `free_fns`
     unconditionally, then exactly one used module's `pub` export;
     anything else (not found at all, or ambiguous across more than one
     used module) is `None`, not a guess -- that ambiguity is
     `WO-E217`/`WO-E218`'s own territory (`check_module_refs`, below),
     not this derivation's to adjudicate. Deliberately NOT
     `syms.free_fns` (the flat, whole-program merge fed to owner.ml and
     most of emit.ml): two modules sharing a free-fn name would let the
     flat table resolve to whichever module happened to merge first --
     the exact Critical-1 bug shape haxe-parity Task 1 already found
     and fixed for the emitter (emit.ml's `ty_of_expr`/`emit_call`, same
     fix, same reason); this mirrors it here instead of repeating it. *)
  let own_module_syms = Hashtbl.find_opt module_syms (module_of file) in
  let uses_resolved =
    List.filter_map
      (fun (u : use_edge) ->
        if u.ue_is_stdlib then None
        else
          match Hashtbl.find_opt module_syms (path_str u.ue_segments) with
          | Some s -> Some (u, s)
          | None -> None)
      (uses_of_program prog)
  in
  let resolve_free_fn (name : string) : free_fn_info option =
    match own_module_syms with
    | Some s when StringMap.mem name s.free_fns -> StringMap.find_opt name s.free_fns
    | _ -> (
        match
          List.filter_map
            (fun (_, (msyms : symbols)) ->
              match StringMap.find_opt name msyms.free_fns with
              | Some fi when fi.pub -> Some fi
              | _ -> None)
            uses_resolved
        with
        | [ fi ] -> Some fi
        | _ -> None)
  in

  (* WO-E209's own type deriver -- deliberately NOT typecheck_expr's
     `.typ` below, and deliberately narrower. typecheck_expr hands back
     exactly two placeholder values whenever it can't actually resolve
     an expression -- `TScalar "Int"` (an unresolved `Ident`, a
     `Field`/`Index` it can't type, a `Ctor` naming an unknown class)
     and `TScalar "Bool"` (every `Binary`, unconditionally, regardless
     of the real operator -- `a + b` types as "Bool" today exactly like
     `a == b` does). Both are also genuine, real types an expression
     can legitimately have, so trusting them blindly would misfire on
     ordinary code this corpus actually has: `print(items[0])` (`Index`
     always placeholder-types as `Int`) or `print_int(a + b)` (`Binary`
     always types as `Bool`) would both become false positives.
     `confident_typ` instead only trusts a handful of shapes it can
     trace straight back to a declaration -- a literal, `self`'s or a
     parameter's declared type, a class field's declared type, a `let`
     whose own value was itself confidently typed, or (below) a `Call`
     whose *declared signature* (a free fn, a class method, or an
     interface method's signature) is known -- and returns `None` for
     everything else, which is exactly this check's "stay silent when
     underivable" contract. Its own `cenv` threads through every
     statement in lockstep with `env` below (same `Let`/`If`/`While`/
     `For` shape, same scoping quirks) but never shares storage with
     it, so a placeholder that leaks into `env` through a `let` never
     contaminates `cenv`.

     A *declared* return type is the opposite of underivable, so a
     `Call` is not blanket-skipped the way `Index`/`Binary` are: a free
     fn's or method's own signature sits in the symbol table exactly
     like a field's declared type does, and passing its result to a
     builtin without ever narrowing it is the same class of bug
     `print(7)` is -- `print(takesSecret(box))` where `takesSecret`
     is declared `-> Int` is exactly as wrong as `print(7)`, just one
     call deeper. A declared return type of `None` (no `-> T` written
     at all) is `TVoid`, not another guess -- still confident, since a
     fn/method with no declared return genuinely has no value to hand
     back in this grammar. What still isn't chased: a *qualified*
     free-fn call (`mod.fn(...)`) falls through the `Field` case below
     with a use-alias `base` that never resolves as a value, landing on
     `None`; `multi_new`/`map_new` (contextual on the destination,
     `builtin_confident_ret`'s own doc comment); and an
     UNKNOWN-BUT-RESERVED stdlib call (`fs.stat(...)`) has no signature
     to find in the first place, so it also falls through to `None`
     unchanged. *)
  let rec confident_typ (cenv : typ StringMap.t) (e : expr) : typ option =
    match e.kind with
    | IntLit _ -> Some (TScalar "Int")
    | StrLit _ -> Some (TScalar "Text")
    | BoolLit _ -> Some (TScalar "Bool")
    (* A non-empty list literal is confident about its element type; an
       empty `[]`/`{}` is contextual on its destination, exactly like
       `multi_new()`/`map_new()` above. *)
    | ListLit (first :: _) -> (
        match confident_typ cenv first with Some t -> Some (TMulti t) | None -> None)
    | ListLit [] | MapLit -> None
    (* haxe-parity Task 6: `nil` is contextual on its destination, the same
       as an empty container literal — nothing about the literal itself
       says which `?T` it is the absent value of. *)
    | NilLit -> None
    (* haxe-parity Task 5: a `try` expression's type is its try arm's — the
       handler is checked to agree (typecheck_expr below), so either arm
       would answer, and the try arm is the one that always has a value. *)
    | Try { body; _ } -> confident_typ cenv body
    | Ident name -> StringMap.find_opt name cenv
    | Field (base, field_name) -> (
        match confident_typ cenv base with
        | Some (TScalar class_name) -> (
            match StringMap.find_opt class_name syms.classes with
            | None -> None
            | Some cls -> (
                match List.find_opt (fun (fname, _, _, _) -> fname = field_name) cls.fields with
                | Some (_, field_ty, _, _) -> Some (resolve_field_ty field_ty)
                | None -> None))
        | _ -> None)
    | Call (callee, args) -> (
        match callee.kind with
        | Ident name -> (
            match resolve_free_fn name with
            | Some fi -> ( match fi.ret with Some ft -> Some (resolve_field_ty ft) | None -> Some TVoid)
            | None -> (
                (* haxe-parity Task 4: a payload-variant construction
                   (`Failed("boom")`) is confidently its union's own
                   type — the variant name is a declaration lookup, the
                   same "traceable straight back to a declaration"
                   standard every other confident shape here meets. A
                   declared free fn of the same name already won above
                   (the shadowing rule); a call position can never be a
                   local, so no lexical-shadowing hazard either (the
                   bare-Ident variant reference is deliberately NOT
                   derived here for exactly that reason — cenv cannot
                   distinguish an unconfident local from an unbound
                   name). *)
                match find_variant syms name with
                | Some (u, _) -> Some (TScalar u.u_name)
                | None -> (
                    match List.find_opt (fun (n, _, _) -> n = name) builtin_signatures with
                    | None -> None
                    | Some _ ->
                        let arg0 = match args with a :: _ -> confident_typ cenv a | [] -> None in
                        builtin_confident_ret name arg0)))
        | Field (base, mname) -> (
            (* A method call, dispatched by the *receiver's* confident
               type -- a concrete class first (an ordinary method), an
               interface second (structural dispatch, ICALL). A
               use-alias `base` (a qualified free-fn call) never
               resolves here at all -- `confident_typ` has no binding
               for a bare module alias, only for locals/params/`self`
               -- so it falls straight to `None`, matching the existing
               exemption for stdlib-qualified calls elsewhere in this
               check. *)
            match confident_typ cenv base with
            | Some (TScalar class_name) -> (
                match StringMap.find_opt class_name syms.classes with
                | Some cls -> (
                    match List.find_opt (fun (m : method_info) -> m.name = mname) cls.methods with
                    | Some m -> (
                        match m.ret with Some ft -> Some (resolve_field_ty ft) | None -> Some TVoid)
                    | None -> None)
                | None -> (
                    match StringMap.find_opt class_name syms.interfaces with
                    | Some iface -> (
                        match
                          List.find_opt (fun (s : method_sig_info) -> s.name = mname) iface.methods
                        with
                        | Some s -> (
                            match s.ret with Some ft -> Some (resolve_field_ty ft) | None -> Some TVoid)
                        | None -> None)
                    | None -> None))
            | _ -> (
                (* haxe-parity Task 7: a static call (`Flock.held(path)`).
                   The base names a class, so it has no confident *value*
                   type above — only this shape reaches here with a
                   resolvable member. *)
                match base.kind with
                | Ident cls_name -> (
                    match static_method_of syms cls_name mname with
                    | Some m -> (
                        match m.ret with Some ft -> Some (resolve_field_ty ft) | None -> Some TVoid)
                    | None -> None)
                | _ -> None))
        | _ -> None)
    | Ctor (class_name, _fields) ->
        (* `Ctor`'s class name is never a placeholder -- unlike
           `Ident`/`Field`/`Index`/`Call`, there is no fallback path
           that invents a `Ctor` node, so the name it carries is always
           exactly what the source wrote. A real declared class makes
           this confident (and is also what lets a receiver built the
           ordinary way, `let b = Box{}`, feed the method-call
           derivation above through `b`'s own `let` binding); an
           unknown class name is already `WO-E207`'s own territory, not
           a fallback this check should trust. *)
        if StringMap.mem class_name syms.classes then Some (TScalar class_name) else None
    | Binary ((Eq | Ne | Lt | Le | Gt | Ge | And | Or), _, _) ->
        (* Unlike Add/Sub/Concat/etc. (still "not chased" below — a real
           placeholder-avoidance gap, not this task's to fix), a
           comparison or `and`/`or` is confidently `Bool` regardless of
           its operands' own types: that is what the operator *means*,
           not a guess the way `TScalar "Int"` would be for e.g. `Index`.
           This is what lets the and/or operand check (below,
           typecheck_expr's own `Binary` case) see through
           `a == 1 and b == 2` without a false "underivable" silence on
           the left-hand comparison. *)
        Some (TScalar "Bool")
    | Interp _ ->
        (* An interpolation always *produces* Text by construction
           (emit.ml decides, per-segment, whether the embedded value
           needs `int_to_text` first) -- unlike the placeholders below,
           this is a fact, not a guess. *)
        Some (TScalar "Text")
    | Index _ | Unary _ | Binary _ | DbStub _ ->
        (* Not chased: `Index`/the arithmetic-ladder `Binary` ops have no
           reliable per-node type in this pass at all (see above);
           `Unary`/`DbStub` would be cheap to add but nothing in this
           task's fixtures or the log-watcher sample needs them, and a
           narrower deriver is the safer default. *)
        None
    | Switch _ ->
        (* haxe-parity Task 3: same "not chased" call as `Index`/`Binary`
           above — `typecheck_expr`'s own `Switch` case (below) is the
           real, full derivation (arm unification, the default-required
           check); a `switch` as a WO-E209 builtin argument (e.g.
           `print(switch x {...})`) is not a shape any fixture or the
           sample needs, so this narrower deriver stays silent rather
           than duplicating that logic here. *)
        None
  in

  let rec typecheck_expr (env : typ StringMap.t) (cenv : typ StringMap.t) (e : expr) :
      expr_type_result =
    match e.kind with
    | IntLit _ -> { typ = TScalar "Int"; is_nil = false }
    | StrLit _ -> { typ = TScalar "Text"; is_nil = false }
    | BoolLit _ -> { typ = TScalar "Bool"; is_nil = false }
    | Ident name ->
        (try
           let t = StringMap.find name env in
           { typ = t; is_nil = false }
         with Not_found -> { typ = TScalar "Int"; is_nil = false })
    | Field (base, field_name) ->
        let base_res = typecheck_expr env cenv base in
        (match base_res.typ with
         | TScalar class_name ->
             (* Only a *declared* class can be checked for a missing field.
                typecheck_expr falls back to `TScalar "Int"` for everything
                it cannot type yet (an unresolved builtin call such as the
                spec's own `latest(...)`, an indexed element), so reporting
                on a non-class base turned every one of those placeholders
                into a bogus "unknown field" error -- spec section 3's
                `latest(self.prices).amount` was one. Precision here comes
                back when builtin signatures land; Task 7 surfaced this by
                being the first stage to run the typechecker over a whole
                method body from the CLI. *)
             (match StringMap.find_opt class_name syms.classes with
              | None -> { typ = TScalar "Int"; is_nil = false }
              | Some cls ->
                (match List.find_opt (fun (fname, _, _, _) -> fname = field_name) cls.fields with
                 | Some (_, field_ty, _, _) -> { typ = resolve_field_ty field_ty; is_nil = false }
                 | None ->
                     Diag.Collector.add collector
                       (Diag.error ~code:unknown_field_code ~file ~line:e.pos.line ~col:e.pos.col
                          ~message:(Printf.sprintf "unknown field `%s` on `%s`" field_name class_name) ());
                     { typ = TScalar "Int"; is_nil = false }))
         | _ -> { typ = TScalar "Int"; is_nil = false })
    | Index (base, idx) ->
        let _ = typecheck_expr env cenv base in
        let _ = typecheck_expr env cenv idx in
        { typ = TScalar "Int"; is_nil = false }
    | Call (callee, args) ->
        List.iter (fun arg -> ignore (typecheck_expr env cenv arg)) args;
        (match callee.kind with
         | Ident name when Option.is_none (resolve_free_fn name) -> (
             (* "A user-declared free fn of the same name always wins"
                (08-builtin-surface.md's shadowing rule) -- resolved
                through `resolve_free_fn` (own module, then used
                modules), NOT the flat `syms.free_fns`: two modules
                sharing a free-fn name would let the flat table pick
                whichever merged first, shadowing the builtin from a
                file that never actually `use`s the module that
                collided with it. A `free_fns` hit means this isn't a
                builtin call at all, so this check has nothing to say
                about it (WO-E203's fn/method arity gap is a separate,
                pre-existing, not-this-task's-to-fix hole). A qualified
                call (`mod.fn(...)`) never has an `Ident` callee -- its
                callee is a `Field` -- so a stdlib call through a
                `use`d alias is exempt automatically, matching its
                UNKNOWN-BUT-RESERVED typing everywhere else. *)
             match find_variant syms name with
             | Some (u, vi) ->
                 (* haxe-parity Task 4: constructing a payload variant by
                    name — the argument count must match the variant's
                    declared payload fields exactly (they are positional).
                    WO-E203 (bad arity), reserved since plan 2 Task 6:
                    this is its first real emission site, scoped to
                    variant constructions (fn/method call arity stays the
                    emitter's WO-E403, unchanged). *)
                 let want = List.length vi.vi_fields and got = List.length args in
                 if got <> want then
                   Diag.Collector.add collector
                     (Diag.error ~code:bad_arity_code ~file ~line:e.pos.line ~col:e.pos.col
                        ~message:
                          (Printf.sprintf
                             "variant `%s` of `%s` takes %d payload argument(s), given %d"
                             vi.vi_name u.u_name want got)
                        ())
             | None ->
                 let confident_types = List.map (confident_typ cenv) args in
                 check_builtin_call ~file collector name e.pos args confident_types)
         | _ -> ());
        { typ = TScalar "Int"; is_nil = false }
    | Unary (_, operand) -> typecheck_expr env cenv operand
    | Binary ((And | Or) as op, left, right) ->
        (* haxe-parity Task 2: `Bool`-typed operands only, no truthiness
           -- wired through the same E209-style "confident-type, stay
           silent when underivable" contract check_builtin_call already
           uses, rather than a duplicate of it, per this task's own
           design note. Chose WO-E201 (`type_mismatch_code`): declared
           in this file since Task 6, never given a real emission site
           until now (docs/plan/oop-vm/01-error-catalog.md's own
           "Reserved, not yet emitted" list) -- and "a non-Bool operand
           where Bool was required" is exactly what that name promises,
           so this is the clean case the brief's own note anticipated,
           not the fallback ("reuse the invalid-operand pattern"). *)
        let _ = typecheck_expr env cenv left in
        let _ = typecheck_expr env cenv right in
        let op_name = match op with And -> "and" | _ -> "or" in
        let check_operand (operand : expr) =
          match confident_typ cenv operand with
          | None -> () (* underivable -- stay silent, no false positives *)
          | Some t ->
              (* `unwrap_nullable` accepts a `?Bool` operand silently --
                 no forced-handling diagnostic for the nil case, same
                 shape as the pre-existing, disclosed `?T`-enforcement
                 gap (docs/plan/compiler/nullable-types-implementation.md:
                 "?T is plumbed but not enforced"). Task 6's own
                 WO-E211/E212/E213 work should revisit this call site
                 too, not just field/return positions. *)
              if unwrap_nullable t <> TScalar "Bool" then
                Diag.Collector.add collector
                  (Diag.error ~code:type_mismatch_code ~file ~line:operand.pos.line
                     ~col:operand.pos.col
                     ~message:
                       (Printf.sprintf
                          "`%s` operand must be `Bool`, got `%s` -- no truthiness in this language"
                          op_name (typ_label t))
                     ())
        in
        check_operand left;
        check_operand right;
        { typ = TScalar "Bool"; is_nil = false }
    | Binary ((Eq | Ne), left, right) ->
        let _ = typecheck_expr env cenv left in
        let _ = typecheck_expr env cenv right in
        (* Task 4 fix round 1 (review Major): two bare unions share the
           ordinal tag representation, so `X == P` across two DIFFERENT
           unions was silently true whenever the ordinals matched. An
           operand's union is derived from `confident_typ` or — for a
           bare variant reference, which that deriver deliberately skips
           — from the variant table, locals winning first (`env`, the
           Task 1 shadowing rule). Same-union comparison stays legal
           (bare tags compare exactly); one union against anything else
           is left to Task 6's wider porosity work, disclosed. *)
        let union_of_operand (e : expr) : union_info option =
          match confident_typ cenv e with
          | Some t -> (
              match unwrap_nullable t with
              | TScalar n -> StringMap.find_opt n syms.unions
              | _ -> None)
          | None -> (
              match e.kind with
              | (Ident n | Call ({ kind = Ident n; _ }, _)) when not (StringMap.mem n env) -> (
                  match find_variant syms n with Some (u, _) -> Some u | None -> None)
              | _ -> None)
        in
        (match (union_of_operand left, union_of_operand right) with
         | Some ul, Some ur when ul.u_name <> ur.u_name ->
             Diag.Collector.add collector
               (Diag.error ~code:type_mismatch_code ~file ~line:e.pos.line ~col:e.pos.col
                  ~message:
                    (Printf.sprintf
                       "cannot compare `%s` with `%s` — values of different unions never \
                        compare equal (their tags merely share a representation)"
                       ul.u_name ur.u_name)
                  ())
         | _ -> ());
        { typ = TScalar "Bool"; is_nil = false }
    | Binary (_, left, right) ->
        let _ = typecheck_expr env cenv left in
        let _ = typecheck_expr env cenv right in
        { typ = TScalar "Bool"; is_nil = false }
    | Interp inner ->
        let _ = typecheck_expr env cenv inner in
        { typ = TScalar "Text"; is_nil = false }
    | Ctor (class_name, fields) ->
        (try
           let cls = StringMap.find class_name syms.classes in
           let provided = List.map (fun (n, _) -> n) fields in
           (* haxe-parity Task 4: a field with a declared default is
              omittable (the emitter now fills it — `TailState {}`, the
              sample's own pattern), and so is a `?`-typed field
              ("?fields land as nullable-by-shape": omitted means nil,
              the zero word NEW already leaves there). Everything else
              stays WO-E206, classes and records alike. *)
           let omittable (default : default_expr option) (fty : field_ty) : bool =
             Option.is_some default || (match fty with Nullable _ -> true | _ -> false)
           in
           List.iter (fun (fname, fty, fdefault, _) ->
             if not (List.mem fname provided) && not (omittable fdefault fty) then
               Diag.Collector.add collector
                 (Diag.error ~code:incomplete_ctor_code ~file ~line:e.pos.line ~col:e.pos.col
                    ~message:(Printf.sprintf "missing field `%s` in constructor of `%s`" fname class_name) ())
           ) cls.fields;
           { typ = TScalar class_name; is_nil = false }
         with Not_found ->
           Diag.Collector.add collector
             (Diag.error ~code:unknown_type_code ~file ~line:e.pos.line ~col:e.pos.col
                ~message:(Printf.sprintf "unknown type `%s` in constructor" class_name) ());
           { typ = TScalar "Int"; is_nil = false })
    | DbStub _ -> { typ = TVoid; is_nil = false }
    | Switch (subject, arms) -> typecheck_switch ~want_value:true env cenv subject arms
    | ListLit items ->
        let elem_types = List.map (fun i -> (typecheck_expr env cenv i).typ) items in
        (* Same contextual answer the builtin container constructors give
           when the destination is what decides (see confident_typ): an
           empty literal has no element type to report. *)
        (match elem_types with
         | t :: _ -> { typ = TMulti t; is_nil = false }
         | [] -> { typ = TScalar "Int"; is_nil = false })
    | MapLit -> { typ = TScalar "Int"; is_nil = false }
    (* `is_nil` is what marks the literal: the `typ` is the same
       placeholder every contextual value here reports, and the flag is
       what lets a comparison or a binding treat it as the absent value of
       whatever `?T` it meets. *)
    | NilLit -> { typ = TScalar "Int"; is_nil = true }
    | Try { body; ename; handler } ->
        let body_res = typecheck_expr env cenv body in
        (* The catch arm sees exactly one new name: the error record. *)
        let herr = TScalar error_record_name in
        let benv = StringMap.add ename herr env in
        let bcenv = StringMap.add ename herr cenv in
        let handler_res =
          match List.rev handler with
          | [] -> None
          | last :: rev_init -> (
              let env', cenv' = List.fold_left typecheck_stmt (benv, bcenv) (List.rev rev_init) in
              match last.s_kind with
              | ExprStmt e -> Some (confident_typ cenv' e, e.pos)
              | _ ->
                  let _ = typecheck_stmt (env', cenv') last in
                  None)
        in
        (* Both arms must yield one type where the value is used. Reported
           only when BOTH types are confident — the same "stay silent when
           underivable" contract every other confident-type consumer here
           follows, which also keeps a `catch (e) nil` arm quiet until
           optionals land. *)
        (match (handler_res, confident_typ cenv body) with
         | Some (Some ht, hpos), Some bt when not (typ_equal syms ht bt) ->
             Diag.Collector.add collector
               (Diag.error ~code:type_mismatch_code ~file ~line:hpos.line ~col:hpos.col
                  ~message:
                    (Printf.sprintf
                       "the `catch` arm yields `%s`, but the `try` arm yields `%s` — both arms of \
                        a `try` expression must have one type"
                       (typ_label ht) (typ_label bt))
                  ())
         | _ -> ());
        { typ = body_res.typ; is_nil = false }

  (* haxe-parity Task 3: the one deriver behind both `Switch` call sites
     -- `typecheck_expr`'s own case above (every "the value is used"
     position: a `let`'s value, a `return`, nested inside another expr
     -- reached generically, with zero extra code per call site, simply
     because `typecheck_expr` is what every one of those already
     recurses through) and `typecheck_stmt`'s `ExprStmt` case below (the
     one position where the value is NOT used -- "statement position is
     the expression with a discarded value", the brief's own words, so
     this is the ONE place `want_value` differs from the default). Both
     modes always fully typecheck every arm's body (side effects/
     diagnostics inside an arm are never skipped); `want_value` only
     gates whether a value is *required and unified* across arms.

     Default-required (WO-E208): unconditional here, because no union
     type exists yet (`typ` above has no `TUnion` -- ast.ml's own module
     doc). This is deliberately the exact shape Task 4 extends, not a
     bespoke check to replace: add a `TUnion variants` arm that walks
     the arms' `values` for coverage (naming the missing variants) and
     falls through to this same E208 for every other subject type,
     unchanged.

     Arm typing avoids double-typechecking each arm's own trailing
     expression: `typecheck_stmt` already knows how to fold a `stmt
     list`, but it discards each statement's own expr *type* (only
     `typecheck_expr`'s caller sees that) -- so the last statement is
     handled specially here rather than calling `typecheck_stmt` on the
     whole body and then re-deriving the tail's type a second time
     (which `diag.ml`'s own (code,file,line,col) dedup would make
     harmless, but doing the double work at all is needless). An arm
     whose last statement is not `ExprStmt` (e.g. every arm in the
     sample's own statement-position switches, which end in `return`)
     types as `TVoid` -- not a special case, just what "no value here"
     naturally is, and it is what makes an arm that fails to yield a
     value in expression position surface as an ordinary arm-type-
     mismatch against its sibling arms, with no separate diagnostic. *)
  and typecheck_switch ~(want_value : bool) (env : typ StringMap.t) (cenv : typ StringMap.t)
      (subject : expr) (arms : switch_arm list) : expr_type_result =
    let subj_res = typecheck_expr env cenv subject in
    (* review fix, Critical 2: a case label whose type is confidently
       Text against a non-Text scalar subject (or vice versa) is not
       just a type error — it is a real VM segfault (reviewer-
       reproduced: `switch s { case 1: ... }` over `s: Text` emits
       EQS on a raw int register, and the VM's `str_check` dereferences
       it as a `wo_str*`; the reverse direction — an Int subject with a
       Text label — is equally wrong, just silently always-false
       rather than a crash).

       Deliberately built on `confident_typ`, NOT `typecheck_expr`'s
       own `.typ` (round-1 mistake, self-caught before shipping):
       `typecheck_expr` hands back `TScalar "Int"` for *any*
       unresolved base (an unbound `Ident`, and therefore any `Field`
       read through one) — exactly the placeholder that made
       `j.method` (log-watcher's own `mcp.wo`, where `j`'s own `let ...
       as RpcReq` fails to parse today, leaving `j` unbound) look
       "confidently Int" and false-positive against its very real
       `"initialize"`/`"ping"`/... Text case labels. `confident_typ`
       returns `None` for exactly that shape (an unbound `Ident`'s
       `Field`), so this stays silent there — the same "confident, stay
       silent when underivable" contract WO-E209 already established.
       `repr_kind` narrows a confident type to exactly the two
       representations emit.ml's own EQ-vs-EQS choice cares about
       (`WO_K_TEXT` vs `WO_K_SCALAR`); anything else (an unresolved or
       union-typed subject — the sample's own `switch res {...}`/
       `switch names {...}` sites, Task 4's territory, already
       WO-E207'd) is `Other` and never compared. Reuses WO-E201
       (`type_mismatch_code`) — the same code the arm-unification check
       below uses — per the review's own instruction ("wire through
       E201 like arm mismatch"). *)
    let repr_kind (t : typ) : [ `Text | `Scalar | `Other ] =
      match wob_kind_of_typ syms t with
      | WO_K_TEXT -> `Text
      | WO_K_SCALAR -> `Scalar
      | WO_K_OWNED | WO_K_GCREF | WO_K_MULTI | WO_K_MAP | WO_K_NULLABLE -> `Other
    in
    (* haxe-parity Task 4: a union-typed subject switches the arms from
       VALUE comparisons to variant PATTERNS — `case Ok:` names a
       variant (never an expression to evaluate), `case Failed(reason):`
       additionally binds the payload fields for that arm's own body.
       Derived through `confident_typ` exactly like the repr check below
       (and NOT unwrapped through `?T`: a `?Union` subject stays on the
       plain-value path until Task 6's forced-handling work legalizes
       narrowing it) — an underivable subject falls through to the
       scalar rules unchanged, the same "stay silent when underivable"
       contract as every other confident-type consumer. *)
    let subj_union =
      match confident_typ cenv subject with
      | Some (TScalar n) -> StringMap.find_opt n syms.unions
      | _ -> None
    in
    (* Task 4 fix round 1 (review Critical 2): a `?Union` subject with a
       variant-named case compiled clean and could NEVER match — the
       case constructed a fresh variant object (or, for a payload
       pattern, fell into the emitter as an unbound name) and the
       pointer compare was always false, so `default` always won. A
       `?`-typed union is not narrowed here (narrowing is Task 6's
       forced-handling work, deliberately not implemented), so variant
       patterns are meaningless over it — say so, pointing at the nil
       case first. *)
    let subj_opt_union =
      match confident_typ cenv subject with
      | Some (TNullable inner) -> (
          match unwrap_nullable inner with
          | TScalar n -> StringMap.find_opt n syms.unions
          | _ -> None)
      | _ -> None
    in
    (match subj_opt_union with
     | None -> ()
     | Some u ->
         List.iter
           (fun (a : switch_arm) ->
             List.iter
               (fun (v : expr) ->
                 match v.kind with
                 | (Ident n | Call ({ kind = Ident n; _ }, _))
                   when Option.is_some (find_variant syms n) ->
                     Diag.Collector.add collector
                       (Diag.error ~code:type_mismatch_code ~file ~line:v.pos.line
                          ~col:v.pos.col
                          ~message:
                            (Printf.sprintf
                               "`%s` can never match here — the subject is `?%s`, which may be \
                                nil and is not narrowed by `switch`; handle the nil case first \
                                (forced `?T` handling arrives with Task 6)"
                               n u.u_name)
                          ())
                 | _ -> ())
               a.values)
           arms);
    (match subj_union with
     | None -> ()
     | Some u ->
         let variant_of name = List.find_opt (fun v -> v.vi_name = name) u.u_variants in
         let pattern_err ~code (pos : pos) message =
           Diag.Collector.add collector
             (Diag.error ~code ~file ~line:pos.line ~col:pos.col ~message ())
         in
         List.iter
           (fun (a : switch_arm) ->
             List.iter
               (fun (v : expr) ->
                 match v.kind with
                 | Ident vname -> (
                     match variant_of vname with
                     | Some _ -> ()
                     | None ->
                         pattern_err ~code:type_mismatch_code v.pos
                           (Printf.sprintf "`%s` is not a variant of union `%s`" vname u.u_name))
                 | Call ({ kind = Ident vname; _ }, args) -> (
                     match variant_of vname with
                     | None ->
                         pattern_err ~code:type_mismatch_code v.pos
                           (Printf.sprintf "`%s` is not a variant of union `%s`" vname u.u_name)
                     | Some vi ->
                         let want = List.length vi.vi_fields and got = List.length args in
                         if got <> want then
                           pattern_err ~code:bad_arity_code v.pos
                             (Printf.sprintf
                                "pattern for `%s` binds %d payload field(s), but `%s` declares %d"
                                vi.vi_name got vi.vi_name want)
                         else if
                           not
                             (List.for_all
                                (fun (arg : expr) ->
                                  match arg.kind with Ident _ -> true | _ -> false)
                                args)
                         then
                           pattern_err ~code:bad_arity_code v.pos
                             (Printf.sprintf
                                "pattern for `%s` must bind plain names — payload fields are \
                                 bound positionally, never matched by value"
                                vi.vi_name)
                         else if List.length a.values > 1 then
                           pattern_err ~code:bad_arity_code v.pos
                             (Printf.sprintf
                                "a payload-binding pattern (`%s(...)`) must be its arm's only \
                                 value"
                                vi.vi_name))
                 | _ ->
                     pattern_err ~code:type_mismatch_code v.pos
                       (Printf.sprintf
                          "switch over union `%s` matches variants — this case value is not one"
                          u.u_name))
               a.values)
           arms);
    (match (if Option.is_none subj_union then confident_typ cenv subject else None) with
     | None -> () (* subject underivable (or a union, handled above) -- stay silent *)
     | Some subj_t ->
         let subj_repr = repr_kind subj_t in
         List.iter
           (fun (a : switch_arm) ->
             List.iter
               (fun v ->
                 ignore (typecheck_expr env cenv v);
                 (* Task 4 fix round 1 (review Major): the inverse of the
                    union-subject direction — a case value naming a KNOWN
                    variant while the subject is confidently some other
                    type silently ordinal-matched (`switch n { case Lo: }`
                    over `n: Int` matched n == 0). Lexical scope wins
                    first (`env`, every local/param — the Task 1
                    shadowing lesson), so a local that happens to share a
                    variant's name is never misread as one. *)
                 (match v.kind with
                  | Ident n | Call ({ kind = Ident n; _ }, _) -> (
                      if not (StringMap.mem n env) then
                        match find_variant syms n with
                        | Some (vu, _) ->
                            Diag.Collector.add collector
                              (Diag.error ~code:type_mismatch_code ~file ~line:v.pos.line
                                 ~col:v.pos.col
                                 ~message:
                                   (Printf.sprintf
                                      "`%s` is a variant of union `%s`, but the switch subject \
                                       has type `%s`"
                                      n vu.u_name (typ_label subj_t))
                                 ())
                        | None -> ())
                  | _ -> ());
                 match confident_typ cenv v with
                 | None -> ()
                 | Some vt -> (
                     match (repr_kind vt, subj_repr) with
                     | (`Text, `Scalar | `Scalar, `Text) ->
                         Diag.Collector.add collector
                           (Diag.error ~code:type_mismatch_code ~file ~line:v.pos.line
                              ~col:v.pos.col
                              ~message:
                                (Printf.sprintf
                                   "switch case value has type `%s`, but the switch subject \
                                    has type `%s`"
                                   (typ_label vt) (typ_label subj_t))
                              ())
                     | _ -> ()))
               a.values)
           arms);
    (* review fix, Critical 1 (the warning half — the reorder itself is
       ast.ml's `switch_lowering_order`, applied downstream in
       owner.ml/emit.ml, not here): a `case` arm textually after
       `default` no longer silently loses to it (that was the bug),
       but it is still surprising source, so this warns once per
       switch shaped that way, anchored at `default`'s own position. *)
    let default_pos = ref None in
    let case_after_default = ref false in
    List.iter
      (fun (a : switch_arm) ->
        match !default_pos with
        | None -> if a.is_default then default_pos := Some a.arm_pos
        | Some _ -> if not a.is_default then case_after_default := true)
      arms;
    (match !default_pos with
     | Some pos when !case_after_default ->
         Diag.Collector.add collector
           (Diag.warning ~code:switch_default_not_last_code ~file ~line:pos.line ~col:pos.col
              ~message:
                "`default` is not the last arm -- a `case` written after it still matches (this \
                 compiler evaluates `default` last regardless of source position), which reads \
                 as dead code"
              ())
     | _ -> ());
    (* WO-E208. Union subjects get the exhaustiveness rule the spec's
       switch row promised ("`default` optional when exhaustive"): no
       `default` is fine exactly when every variant is covered by some
       arm; a gap names the missing variants, in declaration order.
       Every other subject keeps Task 3's unconditional rule — scalars
       and Text always require `default`. *)
    (if not (List.exists (fun (a : switch_arm) -> a.is_default) arms) then
       match subj_union with
       | Some u ->
           let covered =
             List.concat_map
               (fun (a : switch_arm) ->
                 List.filter_map
                   (fun (v : expr) ->
                     match v.kind with
                     | Ident n | Call ({ kind = Ident n; _ }, _) -> Some n
                     | _ -> None)
                   a.values)
               arms
           in
           let missing =
             List.filter (fun vi -> not (List.mem vi.vi_name covered)) u.u_variants
           in
           if missing <> [] then
             Diag.Collector.add collector
               (Diag.error ~code:non_exhaustive_switch_code ~file ~line:subject.pos.line
                  ~col:subject.pos.col
                  ~message:
                    (Printf.sprintf
                       "switch over `%s` has no `default` arm and does not cover: %s" u.u_name
                       (String.concat ", " (List.map (fun vi -> vi.vi_name) missing)))
                  ())
       | None ->
           Diag.Collector.add collector
             (Diag.error ~code:non_exhaustive_switch_code ~file ~line:subject.pos.line
                ~col:subject.pos.col
                ~message:
                  (Printf.sprintf "switch over `%s` has no `default` arm" (typ_label subj_res.typ))
                ()));
    (* haxe-parity Task 4: the names a payload pattern binds, typed with
       the variant's own declared field types, visible to that arm's
       body only. Empty for every non-union subject and every bare/
       malformed pattern (the malformed ones already got their own
       diagnostic above — typing the body against fewer names is the
       do-no-harm fallback, not a second error). *)
    let bindings_of (a : switch_arm) : (string * typ) list =
      (* `?Union` too (fix round 1): the variant-named cases over a
         `?Union` subject are already a hard WO-E201 (above) — binding
         the pattern's names anyway keeps the arm BODY typed against
         real names instead of cascading a second, misleading
         arm-mismatch error off an unbound placeholder. *)
      match (match subj_union with Some _ as u -> u | None -> subj_opt_union) with
      | None -> []
      | Some u -> (
        match a.values with
        | [ { kind = Call ({ kind = Ident vname; _ }, args); _ } ] -> (
            match List.find_opt (fun vi -> vi.vi_name = vname) u.u_variants with
            | Some vi when List.length args = List.length vi.vi_fields ->
                List.concat
                  (List.map2
                     (fun (arg : expr) (_, fty) ->
                       match arg.kind with
                       | Ident bn -> [ (bn, resolve_field_ty fty) ]
                       | _ -> [])
                     args vi.vi_fields)
            | _ -> [])
        | _ -> [])
    in
    let arm_value (a : switch_arm) : typ * pos =
      let bound = bindings_of a in
      let benv = List.fold_left (fun m (n, t) -> StringMap.add n t m) env bound in
      let bcenv = List.fold_left (fun m (n, t) -> StringMap.add n t m) cenv bound in
      match List.rev a.body with
      | [] -> (TVoid, a.arm_pos)
      | last :: rev_init ->
          let env', cenv' = List.fold_left typecheck_stmt (benv, bcenv) (List.rev rev_init) in
          (match last.s_kind with
           | ExprStmt e -> ((typecheck_expr env' cenv' e).typ, e.pos)
           | _ ->
               let _ = typecheck_stmt (env', cenv') last in
               (TVoid, last.s_pos))
    in
    let arm_types = List.map arm_value arms in
    (if want_value then
       match arm_types with
       | [] -> ()
       | (ref_typ, _) :: rest ->
           List.iter
             (fun (t, pos) ->
               (* structural, not (=): two same-shape typedef records are
                  the same type (haxe-parity Task 4, typ_equal). *)
               if not (typ_equal syms t ref_typ) then
                 Diag.Collector.add collector
                   (Diag.error ~code:type_mismatch_code ~file ~line:pos.line ~col:pos.col
                      ~message:
                        (Printf.sprintf
                           "switch arm yields `%s`, but the switch's type is `%s` (from an \
                            earlier arm)"
                           (typ_label t) (typ_label ref_typ))
                      ()))
             rest);
    match arm_types with (t, _) :: _ -> { typ = t; is_nil = false } | [] -> { typ = TVoid; is_nil = false }

  (* `typecheck_switch` (above) folds `typecheck_stmt` over an arm's own
     body, and `typecheck_stmt`'s `ExprStmt` case (below) calls
     `typecheck_switch` in `want_value:false` mode -- the two are
     mutually recursive, so they (and `typecheck_expr`, which
     `typecheck_switch` also calls) must be one `and`-chain, not the
     three separate `let rec ... in` bindings this function had before
     this task. *)
  and typecheck_stmt ((env, cenv) : typ StringMap.t * typ StringMap.t) (s : stmt) :
      typ StringMap.t * typ StringMap.t =
    match s.s_kind with
    | Let { name; ty; value } ->
        let val_res = typecheck_expr env cenv value in
        (* A written annotation is the authority — it is the only thing
           that types a contextual value (`[]`, `{}`, `nil`), and for
           everything else it is what the author declared the binding to
           be. Only an unannotated `let` falls back to inference. *)
        let declared = Option.map resolve_field_ty ty in
        let bound_typ = match declared with Some t -> t | None -> val_res.typ in
        let new_cenv =
          match (declared, confident_typ cenv value) with
          | Some t, _ -> StringMap.add name t cenv
          | None, Some t -> StringMap.add name t cenv
          | None, None -> StringMap.remove name cenv
        in
        (StringMap.add name bound_typ env, new_cenv)
    | Assign { target; value } ->
        let _ = typecheck_expr env cenv target in
        let _ = typecheck_expr env cenv value in
        (env, cenv)
    | If { cond; then_body; else_body } ->
        let _ = typecheck_expr env cenv cond in
        let then_result = List.fold_left typecheck_stmt (env, cenv) then_body in
        (match else_body with
         | Some (_, else_body) -> List.fold_left typecheck_stmt (env, cenv) else_body
         | None -> then_result)
    | While { cond; body } ->
        let _ = typecheck_expr env cenv cond in
        List.fold_left typecheck_stmt (env, cenv) body
    | For { var; iter; body } ->
        let iter_res = typecheck_expr env cenv iter in
        let env_body = StringMap.add var iter_res.typ env in
        let cenv_body =
          match confident_typ cenv iter with
          | Some (TMulti inner_t) -> StringMap.add var inner_t cenv
          | _ -> StringMap.remove var cenv
        in
        List.fold_left typecheck_stmt (env_body, cenv_body) body
    | Return opt_e ->
        (match opt_e with Some e -> let _ = typecheck_expr env cenv e in () | None -> ());
        (env, cenv)
    | ExprStmt { kind = Switch (subject, arms); _ } ->
        (* haxe-parity Task 3: the one place `want_value` is false --
           "statement position is the expression with a discarded
           value" (the brief's own words). Every arm's body is still
           fully typechecked (typecheck_switch's own contract); nothing
           here requires or unifies a value the way the generic
           `Switch` case of `typecheck_expr` (used for every other
           position: `let`, `return`, nested inside another expr) does. *)
        let _ = typecheck_switch ~want_value:false env cenv subject arms in
        (env, cenv)
    | ExprStmt e ->
        let _ = typecheck_expr env cenv e in
        (env, cenv)
    | Break | Continue -> (env, cenv)
    | DoWhile { cond; body } ->
        let _ = typecheck_expr env cenv cond in
        List.fold_left typecheck_stmt (env, cenv) body
  in

  (* `self` is bound to the *enclosing class name*, not a literal "Self":
     "Self" is not a declared class, so every `self.field` access used to
     miss and report a bogus unknown-field error. *)
  let typecheck_method ~(self_class : string) (env : typ StringMap.t) (m : method_info) : bool =
    let param_env = List.fold_left (fun acc (name, ty, _) ->
      StringMap.add name (resolve_field_ty ty) acc) env m.params in
    let env_with_self = StringMap.add "self" (TScalar self_class) param_env in
    (* Every param/`self` is always confidently typed (a declared type
       is mandatory here), so `cenv`'s starting point for a method body
       is exactly `env_with_self`'s own shape -- no ambiguity to guard
       against at the entry to a body, only inside it. *)
    let cenv_with_self = List.fold_left (fun acc (name, ty, _) ->
      StringMap.add name (resolve_field_ty ty) acc)
      (StringMap.singleton "self" (TScalar self_class)) m.params in
    let _ = List.fold_left typecheck_stmt (env_with_self, cenv_with_self) m.body in
    false
  in

  (* Walk THIS FILE's own classes/free_fns only (`file_syms`, not the
     merged `syms`) -- see this function's own doc comment above for
     why. *)
  StringMap.iter (fun _name cls ->
    let method_env = StringMap.empty in
    List.iter (fun m -> ignore (typecheck_method ~self_class:cls.name method_env m)) cls.methods
  ) file_syms.classes;

  StringMap.iter (fun _name (fn : free_fn_info) ->
    let param_env = List.fold_left (fun acc (name, ty, _) ->
      StringMap.add name (resolve_field_ty ty) acc) StringMap.empty fn.params in
    let param_cenv = List.fold_left (fun acc (name, ty, _) ->
      StringMap.add name (resolve_field_ty ty) acc) StringMap.empty fn.params in
    ignore (List.fold_left typecheck_stmt (param_env, param_cenv) fn.body)
  ) file_syms.free_fns;

  ()

(* ============================================================
   Entry point
   ============================================================ *)

(* Single-file convenience wrapper (runner.ml's ~40 direct assertion
   helpers all go through this, never `typecheck_program` directly) --
   its own external signature is unchanged by the hotfix's new
   `~module_of`/`~module_syms` parameters on `typecheck_program`: a
   lone file has no sibling module structure to differ from, so it is
   its own one-and-only module, exactly the convention runner.ml's
   `emit_str` already established for the same single-file case
   (`~module_of:(fun _ -> ".")`, one `module_syms` entry keyed `"."`). *)
let typecheck ~file (prog : program) (collector : Diag.Collector.t) : symbols * unit =
  let syms = collect_declarations ~file prog collector in
  let module_syms = Hashtbl.create 1 in
  Hashtbl.replace module_syms "." syms;
  (* Single file -- `syms` IS this file's own declarations, so it is
     also exactly this call's `~file_syms` (hotfix). *)
  let () =
    typecheck_program ~file ~module_of:(fun _ -> ".") ~module_syms ~file_syms:syms prog syms collector
  in
  (syms, ())

(* ============================================================
   Modules (haxe-parity Task 1): use-based cross-module visibility
   ============================================================

   Layered ON TOP of the existing multi-file discovery (Task 8,
   compiler/bin/main.ml): every file in one module (one directory) still
   sees every other same-module file's declarations unconditionally,
   exactly as before this task — that mechanism (collect_declarations +
   the driver's merge) is UNCHANGED. What is new is a second, additive
   check that walks each file's own `Ctor`/`Call` sites and asks "is the
   declaration this name resolves to actually reachable from here?" —
   own module: always; a `use`d module: only its `pub` names; anything
   else: an error. This never changes *which* declaration a name
   resolves to (main.ml's driver-level merge, emit.ml's lowering, and
   owner.ml's analysis are all untouched) — it only decides whether that
   resolution was legitimate, so a violation is a hard compile error
   before any of those later stages ever run, never a silent shadow.

   A deliberate scope cut, disclosed rather than silently skipped: only
   `Ctor` (constructor-literal class names) and `Call` (bare `fn(...)`
   and qualified `mod.fn(...)`) sites are checked. Field/parameter/
   return *type* positions (`field: OtherModuleClass`) are not
   module-gated by this task — no fixture upstream of this task needs
   it (log-watcher's own 18 `use` lines are all reserved-stdlib, and
   this task's own fixtures exercise classes/fns through construction
   and calls), and bolting it on would mean walking every field_ty in
   every class, a materially bigger surface than the brief's own
   examples ask for. *)

(* `use_edge`/`uses_of_program`/`path_str` moved up above
   `typecheck_program` (hotfix, hoisted alongside `builtin_signatures`) —
   `confident_typ`'s free-fn return-type derivation needs them there too
   now, and OCaml has no forward reference across top-level `let`s. Kept
   conceptually here in reading order; see their real definitions above
   `typecheck_program` for the code. *)

(* Merges bare `symbols` values (no diagnostics — collisions within one
   module are already WO-E214/WO-E215's job, upstream of this) purely to
   group per-file declarations into their shared module's surface. A
   small local twin of main.ml's own merge_symbols rather than a shared
   export: main.ml's version is already exercised by 399 passing checks
   and touching it is not this change's job (see this task's own
   "surgical changes" instruction). *)
let merge_syms_for_module (syms_list : symbols list) : symbols =
  let keep_first _key a _b = Some a in
  List.fold_left
    (fun (acc : symbols) (s : symbols) ->
      {
        classes = StringMap.union keep_first acc.classes s.classes;
        interfaces = StringMap.union keep_first acc.interfaces s.interfaces;
        free_fns = StringMap.union keep_first acc.free_fns s.free_fns;
        typedefs = StringMap.union keep_first acc.typedefs s.typedefs;
        unions = StringMap.union keep_first acc.unions s.unions;
        modules = acc.modules @ s.modules;
      })
    { classes = StringMap.empty; interfaces = StringMap.empty; free_fns = StringMap.empty;
      typedefs = StringMap.empty; unions = StringMap.empty; modules = [] }
    syms_list

(* Generic structural walk over every Ctor/Call site in a program's
   method/fn bodies — deliberately NOT the full typechecker's
   typecheck_expr (that resolves types; this only needs to *locate*
   constructor names and call callees, own recursion kept separate and
   small rather than teaching typecheck_expr a second, unrelated job).

   Threads `bound` — the set of names currently in lexical scope as a
   local/param/`self` — through every visit (IMPORTANT 2 review fix,
   haxe-parity Task 1: `use secret; let secret = Box(); secret.hidden()`
   was a false-positive WO-E217, because the qualified-call check had no
   notion of scope at all and treated any `Ident` matching a `use`
   alias's spelling as that alias, unconditionally. Lexical scope wins
   here exactly like everywhere else in every language with both
   modules and locals — the checker in check_module_refs below is what
   actually acts on `bound`; this walk only has to compute and pass it
   through). Block-scoped like the language's own `let` (a `let` inside
   an `if`'s body does not leak to code after that `if`) — `walk_block`
   folds `bound` across a statement *list*, but every nested-block call
   site passes the accumulated `bound` down and discards what comes
   back, exactly the semantics that keeps a block's own bindings from
   escaping it. *)
let rec walk_block (bound : StringSet.t) (visit : StringSet.t -> expr -> unit) (stmts : stmt list) : unit
    =
  ignore (List.fold_left (fun b s -> walk_stmt b visit s) bound stmts)

and walk_stmt (bound : StringSet.t) (visit : StringSet.t -> expr -> unit) (s : stmt) : StringSet.t =
  match s.s_kind with
  | Let { name; value; _ } ->
    walk_expr bound visit value;
    StringSet.add name bound
  | Assign { target; value } ->
    walk_expr bound visit target;
    walk_expr bound visit value;
    bound
  | If { cond; then_body; else_body } ->
    walk_expr bound visit cond;
    walk_block bound visit then_body;
    (match else_body with Some (_, b) -> walk_block bound visit b | None -> ());
    bound
  | While { cond; body } ->
    walk_expr bound visit cond;
    walk_block bound visit body;
    bound
  | For { var; iter; body } ->
    walk_expr bound visit iter;
    walk_block (StringSet.add var bound) visit body;
    bound
  | Return (Some e) ->
    walk_expr bound visit e;
    bound
  | Return None -> bound
  | ExprStmt e ->
    walk_expr bound visit e;
    bound
  | Break | Continue -> bound
  | DoWhile { cond; body } ->
    walk_block bound visit body;
    walk_expr bound visit cond;
    bound

and walk_expr (bound : StringSet.t) (visit : StringSet.t -> expr -> unit) (e : expr) : unit =
  visit bound e;
  match e.kind with
  | IntLit _ | StrLit _ | BoolLit _ | Ident _ -> ()
  | Field (b, _) -> walk_expr bound visit b
  | Index (b, i) ->
    walk_expr bound visit b;
    walk_expr bound visit i
  | Call (callee, args) ->
    walk_expr bound visit callee;
    List.iter (walk_expr bound visit) args
  | Unary (_, o) -> walk_expr bound visit o
  | Binary (_, l, r) ->
    walk_expr bound visit l;
    walk_expr bound visit r
  | Ctor (_, fields) -> List.iter (fun (_, v) -> walk_expr bound visit v) fields
  | Interp inner -> walk_expr bound visit inner
  | ListLit items -> List.iter (walk_expr bound visit) items
  | MapLit | NilLit -> ()
  | Try { body; ename; handler } ->
    walk_expr bound visit body;
    walk_block (StringSet.add ename bound) visit handler
  | DbStub _ -> ()
  | Switch (subject, arms) ->
      walk_expr bound visit subject;
      List.iter
        (fun (a : switch_arm) ->
          List.iter (walk_expr bound visit) a.values;
          walk_block bound visit a.body)
        arms

let walk_program (visit : StringSet.t -> expr -> unit) (prog : program) : unit =
  List.iter
    (function
      | Class c ->
        List.iter
          (fun (m : method_decl) ->
            let params = List.fold_left (fun acc (p : param) -> StringSet.add p.name acc) (StringSet.singleton "self") m.params in
            walk_block params visit m.body)
          c.methods
      | Interface _ -> ()
      | Fn m ->
        let params = List.fold_left (fun acc (p : param) -> StringSet.add p.name acc) StringSet.empty m.params in
        walk_block params visit m.body
      | Use _ -> ()
      | Const _ -> ()
      | Union _ -> ())
    prog.decls

(* How a name used from this file resolved, against one declaration-kind
   projection (classes for Ctor, free_fns for Call) of the surrounding
   module graph. *)
type resolution =
  | ROwn
  | RUsed of string (* the use-edge alias that supplied it *)
  | RCollision of string list (* every alias whose used module exports it `pub` *)
  | RNotImported of string (* the module id it actually lives in *)
  | RNotFound (* not own, not used, not anywhere else either — pre-existing gap (WO-E204/WO-E207's own territory), not this check's to raise *)

let resolve_name (get : symbols -> 'a StringMap.t) (pub_of : 'a -> bool) ~(own : symbols)
    ~(uses_resolved : (use_edge * symbols) list) ~(others : (string * symbols) list) (name : string) :
    resolution =
  if StringMap.mem name (get own) then ROwn
  else
    let used_hits =
      List.filter_map
        (fun (u, msyms) ->
          match StringMap.find_opt name (get msyms) with
          | Some info when pub_of info -> Some u.ue_alias
          | _ -> None)
        uses_resolved
    in
    match used_hits with
    | [] -> (
      match List.find_opt (fun (_, msyms) -> StringMap.mem name (get msyms)) others with
      | Some (mid, _) -> RNotImported mid
      | None -> RNotFound)
    | [ alias ] -> RUsed alias
    | aliases -> RCollision aliases

let report_not_imported (collector : Diag.Collector.t) ~file (pos : pos) ~(name : string)
    ~(mid : string) : unit =
  Diag.Collector.add collector
    (Diag.error ~code:module_not_imported_code ~file ~line:pos.line ~col:pos.col
       ~message:(Printf.sprintf "`%s` is declared in module `%s`, which is not `use`d here" name mid)
       ())

let report_use_collision (collector : Diag.Collector.t) ~file (pos : pos) ~(name : string)
    ~(aliases : string list) : unit =
  Diag.Collector.add collector
    (Diag.error ~code:use_collision_code ~file ~line:pos.line ~col:pos.col
       ~message:
         (Printf.sprintf "`%s` is ambiguous — exported `pub` by more than one used module (%s)" name
            (String.concat ", " aliases))
       ())

(* Walks one file's Ctor/Call sites, checking each against the module
   graph the caller (check_modules) has already assembled for it.
   `mark_used` is called once per use-edge alias whenever a reference
   actually resolves through it — the unused-`use` warning's own data. *)
let check_module_refs (collector : Diag.Collector.t) ~file ~(own : symbols)
    ~(uses_resolved : (use_edge * symbols) list) ~(stdlib_aliases : string list)
    ~(others : (string * symbols) list) ~(mark_used : string -> unit) (prog : program) : unit =
  let alias_tbl = Hashtbl.create 8 in
  List.iter (fun (u, msyms) -> Hashtbl.replace alias_tbl u.ue_alias msyms) uses_resolved;
  let check_ctor (pos : pos) (name : string) : unit =
    match resolve_name (fun s -> s.classes) (fun (c : class_info) -> c.pub) ~own ~uses_resolved ~others name with
    | ROwn | RNotFound -> ()
    | RUsed alias -> mark_used alias
    | RCollision aliases ->
      (* every alias that contributed to the ambiguity was genuinely
         referenced (that's exactly what makes it ambiguous) — mark
         them all used so the same `use` line doesn't also draw an
         "unused" warning alongside its collision error. *)
      List.iter mark_used aliases;
      report_use_collision collector ~file pos ~name ~aliases
    | RNotImported mid -> report_not_imported collector ~file pos ~name ~mid
  in
  let check_bare_call (pos : pos) (name : string) : unit =
    match resolve_name (fun s -> s.free_fns) (fun (f : free_fn_info) -> f.pub) ~own ~uses_resolved ~others name with
    | ROwn | RNotFound -> ()
    (* RNotFound also covers a builtin (`print`, `len`, ...) or a
       genuinely unresolved name — 08-builtin-surface.md's shadowing
       rule ("a user-declared free fn of the same name always wins")
       already makes ROwn win first when both exist; RNotFound's other
       half (truly nothing resolves) is WO-E204's own pre-existing,
       not-this-task's-to-fix gap (see nullable-types-implementation.md's
       dead-code register) — silence here matches silence there. *)
    | RUsed alias -> mark_used alias
    | RCollision aliases ->
      (* every alias that contributed to the ambiguity was genuinely
         referenced (that's exactly what makes it ambiguous) — mark
         them all used so the same `use` line doesn't also draw an
         "unused" warning alongside its collision error. *)
      List.iter mark_used aliases;
      report_use_collision collector ~file pos ~name ~aliases
    | RNotImported mid -> report_not_imported collector ~file pos ~name ~mid
  in
  let check_qualified_call (bound : StringSet.t) (pos : pos) (alias : string) (member : string) : unit =
    if StringSet.mem alias bound then ()
      (* IMPORTANT 2 review fix: `alias` is a local/param/`self` in scope
         right here — lexical scope wins, exactly like everywhere else
         in every language with both modules and locals. `secret.hidden()`
         where `secret` is `let secret = Box()` is an ordinary method
         call the existing (unrelated, untouched) class-method machinery
         already handles correctly; this module-alias check has nothing
         to say about it at all, not even a used-vs-unused opinion —
         `secret` the module alias was never actually referenced here. *)
    else if List.mem alias stdlib_aliases then mark_used alias
      (* stdlib members arrive in plan 9 — UNKNOWN-BUT-RESERVED here on
         purpose: no E207/E225/arity check, nothing to look up yet. The
         emitter (emit.ml) is the one place that still cares, and only
         if a call through this alias survives all the way to
         emission. *)
    else
      match Hashtbl.find_opt alias_tbl alias with
      | None -> () (* not a use-alias at all: a receiver expression (`obj.method(...)`), unrelated to this check *)
      | Some msyms -> (
        mark_used alias;
        match StringMap.find_opt member msyms.free_fns with
        | None -> () (* unknown fn in that module -- WO-E204's territory, not this check's *)
        | Some (fi : free_fn_info) ->
          if not fi.pub then
            Diag.Collector.add collector
              (Diag.error ~code:private_name_code ~file ~line:pos.line ~col:pos.col
                 ~message:(Printf.sprintf "`%s` is not `pub` in module `%s`" member alias) ()))
  in
  let visit (bound : StringSet.t) (e : expr) : unit =
    match e.kind with
    | Ctor (name, _) -> check_ctor e.pos name
    | Call ({ kind = Ident name; _ }, _) -> check_bare_call e.pos name
    | Call ({ kind = Field ({ kind = Ident alias; _ }, member); pos = fpos; _ }, _) ->
      check_qualified_call bound fpos alias member
    | _ -> ()
  in
  walk_program visit prog

(* Groups per-file declarations by module (directory) and merges within
   each group — the module-scoped analogue of main.ml's existing global
   merge, and every module-aware consumer's shared starting point.
   `module_of` maps a discovered file to its module id (see check_modules'
   own doc comment below for the exact convention). Exposed (not just
   inlined into check_modules) because the emitter needs the same
   per-module tables too — CRITICAL 1 review finding (plan 3): free_fns
   staying one flat, globally-merged StringMap all the way through
   emission is exactly what let `a.thing()`/`b.thing()` both silently
   execute the same (whichever-merged-first) body. emit.ml resolves a
   qualified call against *this* table (the aliased module's own,
   unmangled `free_fns`), never the flat one, so two modules' same-named
   `pub fn` are genuinely distinct once qualification disambiguates them. *)
let module_symbols ~(module_of : string -> string) (per_file_syms : (string * symbols) list) :
    (string, symbols) Hashtbl.t =
  let by_module : (string, symbols list) Hashtbl.t = Hashtbl.create 16 in
  List.iter
    (fun (file, syms) ->
      let mid = module_of file in
      let prev = try Hashtbl.find by_module mid with Not_found -> [] in
      Hashtbl.replace by_module mid (syms :: prev))
    per_file_syms;
  let module_syms : (string, symbols) Hashtbl.t = Hashtbl.create 16 in
  Hashtbl.iter (fun mid syms_list -> Hashtbl.replace module_syms mid (merge_syms_for_module syms_list)) by_module;
  module_syms

(* The one entry point the driver (bin/main.ml) calls. `module_of` maps
   a discovered file to its module id (its directory, relative to
   whatever root path was discovered from — the driver's own concern,
   computed once from the same root/rel machinery Task 8's discover_dir
   already has; "." denotes the root module, matching Filename.dirname's
   own convention for a bare name with no directory part).
   `per_file_syms` is exactly what typecheck_all already builds (each
   file's OWN, unmerged collect_declarations output) — grouping it by
   module and merging within each group is `module_symbols`, above.
   `per_file_progs` supplies the bodies to walk. *)
let check_modules (collector : Diag.Collector.t) ~(module_of : string -> string)
    (per_file_syms : (string * symbols) list) (per_file_progs : (string * program) list) : unit =
  let module_syms = module_symbols ~module_of per_file_syms in
  let known_modules = Hashtbl.fold (fun k _ acc -> k :: acc) module_syms [] in
  List.iter
    (fun (file, prog) ->
      let mid = module_of file in
      let own = try Hashtbl.find module_syms mid with Not_found -> merge_syms_for_module [] in
      let uses = uses_of_program prog in
      (* Defined before the unknown-module check below (not just before
         check_module_refs) so that check can mark_used its own
         offending edge — an unknown-module `use` is already reported
         once, precisely; a second, redundant "unused `use`" on the same
         line would only be noise, not new information. *)
      let used_aliases : (string, unit) Hashtbl.t = Hashtbl.create 8 in
      let mark_used alias = Hashtbl.replace used_aliases alias () in
      List.iter
        (fun (u : use_edge) ->
          if (not u.ue_is_stdlib) && not (List.mem (path_str u.ue_segments) known_modules) then begin
            mark_used u.ue_alias;
            Diag.Collector.add collector
              (Diag.error ~code:unknown_module_code ~file ~line:u.ue_pos.line ~col:u.ue_pos.col
                 ~message:
                   (Printf.sprintf
                      "unknown module `%s` — not a discovered project module and not a reserved \
                       stdlib namespace"
                      (path_str u.ue_segments))
                 ())
          end)
        uses;
      let uses_resolved =
        List.filter_map
          (fun (u : use_edge) ->
            if u.ue_is_stdlib then None
            else
              match Hashtbl.find_opt module_syms (path_str u.ue_segments) with
              | Some s -> Some (u, s)
              | None -> None)
          uses
      in
      let stdlib_aliases = List.filter_map (fun u -> if u.ue_is_stdlib then Some u.ue_alias else None) uses in
      let used_ids = List.map (fun (u, _) -> path_str u.ue_segments) uses_resolved in
      let others =
        Hashtbl.fold
          (fun m s acc -> if m = mid || List.mem m used_ids then acc else (m, s) :: acc)
          module_syms []
      in
      check_module_refs collector ~file ~own ~uses_resolved ~stdlib_aliases ~others ~mark_used prog;
      List.iter
        (fun (u : use_edge) ->
          if not (Hashtbl.mem used_aliases u.ue_alias) then
            Diag.Collector.add collector
              (Diag.warning ~code:unused_use_code ~file ~line:u.ue_pos.line ~col:u.ue_pos.col
                 ~message:(Printf.sprintf "unused `use %s`" (path_str u.ue_segments)) ()))
        uses)
    per_file_progs

(* ============================================================
   Dump support
   ============================================================ *)

let pos_str (p : pos) : string = Printf.sprintf "%d:%d" p.line p.col

let rec field_ty_str (ft : field_ty) : string =
  match ft with
  | Scalar s -> s
  | Ref s -> "ref " ^ s
  | Multi s -> "multi " ^ s
  | Map (k, v) -> "map<" ^ k ^ ", " ^ v ^ ">"
  | Nullable t -> "?" ^ field_ty_str t

let dump_symbols (syms : symbols) : string =
  let class_lines = StringMap.fold (fun _name (cls : class_info) acc ->
    let fields_str = List.map (fun (fname, fty, _fdefault, _fann) ->
      Printf.sprintf "    %s FIELD %s: %s" (pos_str cls.pos) fname (field_ty_str fty)
    ) cls.fields in
    let methods_str = List.map (fun (m : method_info) ->
      Printf.sprintf "    %s METHOD %s" (pos_str m.pos) m.name
    ) cls.methods in
    (Printf.sprintf "%s CLASS %s @gc=%b" (pos_str cls.pos) cls.name cls.is_gc)
    :: fields_str @ methods_str @ acc
  ) syms.classes [] in

  let interface_lines = StringMap.fold (fun _name (iface : interface_info) acc ->
    let methods_str = List.map (fun (m : method_sig_info) ->
      Printf.sprintf "    %s METHOD %s" (pos_str m.pos) m.name
    ) iface.methods in
    (Printf.sprintf "%s INTERFACE %s" (pos_str iface.pos) iface.name)
    :: methods_str @ acc
  ) syms.interfaces [] in

  let fn_lines = StringMap.fold (fun _name (fn : free_fn_info) acc ->
    (Printf.sprintf "%s FN %s" (pos_str fn.pos) fn.name) :: acc
  ) syms.free_fns [] in

  String.concat "\n" (class_lines @ interface_lines @ fn_lines)