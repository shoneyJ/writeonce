(* woc — the writeonce OCaml compiler front end.

   Task 1 scaffolded the CLI's exit-code contract with no compiler
   stage behind it. Task 2 added diagnostics (compiler/src/diag.ml).
   Task 3 added the lexer (compiler/src/{token,lexer}.ml) behind
   --dump-tokens. Task 4 adds the declaration parser
   (compiler/src/{ast,parser}.ml) behind --dump-ast, printing a stable,
   golden-diffed AST dump (compiler/src/dump.ml) to stdout. Tasks 5-7
   add statement/expression parsing, the typechecker, and the ownership
   pass behind their own --dump-* flags. Task 8 adds directory
   discovery and multi-file programs: <path> may now be a single .wo
   file or a directory, recursively discovered the same way `wo run`
   discovers a project (compiler/bin/main.ml's discover_dir mirrors
   crates/rt/src/lib.rs::discover — skips dot-prefixed entries and
   target/data/node_modules, keeps .wo files, sorted by path relative
   to the root). Declarations are collected across every discovered
   file before any file's bodies are checked, so symbols span files;
   every diagnostic from every stage lands in one Diag.Collector, whose
   (file, line, col) sort (diag.ml, Task 2) is what actually gives the
   final ordering — not the discovery order files happen to be visited
   in. The bare `woc <path>` form now runs the full pipeline (lex,
   parse, typecheck, ownership-check) instead of only checking that the
   path exists.

     0 = clean compile
     1 = diagnostics reported
     2 = usage or IO failure

   With no arguments, this prints usage to stderr and exits 2.

   ---- Multi-file dump layout ----

   For a single discovered file, every --dump-* flag's stdout output is
   byte-identical to before Task 8 (no header, nothing changed). When a
   path resolves to more than one file, each file's dump is preceded by
   a Woc_lib.Dump.file_header line naming that file, and files appear
   in the same sorted discovery order used everywhere else — see
   compiler/src/dump.ml's doc comment on file_header for the exact
   format. *)

let usage_msg =
  "usage: woc <path>\n\
   usage: woc --dump-tokens <path>\n\
   usage: woc --dump-ast <path>\n\
   usage: woc --dump-owner <path>\n\
   \n\
   Compiles writeonce (.wo) source. <path> is a single .wo file or a\n\
   directory: a directory is discovered recursively for every .wo file\n\
   under it (dot-prefixed entries and target/data/node_modules are\n\
   skipped, same as `wo run`), sorted by path so discovery order is\n\
   deterministic. Multiple discovered files compile as one program —\n\
   declarations in one file are visible to bodies in another.\n\
   \n\
   With no flag, <path> is fully compiled (lexed, parsed, typechecked,\n\
   ownership-checked) and nothing is printed on success; diagnostics,\n\
   if any, print to stderr.\n\
   \n\
   --dump-tokens prints one line per lexed token to stdout, in source\n\
   order (\"LINE:COL KIND\" or \"LINE:COL KIND(payload)\"), ending with\n\
   EOF; lexing diagnostics, if any, print to stderr.\n\
   \n\
   --dump-ast prints the declaration- and body-level AST as an indented\n\
   tree to stdout (class/type/interface/fn declarations, with real\n\
   statement/expression parsing inside method and free-fn bodies);\n\
   parsing diagnostics, if any, print to stderr.\n\
   \n\
   --dump-owner runs the lexer, parser, typechecker and ownership pass,\n\
   then prints the ownership pass's four emitter tables to stdout in\n\
   source order (moves, scope-end drops, @gc rc sites, residual borrow\n\
   sites — see compiler/src/dump.ml for the format); lexing, parsing,\n\
   type and ownership (WO-E3xx) diagnostics print to stderr.\n\
   \n\
   For a directory (or otherwise multi-file) path, every --dump-* flag\n\
   prints each file's own dump in turn, separated by a header line — see\n\
   compiler/src/dump.ml's file_header doc comment.\n\
   \n\
   Exit codes: 0 clean, 1 diagnostics reported, 2 usage/IO failure.\n"

let read_source path =
  try
    let ic = open_in_bin path in
    let n = in_channel_length ic in
    let s = really_input_string ic n in
    close_in ic;
    Ok s
  with Sys_error msg -> Error msg

(* ---- File discovery (Task 8) ----------------------------------------

   Mirrors wo run's discovery contract (crates/rt/src/lib.rs::discover)
   exactly: recursive, skips any entry (file or directory) whose name
   starts with '.' or is literally "target", "data", or "node_modules",
   keeps only ".wo"-suffixed files, and returns them sorted by path
   relative to the root directory (so nested directories don't disturb
   the order a flat listing would give). A single file argument is
   returned as the one-element list [path], no filtering applied — that
   matches the bare-path form's pre-Task-8 behavior of accepting
   whatever file it's given. *)

let skip_name (name : string) : bool =
  (String.length name > 0 && name.[0] = '.')
  || name = "target" || name = "data" || name = "node_modules"

let discover_dir (root : string) : string list =
  let rec walk (dir : string) (rel : string) : (string * string) list =
    Sys.readdir dir |> Array.to_list
    |> List.concat_map (fun name ->
      if skip_name name then []
      else
        let full = Filename.concat dir name in
        let rel' = if rel = "" then name else Filename.concat rel name in
        if Sys.is_directory full then walk full rel'
        else if Filename.check_suffix name ".wo" then [ (rel', full) ]
        else [])
  in
  walk root "" |> List.sort (fun (a, _) (b, _) -> compare (a : string) b) |> List.map snd

let discover_files (path : string) : (string list, string) result =
  if not (Sys.file_exists path) then
    Error (Printf.sprintf "no such file or directory: %s" path)
  else if Sys.is_directory path then Ok (discover_dir path)
  else Ok [ path ]

let discover_and_read (path : string) : (string * string) list =
  match discover_files path with
  | Error msg ->
    Printf.eprintf "woc: %s\n" msg;
    exit 2
  | Ok files ->
    List.map
      (fun f ->
        match read_source f with
        | Ok src -> (f, src)
        | Error msg ->
          Printf.eprintf "woc: %s\n" msg;
          exit 2)
      files

let build_lookup (sources : (string * string) list) : Woc_lib.Diag.source_lookup =
  let tbl = Hashtbl.create (List.length sources) in
  List.iter (fun (f, src) -> Hashtbl.replace tbl f src) sources;
  fun f -> Hashtbl.find_opt tbl f

let finish (collector : Woc_lib.Diag.Collector.t) (lookup : Woc_lib.Diag.source_lookup) : unit =
  if Woc_lib.Diag.Collector.has_error collector then begin
    prerr_string (Woc_lib.Diag.Collector.render_all collector lookup);
    prerr_newline ();
    exit 1
  end
  else exit 0

(* ---- Cross-file symbol resolution (Task 8) ---------------------------

   Types.collect_declarations / Types.typecheck_program are already
   split into a declare-pass and a check-pass (Task 6); that split is
   exactly what multi-file needs, so the driver spans files by calling
   each pass once per file and merging the declare-pass output before
   any file's check-pass runs — types.ml itself needs no change. Each
   file's own collect_declarations call still gets that file's own
   `~file`, so its own diagnostics (e.g. WO-W201) tag the right file;
   only the merged `symbols` value, not any single file's, is what
   check-pass calls see, so a class declared in one file resolves for a
   field/constructor/etc. in another regardless of discovery order. *)

let parse_all (collector : Woc_lib.Diag.Collector.t) (sources : (string * string) list) :
    (string * Woc_lib.Ast.program) list =
  List.map
    (fun (f, src) ->
      let toks = Woc_lib.Lexer.tokenize collector ~file:f src in
      let prog = Woc_lib.Parser.parse collector ~file:f toks in
      (f, prog))
    sources

let merge_symbols (syms_list : Woc_lib.Types.symbols list) : Woc_lib.Types.symbols =
  let module SM = Woc_lib.Types.StringMap in
  let keep_first _key a _b = Some a in
  List.fold_left
    (fun (acc : Woc_lib.Types.symbols) (s : Woc_lib.Types.symbols) ->
      Woc_lib.Types.{
        classes = SM.union keep_first acc.classes s.classes;
        interfaces = SM.union keep_first acc.interfaces s.interfaces;
        free_fns = SM.union keep_first acc.free_fns s.free_fns;
        typedefs = SM.union keep_first acc.typedefs s.typedefs;
        modules = acc.modules @ s.modules;
      })
    Woc_lib.Types.{
      classes = SM.empty; interfaces = SM.empty; free_fns = SM.empty;
      typedefs = SM.empty; modules = [];
    }
    syms_list

(* ---- Cross-file symbol collision (review follow-up, Important 2) -----

   merge_symbols's first-wins StringMap.union was silent: a same-named
   class/interface declared again in a later file doesn't disappear —
   it's just dropped from the merged table, so that file's own methods
   still get typechecked, but against the *winning* file's field list.
   That is a real wrong-shape bug (spurious unknown-field/missing-field
   on otherwise-correct code, or a silent pass against the wrong
   shape), not merely an untested edge. Reported once per collision, at
   merge time, before the losing declaration's own position is gone —
   the first-wins merge behavior itself is unchanged; only the silence
   is fixed. *)

let duplicate_symbol_code = Woc_lib.Diag.types_prefix ^ "14" (* WO-E214 *)

let report_collision (collector : Woc_lib.Diag.Collector.t) ~(kind : string) ~(name : string)
    ~(file : string) ~(pos : Woc_lib.Ast.pos) ~(first_file : string)
    ~(first_pos : Woc_lib.Ast.pos) : unit =
  Woc_lib.Ast.(
    Woc_lib.Diag.Collector.add collector
      (Woc_lib.Diag.error ~code:duplicate_symbol_code ~file ~line:pos.line ~col:pos.col
         ~message:(Printf.sprintf "%s `%s` already declared in `%s`" kind name first_file)
         ~related:
           [ Woc_lib.Diag.related_site ~file:first_file ~line:first_pos.line ~col:first_pos.col
               ~label:(Printf.sprintf "`%s` first declared here" name)
           ]
         ()))

(* Walks (file, symbols) pairs in discovery order, per kind (class,
   interface), remembering the first file/pos to declare each name and
   reporting every later redeclaration against it. free_fns/typedefs
   aren't checked: nothing downstream resolves them by cross-file
   lookup the way class/interface satisfaction does, so a same-name
   free fn isn't the silent-wrong-shape hazard this exists for — out of
   scope for this fix, not something the review asked for. *)
let check_symbol_collisions (collector : Woc_lib.Diag.Collector.t)
    (per_file : (string * Woc_lib.Types.symbols) list) : unit =
  let seen_classes : (string, string * Woc_lib.Ast.pos) Hashtbl.t = Hashtbl.create 16 in
  let seen_interfaces : (string, string * Woc_lib.Ast.pos) Hashtbl.t = Hashtbl.create 16 in
  List.iter
    (fun (file, (syms : Woc_lib.Types.symbols)) ->
      Woc_lib.Types.(
        StringMap.iter
          (fun name (c : class_info) ->
            match Hashtbl.find_opt seen_classes name with
            | Some (first_file, first_pos) ->
              report_collision collector ~kind:"class" ~name ~file ~pos:c.pos ~first_file ~first_pos
            | None -> Hashtbl.add seen_classes name (file, c.pos))
          syms.classes;
        StringMap.iter
          (fun name (i : interface_info) ->
            match Hashtbl.find_opt seen_interfaces name with
            | Some (first_file, first_pos) ->
              report_collision collector ~kind:"interface" ~name ~file ~pos:i.pos ~first_file
                ~first_pos
            | None -> Hashtbl.add seen_interfaces name (file, i.pos))
          syms.interfaces))
    per_file

let typecheck_all (collector : Woc_lib.Diag.Collector.t)
    (parsed : (string * Woc_lib.Ast.program) list) : Woc_lib.Types.symbols =
  let per_file_syms =
    List.map
      (fun (f, prog) -> (f, Woc_lib.Types.collect_declarations ~file:f prog collector))
      parsed
  in
  check_symbol_collisions collector per_file_syms;
  let syms = merge_symbols (List.map snd per_file_syms) in
  List.iter
    (fun (f, prog) -> Woc_lib.Types.typecheck_program ~file:f prog syms collector)
    parsed;
  syms

let dump_tokens path =
  let sources = discover_and_read path in
  let collector = Woc_lib.Diag.Collector.create () in
  let multi = List.length sources > 1 in
  List.iter
    (fun (f, src) ->
      let toks = Woc_lib.Lexer.tokenize collector ~file:f src in
      if multi then print_string (Woc_lib.Dump.file_header f);
      print_string (Woc_lib.Dump.dump_tokens toks))
    sources;
  finish collector (build_lookup sources)

let dump_ast path =
  let sources = discover_and_read path in
  let collector = Woc_lib.Diag.Collector.create () in
  let multi = List.length sources > 1 in
  let parsed = parse_all collector sources in
  List.iter
    (fun (f, prog) ->
      if multi then print_string (Woc_lib.Dump.file_header f);
      print_string (Woc_lib.Dump.dump_ast prog))
    parsed;
  finish collector (build_lookup sources)

let dump_owner path =
  let sources = discover_and_read path in
  let collector = Woc_lib.Diag.Collector.create () in
  let multi = List.length sources > 1 in
  let parsed = parse_all collector sources in
  let syms = typecheck_all collector parsed in
  List.iter
    (fun (f, prog) ->
      let tables = Woc_lib.Owner.analyze ~file:f prog syms collector in
      if multi then print_string (Woc_lib.Dump.file_header f);
      print_string (Woc_lib.Dump.dump_owner tables))
    parsed;
  finish collector (build_lookup sources)

(* The bare `woc <path>` form (Task 8): runs the full pipeline with no
   dump — check-only. Nothing is printed to stdout on success, matching
   the exit-code contract's "0 = clean compile". *)
let check_only path =
  let sources = discover_and_read path in
  let collector = Woc_lib.Diag.Collector.create () in
  let parsed = parse_all collector sources in
  let syms = typecheck_all collector parsed in
  List.iter (fun (f, prog) -> ignore (Woc_lib.Owner.analyze ~file:f prog syms collector)) parsed;
  finish collector (build_lookup sources)

let () =
  match Sys.argv with
  | [| _; "--dump-tokens"; path |] -> dump_tokens path
  | [| _; "--dump-ast"; path |] -> dump_ast path
  | [| _; "--dump-owner"; path |] -> dump_owner path
  | [| _; path |] -> check_only path
  | _ ->
    prerr_string usage_msg;
    exit 2
