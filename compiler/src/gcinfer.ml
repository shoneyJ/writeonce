(* gcinfer.ml — inferred GC classification (spec 2026-08-11; plan Phase 1,
   structural half).

   Build the class-reference graph and mark every class in a non-trivial SCC,
   or with a self-loop, as traced (`gc`); everything else is `owned`. `ref B`
   is a row id (Copy) and `backlink` is a virtual inverse (recomputed by index
   scan) — neither stores a pointer, so neither contributes a graph edge, and
   neither can force a cycle.

   Additive: this pass does not yet feed field-kind derivation (that is plan
   Phase 2, at the `Types.is_gc_class` seam), so nothing it decides changes
   emitted bytecode. It only backs the `woc --dump-gc` artifact today. *)

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
