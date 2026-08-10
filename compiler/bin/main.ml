(* woc — the writeonce OCaml compiler front end.

   Task 1 scaffolded the CLI's exit-code contract with no compiler
   stage behind it. Task 2 added diagnostics (compiler/src/diag.ml).
   Task 3 added the lexer (compiler/src/{token,lexer}.ml) behind
   --dump-tokens. Task 4 adds the declaration parser
   (compiler/src/{ast,parser}.ml) behind --dump-ast, printing a stable,
   golden-diffed AST dump (compiler/src/dump.ml) to stdout. Tasks 5-7
   add statement/expression parsing, the typechecker, and the ownership
   pass behind their own --dump-* flags; Task 8 adds directory
   discovery and multi-file programs, at which point the bare
   `woc <path>` form below starts actually compiling instead of just
   checking existence.

     0 = clean compile
     1 = diagnostics reported (reachable now: --dump-tokens on source
         with an unknown character, or --dump-ast on source with a
         broken declaration, reports WO-E0xx/WO-E1xx and exits 1)
     2 = usage or IO failure

   With no arguments, this prints usage to stderr and exits 2. Given a
   single path argument (no flag), it exits 0 if the path exists and 2
   (with a stderr message) if it does not — this bare-path form
   predates any real compilation and stays as-is until Task 8. *)

let usage_msg =
  "usage: woc <path>\n\
   usage: woc --dump-tokens <file.wo>\n\
   usage: woc --dump-ast <file.wo>\n\
   \n\
   Compiles writeonce (.wo) source. <path> is a single .wo file or a\n\
   directory to discover .wo files under (directory discovery lands in\n\
   a later task; compilation itself has not landed yet either).\n\
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
   Exit codes: 0 clean, 1 diagnostics reported, 2 usage/IO failure.\n"

let read_source path =
  try
    let ic = open_in_bin path in
    let n = in_channel_length ic in
    let s = really_input_string ic n in
    close_in ic;
    Ok s
  with Sys_error msg -> Error msg

let dump_tokens path =
  if not (Sys.file_exists path) then begin
    Printf.eprintf "woc: no such file or directory: %s\n" path;
    exit 2
  end;
  match read_source path with
  | Error msg ->
    Printf.eprintf "woc: %s\n" msg;
    exit 2
  | Ok src ->
    let collector = Woc_lib.Diag.Collector.create () in
    let toks = Woc_lib.Lexer.tokenize collector ~file:path src in
    print_string (Woc_lib.Dump.dump_tokens toks);
    if Woc_lib.Diag.Collector.has_error collector then begin
      let lookup f = if f = path then Some src else None in
      prerr_string (Woc_lib.Diag.Collector.render_all collector lookup);
      prerr_newline ();
      exit 1
    end
    else exit 0

let dump_ast path =
  if not (Sys.file_exists path) then begin
    Printf.eprintf "woc: no such file or directory: %s\n" path;
    exit 2
  end;
  match read_source path with
  | Error msg ->
    Printf.eprintf "woc: %s\n" msg;
    exit 2
  | Ok src ->
    let collector = Woc_lib.Diag.Collector.create () in
    let toks = Woc_lib.Lexer.tokenize collector ~file:path src in
    let prog = Woc_lib.Parser.parse collector ~file:path toks in
    print_string (Woc_lib.Dump.dump_ast prog);
    if Woc_lib.Diag.Collector.has_error collector then begin
      let lookup f = if f = path then Some src else None in
      prerr_string (Woc_lib.Diag.Collector.render_all collector lookup);
      prerr_newline ();
      exit 1
    end
    else exit 0

let () =
  match Sys.argv with
  | [| _; "--dump-tokens"; path |] -> dump_tokens path
  | [| _; "--dump-ast"; path |] -> dump_ast path
  | [| _; path |] ->
    if Sys.file_exists path then exit 0
    else begin
      Printf.eprintf "woc: no such file or directory: %s\n" path;
      exit 2
    end
  | _ ->
    prerr_string usage_msg;
    exit 2
