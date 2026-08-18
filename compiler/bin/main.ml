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
   usage: woc <dir>              (builds when <dir>/wo.toml exists)\n\
   usage: woc --emit <path> -o <out.wob>\n\
   usage: woc build <dir> -o <app> [--runtime <path>]\n\
   usage: woc version\n\
   usage: woc --dump-tokens <path>\n\
   usage: woc --dump-ast <path>\n\
   usage: woc --dump-owner <path>\n\
   usage: woc --dump-gc <path>\n\
   usage: woc --dump-bc <path>\n\
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
   if any, print to stderr. One exception: a directory containing a\n\
   wo.toml manifest is BUILT instead — `woc .` inside a project (or\n\
   `woc path/to/project` from anywhere) reads the manifest's `name` plus\n\
   the optional [build] runtime/target keys (paths relative to the\n\
   manifest) and produces <target>/<name> exactly as `woc build` would.\n\
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
   --emit runs the whole pipeline and writes the `.wob` v1 image named\n\
   by -o (docs/plan/oop-vm/00-wob-format.md). Every discovered file\n\
   contributes to one image; the entry point is the zero-argument free\n\
   fn `main`, if the program declares one. Nothing is written when any\n\
   diagnostic is an error — bytecode for a program that does not compile\n\
   is never produced.\n\
   \n\
   --dump-bc emits the same image and prints its disassembly to stdout\n\
   (compiler/src/disasm.ml). Unlike the other dumps it prints nothing\n\
   when the compile is not clean: a disassembly of a program that failed\n\
   to compile would be describing bytecode nobody may run.\n\
   \n\
   build compiles <dir> like --emit, then produces one self-contained\n\
   executable at -o: the wovm runtime binary (--runtime <path>; else\n\
   $WO_RUNTIME, a `wovm` beside this `woc`, or runtime/wovm relative to\n\
   the current directory) with the\n\
   compiled .wob image and a fixed-size trailer appended, so the result\n\
   runs standalone with no separate .wob file or argument (wovm finds the\n\
   embedded image via /proc/self/exe -- see docs/plan/oop-vm/00-wob-format.md's\n\
   \"single-binary trailer\" section). Nothing is written when the compile\n\
   has diagnostics, when the program declares no zero-argument free fn\n\
   named `main`, or when the runtime binary cannot be found.\n\
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

(* Pre-existing defect, fixed here (haxe-parity Task 1, modules): this
   used to gate printing on `has_error` alone, so a collector holding
   *only* warnings (no error at all — e.g. WO-W201's gc-suggestion, or
   this task's own WO-W202 unused-`use`) printed nothing and exited 0,
   indistinguishable from a collector with zero diagnostics. A warning
   nobody ever sees is a dead feature, not a working one — this task's
   own unused-`use` warning needs to actually reach stderr to be worth
   having, which is what surfaced this. Still exits 0 whenever nothing
   is an error (unchanged contract); the only behavior change is that a
   warning-only run now also prints, matching what "diagnostics, if
   any, print to stderr" (this file's own usage_msg) already promised. *)
let finish (collector : Woc_lib.Diag.Collector.t) (lookup : Woc_lib.Diag.source_lookup) : unit =
  let text = Woc_lib.Diag.Collector.render_all collector lookup in
  if text <> "" then begin
    prerr_string text;
    prerr_newline ()
  end;
  exit (Woc_lib.Diag.Collector.exit_code collector)

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

(* Parses every discovered file, then makes one more constant-substitution
   pass so a `const` declared in one file reaches its siblings: files in a
   directory are one module and unconditionally visible to each other
   (haxe-parity Task 1), and the workload relies on it (logtail.wo's
   `const CHUNK` is read from mcp.wo). Parser.parse already substituted each
   file's own constants; this second pass only fills names that were still
   unresolved, since a file's own const wins over a sibling's. *)
let parse_all (collector : Woc_lib.Diag.Collector.t) (sources : (string * string) list) :
    (string * Woc_lib.Ast.program) list =
  let parsed =
    List.map
      (fun (f, src) ->
        let toks = Woc_lib.Lexer.tokenize collector ~file:f src in
        let prog = Woc_lib.Parser.parse collector ~file:f toks in
        (f, prog))
      sources
  in
  let module SM = Woc_lib.Parser.StringMap in
  let all_consts =
    List.fold_left
      (fun acc (_, prog) -> SM.fold SM.add (Woc_lib.Parser.top_level_consts prog) acc)
      SM.empty parsed
  in
  if SM.is_empty all_consts then parsed
  else List.map (fun (f, prog) -> (f, Woc_lib.Parser.subst_consts ~extra:all_consts prog)) parsed

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
        unions = SM.union keep_first acc.unions s.unions;
        modules = acc.modules @ s.modules;
      })
    Woc_lib.Types.{
      classes = SM.empty; interfaces = SM.empty; free_fns = SM.empty;
      typedefs = SM.empty; unions = SM.empty; modules = [];
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

(* ---- module identity (haxe-parity Task 1, modules) --------------------

   A file's module is its directory, relative to the root `woc` was
   pointed at — exactly the directory structure discover_dir above
   already walks, just not thrown away this time. "." denotes the root
   module itself (Filename.dirname's own convention for a name with no
   directory part — reused rather than inventing a second sentinel). A
   single-file invocation (root is not a directory — the bare-path
   `woc <file.wo>` form) has exactly one file and therefore exactly one
   module: "." unconditionally, since there is no sibling directory
   structure to differ from. *)
let module_of_file ~(root : string) (file : string) : string =
  if not (Sys.is_directory root) then "."
  else
    let root_norm =
      if String.length root > 0 && root.[String.length root - 1] = '/' then
        String.sub root 0 (String.length root - 1)
      else root
    in
    let prefix = root_norm ^ "/" in
    let plen = String.length prefix in
    let rel =
      if String.length file >= plen && String.sub file 0 plen = prefix then
        String.sub file plen (String.length file - plen)
      else file (* defensive: discover_dir always builds full = Filename.concat root rel', so this never triggers *)
    in
    Filename.dirname rel

(* Returns the existing global, flat-merged `syms` (owner.ml's and most of
   emit.ml's own view — unchanged by this task) alongside the new
   per-module tables (CRITICAL 1 review finding: the emitter needs these
   too, for the one place a flat merge is the wrong answer — see
   Types.module_symbols' own doc comment). *)
let typecheck_all (collector : Woc_lib.Diag.Collector.t) ~(root : string)
    (parsed : (string * Woc_lib.Ast.program) list) :
    Woc_lib.Types.symbols * (string, Woc_lib.Types.symbols) Hashtbl.t =
  let per_file_syms =
    List.map
      (fun (f, prog) -> (f, Woc_lib.Types.collect_declarations ~file:f prog collector))
      parsed
  in
  check_symbol_collisions collector per_file_syms;
  let module_of = module_of_file ~root in
  Woc_lib.Types.check_modules collector ~module_of per_file_syms parsed;
  let module_syms = Woc_lib.Types.module_symbols ~module_of per_file_syms in
  (* haxe-parity Task 5: the predeclared `Error` record joins the merged
     table only — see Types.with_builtin_records for why not per-file. *)
  let syms = Woc_lib.Types.with_builtin_records (merge_symbols (List.map snd per_file_syms)) in
  (* `~file_syms` (hotfix, multi-file double-report): `per_file_syms` and
     `parsed` are both `List.map`s over the same original file list, in
     the same order, so pairing them positionally is exact -- each
     file's own collect_declarations output goes with that same file's
     own prog. `syms` (the merged table) is still passed through
     separately for cross-file resolution; see typecheck_program's own
     doc comment for what narrows and what doesn't. *)
  List.iter2
    (fun (f, prog) (_, file_syms) ->
      Woc_lib.Types.typecheck_program ~file:f ~module_of ~module_syms ~file_syms prog syms collector)
    parsed per_file_syms;
  (syms, module_syms)

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
  let syms, _module_syms = typecheck_all collector ~root:path parsed in
  List.iter
    (fun (f, prog) ->
      let tables = Woc_lib.Owner.analyze ~file:f prog syms collector in
      if multi then print_string (Woc_lib.Dump.file_header f);
      print_string (Woc_lib.Dump.dump_owner tables))
    parsed;
  finish collector (build_lookup sources)

(* --dump-gc (iteration 7b, plan Phase 1): runs lex/parse/typecheck, then prints
   the inferred GC classification (one line per class). Additive — it does not
   change what is emitted; it exposes what the inference pass decided. *)
let dump_gc path =
  let sources = discover_and_read path in
  let collector = Woc_lib.Diag.Collector.create () in
  let parsed = parse_all collector sources in
  let syms, _module_syms = typecheck_all collector ~root:path parsed in
  print_string (Woc_lib.Gcinfer.render (Woc_lib.Gcinfer.classify syms));
  finish collector (build_lookup sources)

(* The bare `woc <path>` form (Task 8): runs the full pipeline with no
   dump — check-only. Nothing is printed to stdout on success, matching
   the exit-code contract's "0 = clean compile". *)
let check_only path =
  let sources = discover_and_read path in
  let collector = Woc_lib.Diag.Collector.create () in
  let parsed = parse_all collector sources in
  let syms, _module_syms = typecheck_all collector ~root:path parsed in
  List.iter (fun (f, prog) -> ignore (Woc_lib.Owner.analyze ~file:f prog syms collector)) parsed;
  finish collector (build_lookup sources)

(* ---- emit mode (plan 3, Task 1) --------------------------------------

   The whole pipeline plus the emitter. Every discovered file feeds one
   `.wob` image: class ids, interface slot ids and method indexes are
   assigned in discovery-then-declaration order, and each file keeps its
   own owner tables because node ids are minted per parse (unique within
   a file, not across files). *)

let compile_image path =
  let sources = discover_and_read path in
  let collector = Woc_lib.Diag.Collector.create () in
  let parsed = parse_all collector sources in
  let syms, module_syms = typecheck_all collector ~root:path parsed in
  let units =
    List.map
      (fun (f, prog) ->
        { Woc_lib.Emit.file = f; prog; tables = Woc_lib.Owner.analyze ~file:f prog syms collector })
      parsed
  in
  let image =
    Woc_lib.Emit.emit ~syms ~module_of:(module_of_file ~root:path) ~module_syms collector units
  in
  (collector, build_lookup sources, image)

let write_file path contents =
  try
    let oc = open_out_bin path in
    output_string oc contents;
    close_out oc
  with Sys_error msg ->
    Printf.eprintf "woc: %s\n" msg;
    exit 2

let emit_mode path out =
  let collector, lookup, image = compile_image path in
  if Woc_lib.Diag.Collector.has_error collector then finish collector lookup
  else begin
    write_file out image;
    finish collector lookup
  end

let dump_bc path =
  let collector, lookup, image = compile_image path in
  if not (Woc_lib.Diag.Collector.has_error collector) then
    print_string (Woc_lib.Disasm.dump image);
  finish collector lookup

(* ---- build mode (plan 3, Task 6): the single self-contained binary ---

   `woc build <dir> -o app` compiles like --emit, then glues together a
   runnable executable: the wovm runtime binary, the freshly compiled
   .wob image, and a fixed-size trailer so wovm's own startup
   (runtime/src/main.c) can find the embedded image via /proc/self/exe
   and ignore argv. Trailer layout is docs/plan/oop-vm/00-wob-format.md's
   "single-binary trailer" section -- this writer and main.c's reader
   must never disagree about it.

   Edge cases, decided and documented alongside the trailer format:
   - output path already exists: overwritten, but atomically (build to a
     temp file next to -o, then rename over it) so a failed build never
     clobbers a working binary with a partial one.
   - a directory with no `main`: unlike --emit (where a .wob with no
     entry is a legitimate artifact), `build`'s whole point is something
     you can run, so this is a build-time error, not deferred to wovm's
     own "module has no entry method" at run time.
   - the --runtime binary is itself already a built single binary (has
     its own trailer): its embedded payload is stripped before copying,
     so rebuilding from a built binary doesn't chain payloads/trailers. *)

let trailer_magic = 0x31544257l (* "WBT1" read as LE u32 (mirrors WOB_MAGIC's "WOB1") *)
let trailer_size = 20 (* payload_off u64, payload_len u64, magic u32 *)
let wob_off_entry = 40 (* WOB_OFF_ENTRY, runtime/src/wob.h *)

let trailer_bytes ~(payload_off : int) ~(payload_len : int) : bytes =
  let t = Bytes.create trailer_size in
  Bytes.set_int64_le t 0 (Int64.of_int payload_off);
  Bytes.set_int64_le t 8 (Int64.of_int payload_len);
  Bytes.set_int32_le t 16 trailer_magic;
  t

(* If `rt` already carries a valid trailer of our own (i.e. it's itself
   the output of a previous `woc build`), its embedded payload is dead
   weight for a fresh build: return just the pristine runtime prefix.
   Anything that doesn't look unambiguously like our own trailer (wrong
   magic, or offsets that don't exactly account for every trailing byte)
   is returned untouched -- the safe default when it's not certain. *)
let strip_existing_trailer (rt : string) : string =
  let n = String.length rt in
  if n < trailer_size then rt
  else if String.get_int32_le rt (n - 4) <> trailer_magic then rt
  else
    let payload_off = Int64.to_int (String.get_int64_le rt (n - trailer_size)) in
    let payload_len = Int64.to_int (String.get_int64_le rt (n - trailer_size + 8)) in
    if payload_off >= 0 && payload_off <= n - trailer_size
       && payload_len = n - trailer_size - payload_off
    then String.sub rt 0 payload_off
    else rt

(* keep in sync with the repo-root VERSION file; `just dist` asserts that
   `woc version`, `wovm --version`, and VERSION all agree before packaging. *)
let toolchain_version = "0.1.0"

(* the wovm the standalone build embeds into. When neither --runtime nor the
   manifest's [build] runtime is given, resolve the default in order:
     1. $WO_RUNTIME                    -- explicit override
     2. a `wovm` beside this `woc`     -- the tarball layout <prefix>/bin/{woc,wovm},
                                          located via Sys.executable_name
     3. `runtime/wovm` relative to CWD -- the repo/dev fallback *)
let default_runtime_path () : string =
  match Sys.getenv_opt "WO_RUNTIME" with
  | Some p when p <> "" -> p
  | _ ->
    let sibling = Filename.concat (Filename.dirname Sys.executable_name) "wovm" in
    if Sys.file_exists sibling && not (Sys.is_directory sibling) then sibling
    else "runtime/wovm"

let build_mode ~(runtime : string option) (path : string) (out : string) : unit =
  let collector, lookup, image = compile_image path in
  if Woc_lib.Diag.Collector.has_error collector then finish collector lookup
  else begin
    if String.get_int32_le image wob_off_entry = -1l then begin
      Printf.eprintf
        "woc: %s: no `main` entry point found; `build` requires a zero-argument free fn named \
         `main`\n"
        path;
      exit 2
    end;
    let rt_path = match runtime with Some p -> p | None -> default_runtime_path () in
    if (not (Sys.file_exists rt_path)) || Sys.is_directory rt_path then begin
      Printf.eprintf "woc: runtime binary not found at '%s' -- build it with: make -C runtime wovm\n"
        rt_path;
      exit 2
    end;
    let rt_bytes =
      match read_source rt_path with
      | Ok s -> strip_existing_trailer s
      | Error msg ->
        Printf.eprintf "woc: %s\n" msg;
        exit 2
    in
    let tmp = out ^ ".woc-build.tmp" in
    (* stale tmp from an interrupted earlier build must not survive: its
       permission bits would leak through, since Open_creat on an
       existing inode does not apply the requested mode *)
    (try Sys.remove tmp with Sys_error _ -> ());
    (try
       let oc = open_out_gen [ Open_wronly; Open_creat; Open_trunc; Open_binary ] 0o755 tmp in
       output_string oc rt_bytes;
       output_string oc image;
       output_bytes oc
         (trailer_bytes ~payload_off:(String.length rt_bytes) ~payload_len:(String.length image));
       close_out oc
     with Sys_error msg ->
       (try Sys.remove tmp with Sys_error _ -> ());
       Printf.eprintf "woc: %s\n" msg;
       exit 2);
    (try Sys.rename tmp out
     with Sys_error msg ->
       Printf.eprintf "woc: %s\n" msg;
       exit 2);
    finish collector lookup
  end

(* ---- manifest build ---------------------------------------------------
   `woc <dir>` where <dir>/wo.toml exists is a BUILD, not a check: the
   manifest names the application, so pointing woc at the project is enough
   (`woc .` inside it). The schema is the one the sample already carried —
   top-level `name` (the executable's basename), `version`, `description`,
   a `[runtime]` section whose `wo` constraint is a minimum toolchain
   version, enforced against `woc`'s own version — plus one new section
   this feature adds:

     [build]
     runtime = "../runtime/wovm" # optional: wovm to prepend, relative to
                                 # the manifest's own directory (default:
                                 # `runtime/wovm` relative to the CWD, the
                                 # same as `woc build` with no --runtime)
     target = "target"           # optional: output directory, relative to
                                 # the manifest's directory

   Anything else is an error: a typo'd key silently ignored would build the
   wrong thing. A directory WITHOUT wo.toml keeps today's meaning (check
   only), and the corpus is full of those. *)
let manifest_parse (path : string) : (string * string) list =
  let fail line msg =
    Printf.eprintf "woc: %s:%d: %s\n" path line msg;
    exit 2
  in
  let ic = try open_in path with Sys_error m -> Printf.eprintf "woc: %s\n" m; exit 2 in
  let kvs = ref [] in
  let section = ref "" in
  let lineno = ref 0 in
  (try
     while true do
       let raw = input_line ic in
       incr lineno;
       let line = String.trim raw in
       if line = "" || (String.length line >= 1 && line.[0] = '#') then ()
       else if line.[0] = '[' then begin
         if line.[String.length line - 1] <> ']' then fail !lineno "malformed section header";
         (* accept `[[table.array]]` headers too (iteration 9c's
            [[share.clients]]) by trimming the doubled brackets *)
         let inner = String.sub line 1 (String.length line - 2) in
         let inner =
           if String.length inner >= 2 && inner.[0] = '[' && inner.[String.length inner - 1] = ']'
           then String.sub inner 1 (String.length inner - 2)
           else inner
         in
         section := inner;
         if !section <> "runtime" && !section <> "build" && !section <> "share"
            && !section <> "share.clients"
         then
           fail !lineno
             (Printf.sprintf "unknown section [%s] (runtime and build exist)" !section)
       end
       else if !section = "share" || !section = "share.clients" then
         () (* iteration 9c manifest keys — parsed by the attach feature, ignored here *)
       else
         match String.index_opt line '=' with
         | None -> fail !lineno "expected `key = \"value\"`"
         | Some eq ->
           let key = String.trim (String.sub line 0 eq) in
           let v = String.trim (String.sub line (eq + 1) (String.length line - eq - 1)) in
           if String.length v < 2 || v.[0] <> '"' || v.[String.length v - 1] <> '"' then
             fail !lineno (Printf.sprintf "`%s`: only quoted string values are supported" key);
           let v = String.sub v 1 (String.length v - 2) in
           let known =
             match (!section, key) with
             | "", ("name" | "version" | "description") -> true
             | "runtime", "wo" -> true (* minimum toolchain version; enforced in manifest_build *)
             | "build", ("runtime" | "target") -> true
             | _ -> false
           in
           if not known then
             fail !lineno
               (if !section = "" then
                  Printf.sprintf "unknown key `%s` (name, version, description exist)" key
                else
                  Printf.sprintf "unknown key `%s` in [%s]" key !section);
           kvs := ((if !section = "" then key else !section ^ "." ^ key), v) :: !kvs
     done
   with End_of_file -> close_in ic);
  !kvs

(* [runtime] wo = ">= X.Y" — a minimum-toolchain-version constraint. Only `>=`
   and a bare version are interpreted; any other operator is accepted untouched
   (forward-compatible — don't hard-fail on a constraint syntax not grokked
   yet). Compared as a (major, minor, patch) triple. *)
let ver_triple (s : string) : int * int * int =
  match String.split_on_char '.' s |> List.filter_map int_of_string_opt with
  | [ a ] -> (a, 0, 0)
  | [ a; b ] -> (a, b, 0)
  | a :: b :: c :: _ -> (a, b, c)
  | [] -> (0, 0, 0)

let check_runtime_constraint (mf : string) (c : string) : unit =
  let c = String.trim c in
  let min_req =
    if String.length c >= 2 && String.sub c 0 2 = ">=" then
      Some (String.trim (String.sub c 2 (String.length c - 2)))
    else if String.length c > 0 && c.[0] >= '0' && c.[0] <= '9' then Some c
    else None
  in
  match min_req with
  | Some req when compare (ver_triple toolchain_version) (ver_triple req) < 0 ->
    Printf.eprintf
      "woc: %s: project requires writeonce %s, but this toolchain is %s -- upgrade the toolchain\n"
      mf c toolchain_version;
    exit 2
  | _ -> ()

let manifest_build (dir : string) : unit =
  let mf = Filename.concat dir "wo.toml" in
  let kvs = manifest_parse mf in
  let get k = List.assoc_opt k kvs in
  (match get "runtime.wo" with Some c -> check_runtime_constraint mf c | None -> ());
  let name =
    match get "name" with
    | Some n when n <> "" && not (String.contains n '/') -> n
    | Some _ ->
      Printf.eprintf "woc: %s: `name` must be a bare file name\n" mf;
      exit 2
    | None ->
      Printf.eprintf "woc: %s: `name` is required\n" mf;
      exit 2
  in
  (* paths in the manifest are the PROJECT's, so they resolve against the
     manifest's directory — `woc .` inside the project and `woc path/to/it`
     from anywhere must build the same thing *)
  let resolve rel = if Filename.is_relative rel then Filename.concat dir rel else rel in
  let runtime = Option.map resolve (get "build.runtime") in
  let target = resolve (match get "build.target" with Some t when t <> "" -> t | _ -> "target") in
  (if not (Sys.file_exists target) then
     try Sys.mkdir target 0o755
     with Sys_error m ->
       Printf.eprintf "woc: %s\n" m;
       exit 2);
  build_mode ~runtime dir (Filename.concat target name)

let () =
  match Sys.argv with
  | [| _; "--dump-tokens"; path |] -> dump_tokens path
  | [| _; "--dump-ast"; path |] -> dump_ast path
  | [| _; "--dump-owner"; path |] -> dump_owner path
  | [| _; "--dump-gc"; path |] -> dump_gc path
  | [| _; "--dump-bc"; path |] -> dump_bc path
  | [| _; "--emit"; path; "-o"; out |] -> emit_mode path out
  | [| _; "build"; path; "-o"; out |] -> build_mode ~runtime:None path out
  | [| _; "build"; path; "-o"; out; "--runtime"; rt |] -> build_mode ~runtime:(Some rt) path out
  | [| _; "build"; path; "--runtime"; rt; "-o"; out |] -> build_mode ~runtime:(Some rt) path out
  | [| _; ("version" | "--version") |] ->
    (* Go-style: `writeonce <ver> <os>/<arch>`. linux/amd64 is the only target. *)
    Printf.printf "writeonce %s linux/amd64\n" toolchain_version
  | [| _; path |] ->
    if Sys.file_exists path && Sys.is_directory path
       && Sys.file_exists (Filename.concat path "wo.toml")
    then manifest_build path
    else check_only path
  | _ ->
    prerr_string usage_msg;
    exit 2
