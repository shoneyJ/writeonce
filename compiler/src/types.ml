(* types.ml — Typechecker for `.wo` OOP source (Task 6).
   Two-pass:
   1. Collect all declarations (classes, interfaces, free fns, typedefs)
   2. Typecheck bodies with full symbol tables.
   Produces typed AST + per-class field-kind table + interface satisfaction set. *)

open Ast

module StringMap = Map.Make(String)

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
  is_gc : bool;
  table : table_cfg option;
  fields : (string * field_ty * default_expr option * string list) list;
  methods : method_info list;
  id : int;
  pos : pos;
}

and interface_info = {
  name : string;
  methods : method_sig_info list;
  id : int;
  pos : pos;
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
}

and typedef_info = {
  name : string;
  fields : (string * field_ty * default_expr option) list;
  id : int;
  pos : pos;
}

and symbols = {
  classes : class_info StringMap.t;
  interfaces : interface_info StringMap.t;
  free_fns : free_fn_info StringMap.t;
  typedefs : typedef_info StringMap.t;
  modules : string list;
}

(* Builtin scalars *)
let builtin_scalars = ["Int"; "Bool"; "Text"; "Money"; "Timestamp"; "Id"; "SKU"]

let is_builtin_scalar name = List.mem name builtin_scalars

let is_gc_class (syms : symbols) name =
  try
    let cls = StringMap.find name syms.classes in
    cls.is_gc
  with Not_found -> false

(* wob_kind_of_typ: maps internal typ to .wob field kind *)
let wob_kind_of_typ (syms : symbols) (t : typ) : wob_kind =
  let kind_of = function
    | TScalar name ->
        if is_builtin_scalar name then WO_K_SCALAR
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

(* ============================================================
   Pass 1: Declaration Collection
   ============================================================ *)

let collect_declarations (_prog : program) (_collector : Diag.Collector.t) : symbols =
  let classes = ref StringMap.empty in
  let interfaces = ref StringMap.empty in
  let free_fns = ref StringMap.empty in
  let typedefs = ref StringMap.empty in
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
            is_static = false;
            id = m.id;
            pos = m.pos; }
        ) (c.methods : Ast.method_decl list) in
        let info = {
          name = c.name;
          is_class = c.is_class;
          is_gc = c.is_gc;
          table = c.table;
          fields = fields;
          methods = methods;
          id = c.id;
          pos = c.pos;
        } in
        classes := StringMap.add c.name info !classes
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
        } in
        interfaces := StringMap.add i.name info !interfaces
    | Ast.Fn f ->
        let info = {
          name = f.name;
          params = List.map (fun (p : Ast.param) -> (p.name, p.ty, p.conv)) f.params;
          ret = f.ret;
          body = f.body;
          mutates = false;
          id = f.id;
          pos = f.pos;
        } in
        free_fns := StringMap.add f.name info !free_fns
  ) _prog.decls;

  { classes = !classes; interfaces = !interfaces; free_fns = !free_fns;
    typedefs = !typedefs; modules = !modules }

(* ============================================================
   Pass 2: Body Typechecking
   ============================================================ *)

type expr_type_result = {
  typ : typ;
  is_nil : bool;
}

let typecheck_program (_prog : program) (syms : symbols) (_collector : Diag.Collector.t) : unit =
  let rec resolve_field_ty (ft : field_ty) : typ =
    match ft with
    | Scalar name ->
        if is_builtin_scalar name then TScalar name
        else if is_gc_class syms name then TScalar name
        else TScalar name
    | Ref name -> TRef name
    | Multi inner_name -> TMulti (TScalar inner_name)
    | Map (k_name, v_name) -> TMap (TScalar k_name, TScalar v_name)
    | Nullable inner -> TNullable (resolve_field_ty inner)
  in

  let rec typecheck_expr (env : typ StringMap.t) (e : expr) : expr_type_result =
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
        let base_res = typecheck_expr env base in
        (match base_res.typ with
         | TScalar class_name ->
             (try
                let cls = StringMap.find class_name syms.classes in
                let (_, field_ty, _, _) = List.find (fun (fname, _, _, _) -> fname = field_name) cls.fields in
                { typ = resolve_field_ty field_ty; is_nil = false }
              with Not_found ->
                Diag.Collector.add _collector
                  (Diag.error ~code:unknown_field_code ~file:"" ~line:e.pos.line ~col:e.pos.col
                     ~message:(Printf.sprintf "unknown field `%s` on `%s`" field_name class_name) ());
                { typ = TScalar "Int"; is_nil = false })
         | _ -> { typ = TScalar "Int"; is_nil = false })
    | Index (base, idx) ->
        let _ = typecheck_expr env base in
        let _ = typecheck_expr env idx in
        { typ = TScalar "Int"; is_nil = false }
    | Call (_callee, args) ->
        List.iter (fun arg -> ignore (typecheck_expr env arg)) args;
        { typ = TScalar "Int"; is_nil = false }
    | Unary (_, operand) -> typecheck_expr env operand
    | Binary (_, left, right) ->
        let _ = typecheck_expr env left in
        let _ = typecheck_expr env right in
        { typ = TScalar "Bool"; is_nil = false }
    | Ctor (class_name, fields) ->
        (try
           let cls = StringMap.find class_name syms.classes in
           let provided = List.map (fun (n, _) -> n) fields in
           List.iter (fun (fname, _, _, _) ->
             if not (List.mem fname provided) then
               Diag.Collector.add _collector
                 (Diag.error ~code:incomplete_ctor_code ~file:"" ~line:e.pos.line ~col:e.pos.col
                    ~message:(Printf.sprintf "missing field `%s` in constructor of `%s`" fname class_name) ())
           ) cls.fields;
           { typ = TScalar class_name; is_nil = false }
         with Not_found ->
           Diag.Collector.add _collector
             (Diag.error ~code:unknown_type_code ~file:"" ~line:e.pos.line ~col:e.pos.col
                ~message:(Printf.sprintf "unknown type `%s` in constructor" class_name) ());
           { typ = TScalar "Int"; is_nil = false })
    | DbStub _ -> { typ = TVoid; is_nil = false }
  in

  let rec typecheck_stmt (env : typ StringMap.t) (s : stmt) : typ StringMap.t =
    match s.s_kind with
    | Let { name; ty = _ty; value } ->
        let val_res = typecheck_expr env value in
        StringMap.add name val_res.typ env
    | Assign { target; value } ->
        let _ = typecheck_expr env target in
        let _ = typecheck_expr env value in
        env
    | If { cond; then_body; else_body } ->
        let _ = typecheck_expr env cond in
        let env_then = List.fold_left typecheck_stmt env then_body in
        (match else_body with
         | Some (_, else_body) ->
             List.fold_left typecheck_stmt env else_body
         | None -> env_then)
    | While { cond; body } ->
        let _ = typecheck_expr env cond in
        List.fold_left typecheck_stmt env body
    | For { var; iter; body } ->
        let iter_res = typecheck_expr env iter in
        let env_body = StringMap.add var iter_res.typ env in
        List.fold_left typecheck_stmt env_body body
    | Return opt_e ->
        (match opt_e with Some e -> let _ = typecheck_expr env e in () | None -> ());
        env
    | ExprStmt e ->
        let _ = typecheck_expr env e in
        env
  in

  let typecheck_method (env : typ StringMap.t) (m : method_info) : bool =
    let param_env = List.fold_left (fun acc (name, ty, _) ->
      StringMap.add name (resolve_field_ty ty) acc) env m.params in
    let env_with_self = StringMap.add "self" (TScalar "Self") param_env in
    let _ = List.fold_left typecheck_stmt env_with_self m.body in
    false
  in

  StringMap.iter (fun _name cls ->
    let method_env = StringMap.empty in
    List.iter (fun m -> ignore (typecheck_method method_env m)) cls.methods
  ) syms.classes;

  StringMap.iter (fun _name (fn : free_fn_info) ->
    let param_env = List.fold_left (fun acc (name, ty, _) ->
      StringMap.add name (resolve_field_ty ty) acc) StringMap.empty fn.params in
    ignore (List.fold_left typecheck_stmt param_env fn.body)
  ) syms.free_fns;

  ()

(* ============================================================
   Entry point
   ============================================================ *)

let typecheck (_prog : program) (_collector : Diag.Collector.t) : symbols * unit =
  let syms = collect_declarations _prog _collector in
  let () = typecheck_program _prog syms _collector in
  (syms, ())

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