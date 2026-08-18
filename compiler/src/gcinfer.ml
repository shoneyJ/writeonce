(* gcinfer.ml — inferred GC classification (spec 2026-08-11).

   `infer` is the pass: it returns `syms` with `traced` populated from two
   halves —
     - STRUCTURAL: the class-reference graph + Tarjan SCC. A class in a
       non-trivial SCC or with a self-loop is traced. `ref B` is a row id (Copy)
       and `backlink` is a virtual inverse — neither stores a pointer, so
       neither contributes an edge nor can force a cycle.
     - DEMAND: ownership run in collect mode; a class whose value must escape
       (a shape only a traced class can hold) is promoted.

   Every consumer (the driver's typecheck_all, the unit-test helpers) calls
   `infer`, so `Types.is_gc_class` — which field-kind derivation, owner
   exemptions, and the class flag all key off — answers identically everywhere.

   KNOWN LIMITATION (to refine with the Phase-3 landing): the demand hook
   promotes the escaping *root local's* class, so `return h.box` over-promotes
   `Holder` as well as `Box`. It should promote the escaping projection's type
   only. Sound (never under-promotes) but imprecise. *)

module SMap = Types.StringMap

(* The user-class names one field type points at — the edges out of the class
   that declares the field. Builtin scalars (Int/Bool/Text) and non-class names
   resolve to no edge. *)
let rec refs_of_ty (classes : Types.class_info SMap.t) (t : Ast.field_ty) :
    string list =
  match t with
  | Ast.Scalar n | Ast.Multi n -> if SMap.mem n classes then [ n ] else []
  | Ast.Map (k, v) -> List.filter (fun n -> SMap.mem n classes) [ k; v ]
  | Ast.Nullable ft -> refs_of_ty classes ft
  | Ast.Ref _ | Ast.Backlink _ -> [] (* id / virtual inverse: no pointer edge *)

let edges (classes : Types.class_info SMap.t) (ci : Types.class_info) :
    string list =
  List.concat_map (fun (_, ft, _, _) -> refs_of_ty classes ft) ci.Types.fields
  |> List.sort_uniq compare

(* Tarjan's strongly-connected components over the class-name graph. Recursive;
   class graphs are tiny, so recursion depth is a non-issue. *)
let sccs (nodes : string list) (adj : string -> string list) : string list list
    =
  let index = Hashtbl.create 64 and low = Hashtbl.create 64 in
  let onstack = Hashtbl.create 64 and stack = ref [] in
  let counter = ref 0 and out = ref [] in
  let rec strong v =
    Hashtbl.replace index v !counter;
    Hashtbl.replace low v !counter;
    incr counter;
    stack := v :: !stack;
    Hashtbl.replace onstack v true;
    List.iter
      (fun w ->
        if not (Hashtbl.mem index w) then begin
          strong w;
          Hashtbl.replace low v (min (Hashtbl.find low v) (Hashtbl.find low w))
        end
        else if Hashtbl.mem onstack w && Hashtbl.find onstack w then
          Hashtbl.replace low v (min (Hashtbl.find low v) (Hashtbl.find index w)))
      (adj v);
    if Hashtbl.find low v = Hashtbl.find index v then begin
      let comp = ref [] and stop = ref false in
      while not !stop do
        match !stack with
        | [] -> stop := true
        | w :: rest ->
          stack := rest;
          Hashtbl.replace onstack w false;
          comp := w :: !comp;
          if w = v then stop := true
      done;
      out := !comp :: !out
    end
  in
  List.iter (fun v -> if not (Hashtbl.mem index v) then strong v) nodes;
  !out

type result = {
  traced : string SMap.t;
      (* traced class name -> human reason (for --dump-gc and, in Phase 2, the
         promotion note) *)
  order : string list; (* all class names, sorted — deterministic dump order *)
}

(* the traced class names as a set, for injection into `Types.symbols.traced`
   (what `Types.is_gc_class` consults). *)
let traced_names (r : result) : Types.StringSet.t =
  SMap.fold (fun k _ acc -> Types.StringSet.add k acc) r.traced Types.StringSet.empty

let classify (syms : Types.symbols) : result =
  let classes = syms.Types.classes in
  let nodes =
    SMap.fold (fun k _ acc -> k :: acc) classes [] |> List.sort compare
  in
  let adj v =
    match SMap.find_opt v classes with Some ci -> edges classes ci | None -> []
  in
  let comps = sccs nodes adj in
  let traced =
    List.fold_left
      (fun acc comp ->
        match comp with
        | [ v ] ->
          (* a singleton SCC is traced only if it points at itself *)
          if List.mem v (adj v) then
            SMap.add v (Printf.sprintf "cycle %s -> %s" v v) acc
          else acc
        | members ->
          let ms = List.sort compare members in
          let path = String.concat " -> " ms ^ " -> " ^ List.hd ms in
          List.fold_left
            (fun a m -> SMap.add m (Printf.sprintf "cycle %s" path) a)
            acc ms)
      SMap.empty comps
  in
  { traced; order = nodes }

(* The `--dump-gc` artifact (spec §1): one line per class in sorted order,
   reading the AUTHORITATIVE traced set on `syms` (structural SCC + demand
   promotions injected by the pipeline). Reason = the structural cycle path
   when there is one, else a demand-promotion note. *)
let render_final (syms : Types.symbols) : string =
  let struct_reasons = (classify syms).traced in
  let names =
    SMap.fold (fun k _ acc -> k :: acc) syms.Types.classes [] |> List.sort compare
  in
  let buf = Buffer.create 256 in
  List.iter
    (fun name ->
      if Types.is_gc_class syms name then
        let reason =
          match SMap.find_opt name struct_reasons with
          | Some r -> r
          | None ->
            if Types.StringSet.mem name syms.Types.traced then "alias escape (demand)"
            else "@gc annotation (redundant — inference covers it)"
        in
        Buffer.add_string buf (Printf.sprintf "%-10s gc     (%s)\n" name reason)
      else Buffer.add_string buf (Printf.sprintf "%-10s owned\n" name))
    names;
  Buffer.contents buf

(* Structural-only render (pre-injection); kept for unit tests of the SCC half. *)
let render (r : result) : string =
  let buf = Buffer.create 256 in
  List.iter
    (fun name ->
      match SMap.find_opt name r.traced with
      | Some reason ->
        Buffer.add_string buf (Printf.sprintf "%-10s gc     (%s)\n" name reason)
      | None -> Buffer.add_string buf (Printf.sprintf "%-10s owned\n" name))
    r.order;
  Buffer.contents buf

(* The full inference pass: structural SCC (cycles) unioned with demand
   promotion (a class value that must escape). Returns `syms` with `traced`
   populated — the single entry point every consumer (the driver AND the unit
   tests) calls, so `is_gc_class` answers identically everywhere. The demand
   half runs ownership in collect mode over every program to a fixpoint;
   promotions only grow (bounded by class count), so it terminates. *)
let infer (parsed : (string * Ast.program) list) (syms : Types.symbols) :
    Types.symbols =
  let structural = traced_names (classify syms) in
  let throwaway = Diag.Collector.create () in
  let traced = ref structural in
  let changed = ref true in
  while !changed do
    let promoted = Hashtbl.create 16 in
    let syms_c = { syms with Types.traced = !traced } in
    List.iter
      (fun (f, prog) ->
        ignore
          (Owner.analyze ~file:f
             ~promote:(Some (fun c -> Hashtbl.replace promoted c ()))
             prog syms_c throwaway))
      parsed;
    let next =
      Hashtbl.fold (fun c () acc -> Types.StringSet.add c acc) promoted !traced
    in
    changed := not (Types.StringSet.equal next !traced);
    traced := next
  done;
  { syms with Types.traced = !traced }
