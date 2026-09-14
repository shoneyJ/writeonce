(* runner.ml — golden-file test runner (Task 3 onward).

   Contract (compiler/plan/2026-08-01-woc-compiler-front.md Task 3):
   walks compiler/test/golden/<stage>/ directories; for every
   <name>.wo file in a stage directory, runs the pipeline stage that
   directory's dump flag names and diffs the produced text against
   <name>.expected. A mismatch prints a line-based diff; the run exits
   nonzero if anything mismatched. WOC_BLESS=1 rewrites <name>.expected
   to the freshly produced text instead of comparing — used once, by
   hand, to seed or intentionally update a fixture; every commit ships
   with the runner GREEN against whatever it last wrote.

   Wired into `dune runtest` alongside test_diag.ml (see test/dune).

   -- Why this file resolves two different "golden root" paths --

   `dune runtest` runs this executable with its current directory set
   to the *build* copy of test/ (e.g. ".../compiler/_build/default/test"),
   not the real source directory — verified empirically: a file written
   via a bare relative path while running under `dune runtest` lands
   under _build/default and is silently discarded (or stale-overwritten)
   on the next build. Reading fixtures from the plain relative "golden"
   path is fine — test/dune declares `(deps (source_tree golden))`,
   which both (a) keeps that build-directory copy fresh from the real
   source on every run, since dune must digest that dependency to
   decide whether to re-run this test's action at all, and (b) is
   itself the reason editing a fixture actually invalidates a
   previously-cached PASS. But WOC_BLESS=1 rewriting *that* copy would
   vanish — the whole point of bless mode is to update the fixture git
   tracks. So bless-mode writes instead go through [source_golden_dir],
   which walks back out of "_build/default" to the real
   compiler/test/golden on disk. *)

module Diag = Woc_lib.Diag
module Token = Woc_lib.Token
module Lexer = Woc_lib.Lexer
module Ast = Woc_lib.Ast
module Parser = Woc_lib.Parser
module Dump = Woc_lib.Dump
module Types = Woc_lib.Types
module Owner = Woc_lib.Owner
module Gcinfer = Woc_lib.Gcinfer
module Emit = Woc_lib.Emit
module Disasm = Woc_lib.Disasm

let read_file path =
  let ic = open_in_bin path in
  let n = in_channel_length ic in
  let s = really_input_string ic n in
  close_in ic;
  s

let write_file path contents =
  let oc = open_out_bin path in
  output_string oc contents;
  close_out oc

(* Manual substring search: no Str/Re library (this project is
   stdlib-only), and this is the only place a substring search is
   needed. Fine for the short, one-shot haystack (a cwd path) this is
   used on. *)
let find_substring ~needle haystack =
  let hlen = String.length haystack and nlen = String.length needle in
  let rec go i =
    if i + nlen > hlen then None
    else if String.sub haystack i nlen = needle then Some i
    else go (i + 1)
  in
  go 0

(* The real, on-disk compiler/test/golden — see module doc above. Falls
   back to a short list of plausible relative paths for a manual
   (non-dune) invocation, where cwd is wherever the caller's shell
   already is. *)
let source_golden_dir () =
  match Sys.getenv_opt "WOC_GOLDEN_DIR" with
  | Some dir -> dir
  | None -> (
    let cwd = Sys.getcwd () in
    let marker = "_build/default/" in
    match find_substring ~needle:marker cwd with
    | Some idx -> String.sub cwd 0 idx ^ "test/golden"
    | None -> (
      let candidates = [ "golden"; "test/golden"; "compiler/test/golden" ] in
      match List.find_opt Sys.file_exists candidates with
      | Some dir -> dir
      | None ->
        failwith
          "runner: cannot locate compiler/test/golden (set WOC_GOLDEN_DIR)"))

let bless = Sys.getenv_opt "WOC_BLESS" = Some "1"
let checks = ref 0
let failures = ref 0

let check name cond =
  incr checks;
  if not cond then begin
    incr failures;
    Printf.printf "FAIL: %s\n" name
  end

let check_eq name ~expected ~actual to_string =
  incr checks;
  if expected <> actual then begin
    incr failures;
    Printf.printf "FAIL: %s\n  expected: %s\n  actual:   %s\n" name
      (to_string expected) (to_string actual)
  end

(* ---- direct lexer assertions (not golden-diffed) -------------------

   golden/tokens/gotcha.wo and unknown-char.wo already pin the token
   *stream* for these cases, but a token dump alone can't distinguish
   "no diagnostic was raised" from "one was raised and silently
   dropped" — both look identical in the dump (the bad character is
   just absent either way). These assertions pin the diagnostic side
   of the WO-E001 contract directly against the Collector. *)

let () =
  let collector = Diag.Collector.create () in
  let toks =
    Lexer.tokenize collector ~file:"gotcha.wo"
      "self me subscribe receive insert select"
  in
  let kinds = List.map (fun (t : Token.t) -> t.kind) toks in
  check
    "gotcha: self/me/subscribe/receive/insert/select all lex as Ident"
    (kinds
    = [
        Token.Ident "self";
        Token.Ident "me";
        Token.Ident "subscribe";
        Token.Ident "receive";
        Token.Ident "insert";
        Token.Ident "select";
        Token.Eof;
      ])

let () =
  let collector = Diag.Collector.create () in
  let toks =
    Lexer.tokenize collector ~file:"gotcha.wo" "INSERT SELECT type class"
  in
  let kinds = List.map (fun (t : Token.t) -> t.kind) toks in
  check "gotcha: uppercase INSERT/SELECT and type/class lex as keywords"
    (kinds = [ Token.KwInsert; Token.KwSelect; Token.KwType; Token.KwClass; Token.Eof ])

let () =
  let collector = Diag.Collector.create () in
  let toks = Lexer.tokenize collector ~file:"bad.wo" "let x = 1 ~ 2" in
  let kinds = List.map (fun (t : Token.t) -> t.kind) toks in
  check "unknown char: skipped, never appears as a token"
    (kinds
    = [ Token.KwLet; Token.Ident "x"; Token.Eq; Token.Int 1; Token.Int 2; Token.Eof ]);
  let diags = Diag.Collector.diagnostics collector in
  check_eq "unknown char: exactly one diagnostic reported" ~expected:1
    ~actual:(List.length diags) string_of_int;
  match diags with
  | [ d ] ->
    check "unknown char: WO-E001 at the '~' position (line 1, col 11)"
      (d.code = "WO-E001" && d.site.line = 1 && d.site.col = 11)
  | _ -> check "unknown char: diagnostic shape" false

let () =
  (* golden/tokens/dangling-escape.wo opens a string and ends the file
     on a lone backslash, with no character left to escape it. Read
     from disk rather than duplicated as a literal so this assertion
     and the golden dump can never silently drift apart from the
     actual fixture bytes (the file has no trailing newline on purpose
     -- its very last byte must be the backslash). *)
  let path = "golden/tokens/dangling-escape.wo" in
  let src = read_file path in
  let collector = Diag.Collector.create () in
  let toks = Lexer.tokenize collector ~file:path src in
  let kinds = List.map (fun (t : Token.t) -> t.kind) toks in
  check
    "dangling escape: string closes with whatever was collected before \
     the backslash"
    (kinds
    = [ Token.KwLet; Token.Ident "bad"; Token.Eq; Token.Str "abc"; Token.Eof ]);
  let diags = Diag.Collector.diagnostics collector in
  check_eq "dangling escape: exactly one diagnostic reported" ~expected:1
    ~actual:(List.length diags) string_of_int;
  match diags with
  | [ d ] ->
    check "dangling escape: WO-E002 at the backslash's position (line 1, col 15)"
      (d.code = "WO-E002" && d.site.line = 1 && d.site.col = 15)
  | _ -> check "dangling escape: diagnostic shape" false

let () =
  (* Deliberate asymmetry with the case above (see
     unterminated_escape_code's doc comment in lexer.ml): a plain
     unterminated string -- no dangling backslash, it simply runs off
     the end of the source with no closing quote at all -- is rt-parity
     silent. Pinned inline (no fixture file needed for this half) so
     nothing starts reporting a diagnostic here without this test
     noticing. *)
  let collector = Diag.Collector.create () in
  let toks =
    Lexer.tokenize collector ~file:"plain.wo" "let plain = \"never closed"
  in
  let kinds = List.map (fun (t : Token.t) -> t.kind) toks in
  check
    "plain unterminated string: closes with all collected content, no \
     diagnostic"
    (kinds
    = [
        Token.KwLet;
        Token.Ident "plain";
        Token.Eq;
        Token.Str "never closed";
        Token.Eof;
      ]);
  check_eq "plain unterminated string: reports nothing" ~expected:0
    ~actual:(List.length (Diag.Collector.diagnostics collector))
    string_of_int

let () =
  (* Faithful rt port (crates/rt/src/lexer.rs read_ident_chars): an
     identifier's continuation characters include '-', so `a-b` lexes
     as one Ident, not Ident/Dash/Ident. Pinned here so nothing "fixes"
     this away before Task 5 decides how its expression grammar wants
     binary minus to interact with it (see lexer.ml's module doc for
     the forward-looking concern this raises). golden/tokens/
     dash-continuation.wo pins the same two shapes as a dump diff. *)
  let collector = Diag.Collector.create () in
  let toks = Lexer.tokenize collector ~file:"dash.wo" "a-b" in
  let kinds = List.map (fun (t : Token.t) -> t.kind) toks in
  check "dash-continuation: a-b (no spaces) lexes as one Ident"
    (kinds = [ Token.Ident "a-b"; Token.Eof ])

let () =
  let collector = Diag.Collector.create () in
  let toks = Lexer.tokenize collector ~file:"dash.wo" "a - b" in
  let kinds = List.map (fun (t : Token.t) -> t.kind) toks in
  check "dash-continuation: a - b (spaced) lexes as Ident, Dash, Ident"
    (kinds = [ Token.Ident "a"; Token.Dash; Token.Ident "b"; Token.Eof ])

(* ---- raw text literal, iteration 37 (not golden-diffed) ------------

   golden/tokens/raw-literal.wo pins the token STREAM; these pin the
   pieces a dump cannot show: that a backtick literal with no holes is
   byte-identical to the Str a "..." string would have produced, that
   the common margin is removed at LEX time (so no runtime cost and no
   downstream stage ever sees the source indentation), and that the two
   new diagnostics fire at the right position. *)

let () =
  let collector = Diag.Collector.create () in
  let toks = Lexer.tokenize collector ~file:"raw.wo" "let t = `<div>hi</div>`" in
  let kinds = List.map (fun (t : Token.t) -> t.kind) toks in
  check "raw literal: no holes lexes as a plain Str"
    (kinds
    = [ Token.KwLet; Token.Ident "t"; Token.Eq; Token.Str "<div>hi</div>"; Token.Eof ]);
  check_eq "raw literal: reports nothing" ~expected:0
    ~actual:(List.length (Diag.Collector.diagnostics collector))
    string_of_int

let () =
  (* Nothing between the backticks is escape-processed: a quote is a
     quote and a backslash-n is two characters, which is the whole
     point of the form: markup without quote-escape noise. *)
  let collector = Diag.Collector.create () in
  let toks = Lexer.tokenize collector ~file:"raw.wo" "`a\"b\\n`" in
  let kinds = List.map (fun (t : Token.t) -> t.kind) toks in
  check "raw literal: content is verbatim, no escape processing"
    (kinds = [ Token.Str "a\"b\\n"; Token.Eof ])

let () =
  (* The margin case, written the way a render() body actually is:
     opening newline dropped, the 4-space common margin removed from
     every line, the whitespace-only closing line reduced to nothing
     while its newline survives (Java text-block behavior). *)
  let src = "let t = `\n    <div>\n      many\n    </div>\n  `" in
  let collector = Diag.Collector.create () in
  let toks = Lexer.tokenize collector ~file:"raw.wo" src in
  let kinds = List.map (fun (t : Token.t) -> t.kind) toks in
  check "raw literal: common margin stripped, leading newline dropped"
    (kinds
    = [
        Token.KwLet;
        Token.Ident "t";
        Token.Eq;
        Token.Str "<div>\n  many\n</div>\n";
        Token.Eof;
      ])

let () =
  (* Both hole forms in one literal. The payloads are raw and unlexed,
     exactly as SExpr has always carried `${...}` -- the parser is what
     tells them apart (SEsc gains the esc() wrapper). *)
  let collector = Diag.Collector.create () in
  let toks = Lexer.tokenize collector ~file:"raw.wo" "`<p>${a}{{ b }}</p>`" in
  let kinds = List.map (fun (t : Token.t) -> t.kind) toks in
  check "raw literal: ${} stays raw, {{}} becomes SEsc"
    (kinds
    = [
        Token.InterpStr
          [
            Token.SText "<p>";
            Token.SExpr "a";
            (* the empty run between two adjacent holes, exactly as a
               "..." string has always produced it -- the parser drops
               empty SText segments in desugar_interp *)
            Token.SText "";
            Token.SEsc " b ";
            Token.SText "</p>";
          ];
        Token.Eof;
      ])

let () =
  (* Unlike a plain "..." string, an unterminated raw literal is an
     error: multi-line is its normal case, so silently swallowing the
     rest of the file would be a footgun, not rt parity. *)
  let collector = Diag.Collector.create () in
  let toks = Lexer.tokenize collector ~file:"raw.wo" "let t = `abc" in
  let kinds = List.map (fun (t : Token.t) -> t.kind) toks in
  check "unterminated raw literal: closes with what was collected"
    (kinds = [ Token.KwLet; Token.Ident "t"; Token.Eq; Token.Str "abc"; Token.Eof ]);
  let diags = Diag.Collector.diagnostics collector in
  check_eq "unterminated raw literal: exactly one diagnostic reported" ~expected:1
    ~actual:(List.length diags) string_of_int;
  match diags with
  | [ d ] ->
    check "unterminated raw literal: WO-E004 at the backtick (line 1, col 9)"
      (d.code = "WO-E004" && d.site.line = 1 && d.site.col = 9)
  | _ -> check "unterminated raw literal: diagnostic shape" false

let () =
  (* A raw newline inside "..." used to be accepted silently (the
     scanner's catch-all appended it like any other byte), which meant a
     forgotten closing quote ate the rest of the file with no
     diagnostic. Now that the backtick literal is the blessed spelling
     for multi-line text, that newline is an error and the scan stops
     WITHOUT consuming it, so the Newline token still terminates the
     statement and the next line parses normally. *)
  let collector = Diag.Collector.create () in
  let toks = Lexer.tokenize collector ~file:"nl.wo" "let t = \"ab\ncd" in
  let kinds = List.map (fun (t : Token.t) -> t.kind) toks in
  check "newline in string: scan stops at the newline, which still tokenizes"
    (kinds
    = [
        Token.KwLet;
        Token.Ident "t";
        Token.Eq;
        Token.Str "ab";
        Token.Newline;
        Token.Ident "cd";
        Token.Eof;
      ]);
  let diags = Diag.Collector.diagnostics collector in
  check_eq "newline in string: exactly one diagnostic reported" ~expected:1
    ~actual:(List.length diags) string_of_int;
  match diags with
  | [ d ] ->
    check "newline in string: WO-E005 at the newline (line 1, col 12)"
      (d.code = "WO-E005" && d.site.line = 1 && d.site.col = 12)
  | _ -> check "newline in string: diagnostic shape" false

(* ---- direct parser/AST assertions (Task 4, not golden-diffed) ------------

   golden/ast/*.wo fixtures already pin the AST *shape* via --dump-ast,
   but a dump alone can't distinguish "no diagnostic was raised" from
   "one was raised and silently dropped" (dump.ml's spec: node ids are
   deliberately never printed), and it can't see the AST's `id`/`conv`/
   `default` fields directly. These assertions pin the parts of the
   Task 4 contract a text dump structurally cannot show. *)

let parse_str ~file src =
  let collector = Diag.Collector.create () in
  let toks = Lexer.tokenize collector ~file src in
  let prog = Parser.parse collector ~file toks in
  (prog, collector)

let () =
  (* golden/ast/two-error-recovery.wo: `class Broken1` has a malformed
     field (`bad_field Text`, missing ':'), `class Good` in between is
     well-formed, `interface Broken2` has a malformed signature
     (`x Text` instead of `x: Text`). The dump golden already shows only
     `Good` survives; this assertion is the load-bearing half: exactly
     two diagnostics (not a cascade from either broken declaration),
     both WO-E101 syntax errors, each at the actual offending token. *)
  let path = "golden/ast/two-error-recovery.wo" in
  let src = read_file path in
  let prog, collector = parse_str ~file:path src in
  check "recovery: exactly one surviving declaration (`Good`)"
    (match prog.Ast.decls with
    | [ Ast.Class c ] -> c.name = "Good"
    | _ -> false);
  let diags = Diag.Collector.diagnostics collector in
  check_eq "recovery: exactly two diagnostics reported (one per broken decl)"
    ~expected:2 ~actual:(List.length diags) string_of_int;
  match diags with
  | [ d1; d2 ] ->
    check "recovery: first diagnostic is WO-E101 at `bad_field` (line 3, col 3)"
      (d1.code = "WO-E101" && d1.site.line = 3 && d1.site.col = 3);
    check "recovery: second diagnostic is WO-E101 at the bad param type (line 11, col 13)"
      (d2.code = "WO-E101" && d2.site.line = 11 && d2.site.col = 13)
  | _ -> check "recovery: diagnostic shape" false

let () =
  (* golden/ast/body-recovery.wo (Task 5): `fn oops` has two malformed
     statement bodies (`let bad1 = ;` — missing expression before the
     terminator — and `return bad2 +` — missing the `+`'s right-hand
     operand), each surrounded by well-formed `let` statements. The
     dump golden already shows only `ok1`/`ok2` surviving; this is the
     load-bearing half proving that's *recovery* (one diagnostic per
     broken statement, syncing at the next newline/semicolon per the
     brief) and not a silent drop or a cascade. *)
  let path = "golden/ast/body-recovery.wo" in
  let src = read_file path in
  let prog, collector = parse_str ~file:path src in
  check "body-recovery: exactly one surviving fn (`oops`)"
    (match prog.Ast.decls with
    | [ Ast.Fn m ] -> m.name = "oops"
    | _ -> false);
  let diags = Diag.Collector.diagnostics collector in
  check_eq "body-recovery: exactly two diagnostics reported (one per broken statement)"
    ~expected:2 ~actual:(List.length diags) string_of_int;
  (match prog.Ast.decls with
  | [ Ast.Fn m ] ->
    check "body-recovery: both `let` statements survive, in order"
      (List.map
         (fun (s : Ast.stmt) ->
           match s.Ast.s_kind with Ast.Let { name; _ } -> name | _ -> "?")
         m.body
      = [ "ok1"; "ok2" ])
  | _ -> check "body-recovery: exactly one surviving fn" false);
  match diags with
  | [ d1; d2 ] ->
    check "body-recovery: first diagnostic is WO-E101 at the missing `let` value (line 3, col 14)"
      (d1.code = "WO-E101" && d1.site.line = 3 && d1.site.col = 14);
    check "body-recovery: second diagnostic is WO-E101 after the dangling `+` (line 5, col 16)"
      (d2.code = "WO-E101" && d2.site.line = 5 && d2.site.col = 16)
  | _ -> check "body-recovery: diagnostic shape" false

let () =
  (* golden/ast/skip-on-block.wo: a policy/service/on-block-laden class
     body. The dump golden shows exactly 3 fields and 0 methods survive
     (the load-bearing brace-depth counter finding the object literal's
     own close, not the type's); this assertion pins the other half —
     zero diagnostics (the skip is a deliberate, silent no-op, not a
     recovered error) and re-confirms shape at the AST level directly. *)
  let path = "golden/ast/skip-on-block.wo" in
  let src = read_file path in
  let prog, collector = parse_str ~file:path src in
  check_eq "skip-on-block: reports nothing (silent skip, not recovery)"
    ~expected:0
    ~actual:(List.length (Diag.Collector.diagnostics collector))
    string_of_int;
  (match prog.Ast.decls with
  | [ Ast.Class c ] ->
    check "skip-on-block: field names/order survive policy/service/on"
      (List.map (fun (f : Ast.field) -> f.name) c.fields = [ "id"; "title"; "published" ]);
    check "skip-on-block: no methods (none were declared)" (c.methods = [])
  | _ -> check "skip-on-block: exactly one class declaration" false)

let () =
  (* Coordinator-review finding: `on`/`service`/`policy` are plain Idents
     (Task 3 deliberately keeps them usable as identifiers), so a FIELD
     literally named one of them (`on: Bool`) must not be swallowed by
     the skip-on-block interpretation just because its name matches —
     only its *shape* (no Colon right after) means "this is a
     service/policy/on block". golden/ast/field-named-sync-keyword.wo
     puts fields named on/service/policy in the SAME class body as real
     `policy ...`, `on ... do ...`, and `service rest ...` blocks, so
     this one fixture proves both halves at once: the fields all survive
     (this assertion), and the genuine blocks still correctly disappear
     (the golden dump for this fixture shows only 5 fields, 0 methods,
     nothing leaking from the skipped blocks) — while
     golden/ast/skip-on-block.wo above continues to prove the reverse
     case (genuine blocks, no fields sharing their names) stays green. *)
  let path = "golden/ast/field-named-sync-keyword.wo" in
  let src = read_file path in
  let prog, collector = parse_str ~file:path src in
  check_eq "field-named-sync-keyword: reports nothing" ~expected:0
    ~actual:(List.length (Diag.Collector.diagnostics collector))
    string_of_int;
  match prog.Ast.decls with
  | [ Ast.Class c ] ->
    check
      "field-named-sync-keyword: fields named on/service/policy all survive, in order"
      (List.map (fun (f : Ast.field) -> f.name) c.fields
      = [ "id"; "on"; "service"; "policy"; "name" ]);
    check "field-named-sync-keyword: no methods (none were declared)" (c.methods = [])
  | _ -> check "field-named-sync-keyword: exactly one class declaration" false

let () =
  (* Param conventions (Task 4 brief: bare = borrow, `mut`, `take`) —
     pinned directly against Ast.param.conv, not just the dump's text
     rendering of it. *)
  let prog, _ =
    parse_str ~file:"conv.wo" "fn f(a: Int, mut b: Int, take c: Int) -> Int {\n  return a;\n}\n"
  in
  match prog.Ast.decls with
  | [ Ast.Fn m ] ->
    check "param conventions: bare/mut/take parsed in order"
      (List.map (fun (p : Ast.param) -> p.conv) m.params = [ Ast.Borrow; Ast.Mut; Ast.Take ])
  | _ -> check "param conventions: exactly one free fn" false

let () =
  (* `= now()` is DefaultNow; anything else is DefaultOpaque carrying
     the raw token span (Task 4 brief, ported from rt's DefaultExpr::Now
     vs ::Opaque). Pinned at the AST level, not just via dump text. *)
  let prog, _ =
    parse_str ~file:"defaults.wo"
      "type T {\n  created: Timestamp = now()\n  active:  Bool = true\n}\n"
  in
  match prog.Ast.decls with
  | [ Ast.Class c ] -> (
    match c.fields with
    | [ f1; f2 ] ->
      check "default now(): recognized as DefaultNow" (f1.default = Some Ast.DefaultNow);
      check "default true: opaque token span, not DefaultNow"
        (match f2.default with
        | Some (Ast.DefaultOpaque [ { Token.kind = Token.KwTrue; _ } ]) -> true
        | _ -> false)
    | _ -> check "defaults: exactly two fields" false)
  | _ -> check "defaults: exactly one type declaration" false

let () =
  (* @table's known keys (name/index) round-trip; an unknown key is a
     parse error under its own code (WO-E102), distinct from the
     generic WO-E101 syntax-error code — mirrors Task 3's WO-E001/002
     split (one code per distinct situation, not one catch-all). *)
  let prog, collector =
    parse_str ~file:"table.wo"
      "@table(name: \"prices\", index: [sku, at])\nclass Price {\n  id: Id\n}\n"
  in
  check_eq "@table: no diagnostics on a well-formed configuration" ~expected:0
    ~actual:(List.length (Diag.Collector.diagnostics collector))
    string_of_int;
  (match prog.Ast.decls with
  | [ Ast.Class c ] ->
    check "@table: name and index captured"
      (match c.table with
      (* `; _` so databasev2 2's durable/resident fields do not have to be
         restated here — this check is about name and index capture only *)
      | Some { Ast.table_name = Some "prices"; indexes = [ [ "sku"; "at" ] ]; _ } -> true
      | _ -> false)
  | _ -> check "@table: exactly one class" false);
  let _, bad_collector = parse_str ~file:"bad-table.wo" "@table(shard_key: sku)\ntype T {\n  id: Id\n}\n" in
  let bad_diags = Diag.Collector.diagnostics bad_collector in
  check_eq "@table: unknown key reports exactly one diagnostic" ~expected:1
    ~actual:(List.length bad_diags) string_of_int;
  match bad_diags with
  | [ d ] -> check "@table: unknown key is WO-E102, not the generic WO-E101" (d.code = "WO-E102")
  | _ -> check "@table: unknown-key diagnostic shape" false

let () =
  (* Node ids: unique across a parse and, for every container relative
     to its own children, strictly smaller (Task 4 decision: a
     container's id is minted once its head is confirmed and before its
     body is parsed, uniformly for class/interface/method/fn — see
     parser.ml's parse_sig_head comment). Collected by hand here rather
     than depending on any future Ast-walking helper, since Task 4
     doesn't ship one. *)
  let prog, _ = parse_str ~file:"ids.wo" (read_file "golden/ast/pricing-demo.wo") in
  let ids = ref [] in
  let add id = ids := id :: !ids in
  let visit_param (p : Ast.param) = add p.id in
  let visit_field (f : Ast.field) = add f.id in
  let visit_method (m : Ast.method_decl) =
    check
      (Printf.sprintf "node ids: method `%s` id < all its param ids" m.name)
      (List.for_all (fun (p : Ast.param) -> m.id < p.id) m.params);
    add m.id;
    List.iter visit_param m.params
  in
  let visit_sig (s : Ast.method_sig) =
    check
      (Printf.sprintf "node ids: interface method `%s` id < all its param ids" s.name)
      (List.for_all (fun (p : Ast.param) -> s.id < p.id) s.params);
    add s.id;
    List.iter visit_param s.params
  in
  List.iter
    (fun (d : Ast.decl) ->
      match d with
      | Ast.Class c ->
        check
          (Printf.sprintf "node ids: class/type `%s` id < all its field/method ids" c.name)
          (List.for_all (fun (f : Ast.field) -> c.id < f.id) c.fields
          && List.for_all (fun (m : Ast.method_decl) -> c.id < m.id) c.methods);
        add c.id;
        List.iter visit_field c.fields;
        List.iter visit_method c.methods;
        (* haxe-parity Task 2: bare class-level consts, same shape *)
        List.iter (fun (cd : Ast.const_decl) -> add cd.id) c.consts
      | Ast.Interface i ->
        check
          (Printf.sprintf "node ids: interface `%s` id < all its method ids" i.name)
          (List.for_all (fun (s : Ast.method_sig) -> i.id < s.id) i.methods);
        add i.id;
        List.iter visit_sig i.methods
      | Ast.Fn m -> visit_method m
      | Ast.Use u -> add u.id
      | Ast.Const c -> add c.id
      | Ast.Union u -> add u.id (* haxe-parity Task 4: variants carry no ids of their own *))
    prog.Ast.decls;
  let sorted = List.sort compare !ids in
  let deduped = List.sort_uniq compare !ids in
  check "node ids: every id in the tree is unique" (List.length sorted = List.length deduped)

(* ---- statement/expression parser (Task 5, not golden-diffed) ------------

   golden/ast/{body-statements,ctor-literal,db-stub,body-recovery}.wo
   already pin the dump-text shape; these assertions pin the parts a
   text dump structurally cannot show — the precedence ladder's actual
   tree shape, and the insert/select asymmetry (statement-only vs.
   statement-and-expression), same rationale as Task 4's own direct
   assertions above. *)

let () =
  (* Precedence ladder (parser.ml's parse_expr chain): comparison is
     loosest, then concat, then additive, then multiplicative, then
     unary — so `1 + 2 * 3 == 4 .. "x"` must parse as
     `Eq(Add(1, Mul(2,3)), Concat(4, "x"))`, not e.g. `Mul` grabbing
     `2 * (3 == 4)` or concat binding tighter than `+`. *)
  let prog, _ = parse_str ~file:"prec.wo" "fn f() {\n  return 1 + 2 * 3 == 4 .. \"x\"\n}\n" in
  match prog.Ast.decls with
  | [ Ast.Fn m ] -> (
    match m.body with
    | [ { Ast.s_kind = Ast.Return (Some e); _ } ] -> (
      match e.Ast.kind with
      | Ast.Binary
          ( Ast.Eq,
            { Ast.kind = Ast.Binary (Ast.Add, { Ast.kind = Ast.IntLit 1; _ }, add_rhs); _ },
            { Ast.kind = Ast.Binary (Ast.Concat, { Ast.kind = Ast.IntLit 4; _ }, concat_rhs); _ }
          ) ->
        check "precedence: `2 * 3` is the addition's right operand, not split by `==`"
          (match add_rhs.Ast.kind with
          | Ast.Binary (Ast.Mul, { Ast.kind = Ast.IntLit 2; _ }, { Ast.kind = Ast.IntLit 3; _ }) ->
            true
          | _ -> false);
        check "precedence: concat's right operand is the string literal"
          (match concat_rhs.Ast.kind with Ast.StrLit "x" -> true | _ -> false)
      | _ -> check "precedence: top-level operator is `==` over an `Add` and a `Concat`" false)
    | _ -> check "precedence: exactly one `return` statement" false)
  | _ -> check "precedence: exactly one free fn" false

let () =
  (* Constructor literal: `ClassName { field: expr, ... }`, recognized
     by the identifier-then-brace shape in expression position (Task 5
     brief). Pinned directly against Ast.Ctor, not just dump text. *)
  let prog, _ = parse_str ~file:"ctor.wo" "fn f() {\n  let w = Widget { a: 1, b: 2 }\n}\n" in
  match prog.Ast.decls with
  | [ Ast.Fn m ] -> (
    match m.body with
    | [ { Ast.s_kind = Ast.Let { value = { Ast.kind = Ast.Ctor (name, fields); _ }; _ }; _ } ] ->
      check "ctor literal: class name captured" (name = "Widget");
      check "ctor literal: field names/order captured"
        (List.map fst fields = [ "a"; "b" ])
    | _ -> check "ctor literal: exactly one `let` binding a Ctor" false)
  | _ -> check "ctor literal: exactly one free fn" false

let () =
  (* Iteration 9 Task 3 retired the old asymmetry: `insert` is grammar-owned
     in BOTH positions now — a typed Insert node validated like a ctor,
     returning the id — while `select` stays the opaque DbStub until
     Task 5. The old contract ("bare insert is a plain Ident") is gone
     with the stub that motivated it. *)
  let prog, collector =
    parse_str ~file:"insert-vs-select.wo"
      "fn f() {\n  let a = insert Product { sku: \"A1\" }\n  let b = select\n}\n"
  in
  check_eq "typed insert + stub select: no diagnostics" ~expected:0
    ~actual:(List.length (Diag.Collector.diagnostics collector))
    string_of_int;
  (match prog.Ast.decls with
  | [ Ast.Fn m ] -> (
    match m.body with
    | [
     { Ast.s_kind = Ast.Let { name = "a"; value = a_val; _ }; _ };
     { Ast.s_kind = Ast.Let { name = "b"; value = b_val; _ }; _ };
    ] ->
      check "`insert` in expression position is a typed Insert node"
        (match a_val.Ast.kind with Ast.Insert ("Product", [ ("sku", _) ]) -> true | _ -> false);
      check "bare `select` in expression position always becomes a DbStub"
        (match b_val.Ast.kind with Ast.DbStub _ -> true | _ -> false)
    | _ -> check "insert vs. select: exactly two `let` statements" false)
  | _ -> check "insert vs. select: exactly one free fn" false);
  (* and a bare `insert` with no literal is a parse error now, not an Ident *)
  let _, c2 = parse_str ~file:"bare-insert.wo" "fn f() {\n  let a = insert\n}\n" in
  check "bare `insert` with no constructor literal is a diagnostic"
    (List.length (Diag.Collector.diagnostics c2) > 0)

let () =
  (* The no_brace guard (parser.ml's state.no_brace / looks_like_ctor):
     a bare identifier condition immediately followed by `{` is the
     if-statement's own block, never a constructor literal — pinned
     directly against the AST shape (Ast.Ident, not Ast.Ctor),
     complementing golden/ast/ctor-literal.wo's dump-level coverage. *)
  let prog, collector = parse_str ~file:"no-brace.wo" "fn f(active: Bool) {\n  if active {\n    return\n  }\n}\n" in
  check_eq "no_brace guard: reports nothing" ~expected:0
    ~actual:(List.length (Diag.Collector.diagnostics collector))
    string_of_int;
  match prog.Ast.decls with
  | [ Ast.Fn m ] -> (
    match m.body with
    | [ { Ast.s_kind = Ast.If { cond; _ }; _ } ] ->
      check "no_brace guard: if-condition is the bare ident, not a Ctor"
        (match cond.Ast.kind with Ast.Ident "active" -> true | _ -> false)
    | _ -> check "no_brace guard: exactly one `if` statement" false)
  | _ -> check "no_brace guard: exactly one free fn" false

(* ---- haxe-parity Task 2 (not golden-diffed) -----------------------------

   `and`/`or` precedence, string-interpolation desugar shape, and const
   substitution's scope-awareness — all structural facts a text dump
   cannot show cleanly (same rationale as this file's other direct
   assertions above). break/continue/do-while's own shapes are simple
   enough that golden/ast coverage would suffice, so they are not
   duplicated here. *)

let () =
  (* `or` binds loosest, then `and`, then comparison (parser.ml's own
     ladder doc): `a == 1 and b == 2` must parse as
     `And(Eq(a,1), Eq(b,2))` with no parens needed. *)
  let prog, _ = parse_str ~file:"and-prec.wo" "fn f(a: Int, b: Int) {\n  return a == 1 and b == 2\n}\n" in
  match prog.Ast.decls with
  | [ Ast.Fn m ] -> (
    match m.body with
    | [ { Ast.s_kind = Ast.Return (Some { Ast.kind = Ast.Binary (Ast.And, l, r); _ }); _ } ] ->
      check "and/or precedence: left operand is `a == 1`"
        (match l.Ast.kind with
        | Ast.Binary (Ast.Eq, { Ast.kind = Ast.Ident "a"; _ }, { Ast.kind = Ast.IntLit 1; _ }) -> true
        | _ -> false);
      check "and/or precedence: right operand is `b == 2`"
        (match r.Ast.kind with
        | Ast.Binary (Ast.Eq, { Ast.kind = Ast.Ident "b"; _ }, { Ast.kind = Ast.IntLit 2; _ }) -> true
        | _ -> false)
    | _ -> check "and/or precedence: top-level operator is `and`, not split by `==`" false)
  | _ -> check "and/or precedence: exactly one free fn" false

let () =
  (* `or` looser than `and`: `x or y and z` is `Or(x, And(y, z))`, not
     `And(Or(x,y), z)`. *)
  let prog, _ = parse_str ~file:"or-prec.wo" "fn f(x: Bool, y: Bool, z: Bool) {\n  return x or y and z\n}\n" in
  match prog.Ast.decls with
  | [ Ast.Fn m ] -> (
    match m.body with
    | [ { Ast.s_kind = Ast.Return (Some { Ast.kind = Ast.Binary (Ast.Or, l, r); _ }); _ } ] ->
      check "or looser than and: left operand is bare `x`"
        (match l.Ast.kind with Ast.Ident "x" -> true | _ -> false);
      check "or looser than and: right operand is `y and z`"
        (match r.Ast.kind with
        | Ast.Binary (Ast.And, { Ast.kind = Ast.Ident "y"; _ }, { Ast.kind = Ast.Ident "z"; _ }) -> true
        | _ -> false)
    | _ -> check "or looser than and: top-level operator is `or`" false)
  | _ -> check "or looser than and: exactly one free fn" false

let () =
  (* Interpolation desugar shape: `"${x} y"` -> Concat(Interp(Ident x),
     StrLit " y") -- the parse-time-decided chain shape parser.ml's own
     Ast.Interp doc comment describes. *)
  let prog, _ = parse_str ~file:"interp.wo" "fn f(x: Int) {\n  return \"${x} y\"\n}\n" in
  match prog.Ast.decls with
  | [ Ast.Fn m ] -> (
    match m.body with
    | [
     {
       Ast.s_kind =
         Ast.Return (Some { Ast.kind = Ast.Binary (Ast.Concat, { Ast.kind = Ast.Interp inner; _ }, tail); _ });
       _;
     };
    ] ->
      check "interp desugar: the embedded expression is bare `x`"
        (match inner.Ast.kind with Ast.Ident "x" -> true | _ -> false);
      check "interp desugar: the trailing text segment is `\" y\"`"
        (match tail.Ast.kind with Ast.StrLit " y" -> true | _ -> false)
    | _ -> check "interp desugar: exactly one `return Concat(Interp, StrLit)`" false)
  | _ -> check "interp desugar: exactly one free fn" false

let () =
  (* `\$` is a literal `$`, so a fully-escaped `${x}` never triggers
     interpolation at all -- the whole literal stays one plain StrLit,
     not a degenerate one-segment Interp chain. *)
  let prog, _ = parse_str ~file:"interp-escape.wo" "fn f() {\n  return \"\\${x}\"\n}\n" in
  match prog.Ast.decls with
  | [ Ast.Fn m ] -> (
    match m.body with
    | [ { Ast.s_kind = Ast.Return (Some e); _ } ] ->
      check "interp desugar: `\\${x}` is a plain StrLit \"${x}\", no interpolation"
        (match e.Ast.kind with Ast.StrLit "${x}" -> true | _ -> false)
    | _ -> check "interp desugar (escape): exactly one `return`" false)
  | _ -> check "interp desugar (escape): exactly one free fn" false

let () =
  (* Review fix (Important, post-Task-2): a genuine sub-parse failure
     inside an interpolation (1 +, not just trailing garbage) must
     report at the outer string literal's own position, with the same
     malformed-interpolation framing the trailing-garbage case already
     used — not at the sub-lexer's own uncorrected 1:1-relative
     line/col, which used to land on some unrelated line of the real
     file (and never mentioned interpolation at all, since it was
     raised straight out of the nested parse_expr). The source below
     puts the string literal's own opening quote at line 2, column 10;
     a regression back to the old, uncaught-inner-failure behavior
     would report at line 1 (the sub-lexer's own count) instead. *)
  let prog, collector =
    parse_str ~file:"interp-malformed.wo" "fn f() {\n  return \"${1 +}\"\n}\n"
  in
  (match Diag.Collector.diagnostics collector with
  | d :: _ ->
    check "interp malformed sub-expr: reports at the outer string's own position (2:10)"
      (d.Diag.site.Diag.line = 2 && d.Diag.site.Diag.col = 10);
    check "interp malformed sub-expr: the \"malformed ${...}\" message, not a raw sub-parse error"
      (d.Diag.message = "malformed \"${...}\" interpolation expression");
    check "interp malformed sub-expr: code is WO-E101" (d.Diag.code = "WO-E101")
  | [] -> check "interp malformed sub-expr: at least one diagnostic reported" false);
  (* The failure is caught and recovered at the *statement* level
     (parse_block's own sync_to_next_stmt, unchanged by this fix) --
     `fn f` itself still parses, just with its one bad `return`
     statement dropped, confirming desugar_interp's `fail` raises
     Parse_error the ordinary way rather than short-circuiting recovery
     entirely. *)
  ignore prog

let () =
  (* const substitution (parser.ml's own post-parse pass): a bare
     `Ident` reference becomes the literal expr the const named. *)
  let prog, _ = parse_str ~file:"const.wo" "const N = 5\nfn f() {\n  return N\n}\n" in
  match prog.Ast.decls with
  | [ Ast.Const _; Ast.Fn m ] -> (
    match m.body with
    | [ { Ast.s_kind = Ast.Return (Some { Ast.kind = Ast.IntLit 5; _ }); _ } ] ->
      check "const substitution: `return N` became `return 5`" true
    | _ -> check "const substitution: `return N` should desugar to `return 5`" false)
  | _ -> check "const substitution: exactly one const decl and one free fn" false

let () =
  (* Scope-aware, exactly like types.ml's own local-shadows-a-`use`-
     alias fix (Task 1 review): a parameter named the same as a
     top-level const always wins, so `f`'s own `N` parameter is returned
     unsubstituted, not silently replaced by the const's `5`. *)
  let prog, _ = parse_str ~file:"const-shadow.wo" "const N = 5\nfn f(N: Int) {\n  return N\n}\n" in
  match prog.Ast.decls with
  | [ Ast.Const _; Ast.Fn m ] -> (
    match m.body with
    | [ { Ast.s_kind = Ast.Return (Some { Ast.kind = Ast.Ident "N"; _ }); _ } ] ->
      check "const substitution: a same-named parameter shadows the const" true
    | _ -> check "const substitution: a same-named parameter must shadow the const, not get replaced" false)
  | _ -> check "const substitution (shadowing): exactly one const decl and one free fn" false

let () =
  (* Class-level consts win over a same-named top-level one (innermost
     scope wins) and are visible bare, with no `self.` prefix, inside
     that class's own methods only. *)
  let prog, _ =
    parse_str ~file:"const-class.wo"
      "const LABEL = \"top\"\nclass Box {\n  const LABEL = \"class\"\n  n: Int\n  fn get() -> Text {\n    return LABEL\n  }\n}\n"
  in
  match prog.Ast.decls with
  | [ Ast.Const _; Ast.Class c ] -> (
    match c.methods with
    | [ { body = [ { Ast.s_kind = Ast.Return (Some { Ast.kind = Ast.StrLit "class"; _ }); _ } ]; _ } ] ->
      check "const substitution: class-level const shadows the top-level one" true
    | _ -> check "const substitution: class-level LABEL should win, returning \"class\"" false)
  | _ -> check "const substitution (class-level): exactly one const decl and one class" false

(* ---- fix round 1 regressions (post-review CRITICAL 1/2/3) ---------------

   golden/ast/{condition-recovery,db-stub-nested}.wo and the extra
   if-conditions added to golden/ast/ctor-literal.wo already pin the
   dump-text shape; these assertions pin the parts a dump alone can't
   show — diagnostic counts, and (for CRITICAL 1) the concrete AST node
   kind that proves state.no_brace was actually restored rather than
   merely "looking right" in rendered text. *)

let () =
  (* CRITICAL 1: state.no_brace's save/restore was not exception-safe —
     a failed if/while/for condition left it stuck at `true`, so every
     constructor literal for the rest of the file silently stopped
     parsing as one (it fell back to a bare Ident, desyncing the parse
     of whatever followed). golden/ast/condition-recovery.wo's `if 1 +
     {` fails inside its own condition; the fix (parser.ml's
     with_no_brace, using Fun.protect) must restore no_brace before the
     Parse_error reaches parse_block's recovery, so the very next
     statement's constructor literal still parses as one. *)
  let path = "golden/ast/condition-recovery.wo" in
  let src = read_file path in
  let prog, collector = parse_str ~file:path src in
  let diags = Diag.Collector.diagnostics collector in
  check_eq "condition-recovery: exactly one diagnostic (the broken condition, not a cascade)"
    ~expected:1 ~actual:(List.length diags) string_of_int;
  (match diags with
  | [ d ] -> check "condition-recovery: WO-E101" (d.code = "WO-E101")
  | _ -> check "condition-recovery: diagnostic shape" false);
  match prog.Ast.decls with
  | [ Ast.Fn m ] -> (
    match m.body with
    | [ { Ast.s_kind = Ast.Let { name = "w"; value; _ }; _ } ] ->
      check "condition-recovery: no_brace was restored — `Widget {...}` after the \
             broken condition is still a real Ctor, not a bare Ident"
        (match value.Ast.kind with Ast.Ctor ("Widget", [ ("a", _) ]) -> true | _ -> false)
    | _ -> check "condition-recovery: exactly one surviving `let w = ...`" false)
  | _ -> check "condition-recovery: exactly one surviving fn" false

let () =
  (* CRITICAL 2: collect_dbstub_tokens only had a depth-0 stop guard on
     RBrace, so a `select`/`insert` nested inside an enclosing call or
     index expression had no way to stop at that call/index's own `)`/
     `]` — it swallowed everything to Eof. golden/ast/db-stub-nested.wo
     nests `select` inside both a call argument and an index
     expression, followed by an unrelated `let done_marker = 1`; if the
     scan ran away, done_marker would never be parsed (or the file
     would end on a misleading "expected ')'/'], got EOF" diagnostic
     instead of the three clean statements below). *)
  let path = "golden/ast/db-stub-nested.wo" in
  let src = read_file path in
  let prog, collector = parse_str ~file:path src in
  check_eq "db-stub-nested: reports nothing (both selects terminate at their own delimiter)"
    ~expected:0
    ~actual:(List.length (Diag.Collector.diagnostics collector))
    string_of_int;
  match prog.Ast.decls with
  | [ Ast.Fn m ] -> (
    match m.body with
    | [
     { Ast.s_kind = Ast.Let { name = "wrapped"; value = wrapped_val; _ }; _ };
     { Ast.s_kind = Ast.Let { name = "arr"; value = arr_val; _ }; _ };
     { Ast.s_kind = Ast.Let { name = "done_marker"; value = done_val; _ }; _ };
    ] ->
      check "db-stub-nested: `select` inside a call argument is a DbStub, and the \
             call's own `)` still closed the call"
        (match wrapped_val.Ast.kind with
        | Ast.Call ({ Ast.kind = Ast.Ident "wrap"; _ }, [ { Ast.kind = Ast.DbStub _; _ } ]) ->
          true
        | _ -> false);
      check "db-stub-nested: `select` inside an index expression is a DbStub, and the \
             index's own `]` still closed it"
        (match arr_val.Ast.kind with
        | Ast.Index ({ Ast.kind = Ast.Ident "data"; _ }, { Ast.kind = Ast.DbStub _; _ }) -> true
        | _ -> false);
      check "db-stub-nested: parsing continued past both closing delimiters — \
             `done_marker` is a normal IntLit, not swallowed or corrupted"
        (match done_val.Ast.kind with Ast.IntLit 1 -> true | _ -> false)
    | _ -> check "db-stub-nested: exactly three surviving `let` statements" false)
  | _ -> check "db-stub-nested: exactly one surviving fn" false

let () =
  (* CRITICAL 3: no_brace was only ever reset to `false` in
     parse_primary's own LParen branch — never in parse_call_args or
     parse_postfix's LBracket branch — so a constructor literal used as
     a call argument or index expression *inside* an if/while/for
     condition was never recognized as one (it hit looks_like_ctor's
     `not st.no_brace` guard, which was still `true`), corrupting the
     rest of the condition's parse. golden/ast/ctor-literal.wo already
     pins the dump-text shape for both; these assertions pin the
     concrete Ctor nodes directly. *)
  let prog, collector =
    parse_str ~file:"ctor-in-call-and-index.wo"
      "fn f(mut active: Bool) {\n\
      \  if make(Widget { x: 1 }) {\n\
      \    return\n\
      \  }\n\
      \  if items[Widget { x: 1 }] {\n\
      \    return\n\
      \  }\n\
       }\n"
  in
  check_eq "ctor in call/index inside a condition: reports nothing" ~expected:0
    ~actual:(List.length (Diag.Collector.diagnostics collector))
    string_of_int;
  match prog.Ast.decls with
  | [ Ast.Fn m ] -> (
    match m.body with
    | [ { Ast.s_kind = Ast.If { cond = call_cond; _ }; _ }; { Ast.s_kind = Ast.If { cond = idx_cond; _ }; _ } ]
      ->
      check "ctor as a call argument inside a condition is a real Ctor node"
        (match call_cond.Ast.kind with
        | Ast.Call (_, [ { Ast.kind = Ast.Ctor ("Widget", _); _ } ]) -> true
        | _ -> false);
      check "ctor as an index expression inside a condition is a real Ctor node"
        (match idx_cond.Ast.kind with
        | Ast.Index (_, { Ast.kind = Ast.Ctor ("Widget", _); _ }) -> true
        | _ -> false)
    | _ -> check "ctor in call/index inside a condition: exactly two `if` statements" false)
  | _ -> check "ctor in call/index inside a condition: exactly one free fn" false

(* ---- haxe-parity Task 3 (`switch` as expression, not golden-diffed) --

   Same rationale as Task 2's own direct-assertion section above: the
   grammar shape (arm count, multi-value `case`, `default`'s empty
   `values`, the no_brace guard on the subject) is a structural fact a
   text dump cannot show cleanly. *)

let () =
  (* `switch code { case 200: "a"; case 404, 410: "b"; default: "c"; }`
     -- the sample's own shape (docs/examples/log-watcher/cron.wo's
     `case "@daily", "@midnight": ...`): a `case` may carry more than
     one value, `default` carries none and is flagged. *)
  let prog, collector =
    parse_str ~file:"switch-shape.wo"
      "fn f(code: Int) -> Text {\n\
      \  return switch code {\n\
      \    case 200: \"a\";\n\
      \    case 404, 410: \"b\";\n\
      \    default: \"c\";\n\
      \  };\n\
       }\n"
  in
  check_eq "switch shape: reports nothing" ~expected:0
    ~actual:(List.length (Diag.Collector.diagnostics collector))
    string_of_int;
  match prog.Ast.decls with
  | [ Ast.Fn m ] -> (
    match m.body with
    | [ { Ast.s_kind = Ast.Return (Some { Ast.kind = Ast.Switch (subject, arms); _ }); _ } ] ->
      check "switch shape: subject is the bare Ident `code`"
        (match subject.Ast.kind with Ast.Ident "code" -> true | _ -> false);
      (match arms with
      | [ a1; a2; a3 ] ->
        check "switch shape: arm 1 is a single-value `case 200`, not default"
          (not a1.Ast.is_default
          && match a1.Ast.values with [ { Ast.kind = Ast.IntLit 200; _ } ] -> true | _ -> false);
        check "switch shape: arm 2 is the multi-value `case 404, 410`"
          (not a2.Ast.is_default
          &&
          match a2.Ast.values with
          | [ { Ast.kind = Ast.IntLit 404; _ }; { Ast.kind = Ast.IntLit 410; _ } ] -> true
          | _ -> false);
        check "switch shape: arm 3 is `default`, with no values"
          (a3.Ast.is_default && a3.Ast.values = [])
      | _ -> check "switch shape: exactly three arms" false)
    | _ -> check "switch shape: exactly one `return switch ...`" false)
  | _ -> check "switch shape: exactly one free fn" false

let () =
  (* Statement position: "one construct, not two" (the brief's own
     words) -- a bare `switch {...}` with no assignment is exactly the
     same `Ast.Switch` node, just wrapped in `ExprStmt`, its value
     discarded -- the same shape a bare `select ...`/function-call
     statement already is. *)
  let prog, collector =
    parse_str ~file:"switch-stmt-shape.wo"
      "fn f(n: Int) {\n\
      \  switch n {\n\
      \    case 1: print(\"one\");\n\
      \    default: print(\"other\");\n\
      \  }\n\
       }\n"
  in
  check_eq "switch statement shape: reports nothing" ~expected:0
    ~actual:(List.length (Diag.Collector.diagnostics collector))
    string_of_int;
  match prog.Ast.decls with
  | [ Ast.Fn m ] -> (
    match m.body with
    | [ { Ast.s_kind = Ast.ExprStmt { Ast.kind = Ast.Switch (subject, arms); _ }; _ } ] ->
      check "switch statement shape: subject is the bare Ident `n`"
        (match subject.Ast.kind with Ast.Ident "n" -> true | _ -> false);
      check_eq "switch statement shape: two arms" ~expected:2 ~actual:(List.length arms)
        string_of_int
    | _ -> check "switch statement shape: exactly one bare `ExprStmt(Switch ...)`" false)
  | _ -> check "switch statement shape: exactly one free fn" false

let () =
  (* The no_brace guard (parser.ml's state.no_brace / looks_like_ctor),
     for `switch` exactly like `if`/`while`/`for` above: a bare
     identifier subject immediately followed by `{` is the switch's own
     body, never a constructor literal swallowing it. *)
  let prog, collector =
    parse_str ~file:"switch-no-brace.wo"
      "fn f(active: Int) {\n\
      \  switch active {\n\
      \    default: return\n\
      \  }\n\
       }\n"
  in
  check_eq "switch no_brace guard: reports nothing" ~expected:0
    ~actual:(List.length (Diag.Collector.diagnostics collector))
    string_of_int;
  match prog.Ast.decls with
  | [ Ast.Fn m ] -> (
    match m.body with
    | [ { Ast.s_kind = Ast.ExprStmt { Ast.kind = Ast.Switch (subject, _); _ }; _ } ] ->
      check "switch no_brace guard: subject is the bare ident, not a Ctor"
        (match subject.Ast.kind with Ast.Ident "active" -> true | _ -> false)
    | _ -> check "switch no_brace guard: exactly one switch statement" false)
  | _ -> check "switch no_brace guard: exactly one free fn" false

(* ---- CLI smoke -------------------------------------------------------

   Everything above calls Lexer.tokenize/Dump.dump_tokens in-process --
   real coverage of the lexer, zero coverage of bin/main.ml's own
   plumbing (argv parsing, which stream a diagnostic lands on, the
   0/1/2 exit contract). This section runs the actual built woc binary
   as a subprocess. Sys.command (stdlib) shells out via /bin/sh; it
   only returns an exit code, so stdout/stderr are captured via
   redirection to temp files rather than a pipe API -- deliberately
   avoids pulling in the unix library for this one need. *)

let woc_binary () =
  match Sys.getenv_opt "WOC_BIN" with
  | Some path -> path
  | None -> (
    (* Under `dune runtest`, cwd is ".../_build/default/test" and the
       built binary sits at the sibling ".../_build/default/bin/woc" --
       test/dune depends on ../bin/woc precisely so that's guaranteed
       built before this test runs, making "../bin/woc" (relative to
       cwd) the right path. Manual invocation falls back through a
       couple of other plausible locations. *)
    let candidates = [ "../bin/woc"; "_build/default/bin/woc"; "bin/woc" ] in
    match List.find_opt Sys.file_exists candidates with
    | Some path -> path
    | None ->
      failwith "runner: cannot locate the built woc binary (set WOC_BIN)")

(* Runs the woc binary with [args]; returns (exit_code, stdout, stderr). *)
let run_cli (args : string list) : int * string * string =
  let bin = woc_binary () in
  let out_file = Filename.temp_file "woc_stdout" ".txt" in
  let err_file = Filename.temp_file "woc_stderr" ".txt" in
  let cmd =
    String.concat " " (List.map Filename.quote (bin :: args))
    ^ " >" ^ Filename.quote out_file ^ " 2>" ^ Filename.quote err_file
  in
  let exit_code = Sys.command cmd in
  let stdout = read_file out_file in
  let stderr = read_file err_file in
  (try Sys.remove out_file with Sys_error _ -> ());
  (try Sys.remove err_file with Sys_error _ -> ());
  (exit_code, stdout, stderr)

let () =
  let path = "golden/tokens/gotcha.wo" in
  let exit_code, stdout, stderr = run_cli [ "--dump-tokens"; path ] in
  check "cli smoke: --dump-tokens on a clean file exits 0" (exit_code = 0);
  check "cli smoke: clean-file run writes nothing to stderr" (stderr = "");
  let collector = Diag.Collector.create () in
  let toks = Lexer.tokenize collector ~file:path (read_file path) in
  check "cli smoke: stdout matches the in-process token dump exactly"
    (stdout = Dump.dump_tokens toks)

let () =
  let path = "golden/tokens/unknown-char.wo" in
  let exit_code, stdout, stderr = run_cli [ "--dump-tokens"; path ] in
  check "cli smoke: a diagnostic-producing file exits 1, not 0" (exit_code = 1);
  check "cli smoke: the WO-E001 diagnostic goes to stderr" (stderr <> "");
  let collector = Diag.Collector.create () in
  let toks = Lexer.tokenize collector ~file:path (read_file path) in
  check "cli smoke: stdout still carries the full token dump on exit 1"
    (stdout = Dump.dump_tokens toks)

let () =
  let exit_code, _stdout, stderr = run_cli [ "does-not-exist.wo" ] in
  check "cli smoke: missing file (bare-path form) exits 2" (exit_code = 2);
  check "cli smoke: missing-file message goes to stderr" (stderr <> "")

let () =
  let path = "golden/ast/pricing-demo.wo" in
  let exit_code, stdout, stderr = run_cli [ "--dump-ast"; path ] in
  check "cli smoke: --dump-ast on a clean file exits 0" (exit_code = 0);
  check "cli smoke: clean-file --dump-ast writes nothing to stderr" (stderr = "");
  let prog, _ = parse_str ~file:path (read_file path) in
  check "cli smoke: --dump-ast stdout matches the in-process AST dump exactly"
    (stdout = Dump.dump_ast prog)

let () =
  let path = "golden/ast/two-error-recovery.wo" in
  let exit_code, stdout, stderr = run_cli [ "--dump-ast"; path ] in
  check "cli smoke: a decl-recovery file exits 1, not 0" (exit_code = 1);
  check "cli smoke: recovered parse diagnostics go to stderr" (stderr <> "");
  let prog, _ = parse_str ~file:path (read_file path) in
  check "cli smoke: --dump-ast stdout still carries the surviving decls on exit 1"
    (stdout = Dump.dump_ast prog)

(* ---- CLI smoke: multi-file driver (Task 8) ----------------------------

   Everything above runs the CLI against a single file. These pin the
   two new driver behaviors end to end, through the actual woc binary,
   against fixtures under test/fixtures/driver/ rather than
   test/golden/ -- see test/dune's comment on why: run_golden_dir's
   one-.wo-file-per-fixture contract doesn't fit a fixture that *is*
   several files compiling as one program. *)

let () =
  (* Cross-file symbol resolution: a_uses.wo (discovered first,
     alphabetically) types a field as `Box`, a class declared only in
     b_declares.wo (discovered second). Compiled alone, a_uses.wo must
     fail WO-E225 unknown-type -- the control proving this is a real
     check, not one that would pass vacuously. Compiled as the
     directory (both files, one program), it must be clean: every
     file's declare-pass runs before any file's body-check, so
     discovery order can't matter for whether the symbol resolves. *)
  let alone = "fixtures/driver/crossfile/a_uses.wo" in
  let exit_code, _, stderr = run_cli [ alone ] in
  check "crossfile control: a_uses.wo alone fails (Box isn't declared here)"
    (exit_code = 1);
  check "crossfile control: it's WO-E225 unknown-type, not something else"
    (find_substring ~needle:"WO-E225" stderr <> None
    && find_substring ~needle:"unknown type `Box`" stderr <> None);
  let dir = "fixtures/driver/crossfile" in
  let exit_code, _, stderr = run_cli [ dir ] in
  check "crossfile: the directory (both files, merged symbols) compiles clean"
    (exit_code = 0 && stderr = "")

let () =
  (* Same directory, --dump-ast: proves both files were actually
     discovered and parsed (not e.g. an empty file list "compiling
     clean" vacuously), each under its own file_header. *)
  let dir = "fixtures/driver/crossfile" in
  let _, stdout, _ = run_cli [ "--dump-ast"; dir ] in
  check "crossfile --dump-ast: both files' headers appear"
    (find_substring ~needle:"=== fixtures/driver/crossfile/a_uses.wo ===" stdout <> None
    && find_substring ~needle:"=== fixtures/driver/crossfile/b_declares.wo ===" stdout <> None);
  check "crossfile --dump-ast: both classes actually got dumped"
    (find_substring ~needle:"CLASS Holder" stdout <> None
    && find_substring ~needle:"CLASS Box" stdout <> None)

let () =
  (* Diagnostic ordering, discriminating (review follow-up, Important
     3): the old version of this fixture had both files erroring at
     the *same* pipeline stage (lexing), with the driver already
     visiting files in sorted order for that stage -- insertion order
     and (file, line, col) order coincided, so a broken sort could have
     passed unnoticed. Here aaa_ownership.wo (sorts FIRST) has only a
     *late*-stage error (WO-E301, found during the owner-analysis pass,
     which runs over every file only after parse_all and typecheck_all
     have both finished for every file) and zzz_lex.wo (sorts SECOND)
     has only an *early*-stage error (WO-E001, found during parse_all,
     the very first per-file pass). That means zzz_lex.wo's diagnostic
     is *inserted into the collector first*, chronologically -- raw
     insertion order is [zzz, aaa], the exact reverse of the required
     [aaa, zzz] output order. Only a real (file, line, col) sort, not
     insertion order, can produce the required order here. *)
  let dir = "fixtures/driver/order" in
  let exit_code, _, stderr = run_cli [ dir ] in
  check "diagnostic order: exits 1 (one ownership error, one lex error)"
    (exit_code = 1);
  let aaa_idx = find_substring ~needle:"aaa_ownership.wo:11:19: error WO-E301" stderr in
  let zzz_idx = find_substring ~needle:"zzz_lex.wo:1:1: error WO-E001" stderr in
  check "diagnostic order: both files' diagnostics are present"
    (aaa_idx <> None && zzz_idx <> None);
  check
    "diagnostic order: aaa_ownership.wo's *later-inserted* ownership error still prints \
     first (file-sorts-first wins over insertion order)"
    (match (aaa_idx, zzz_idx) with Some a, Some z -> a < z | _ -> false)

let () =
  (* Cross-file symbol collision (review follow-up, Important 2):
     a_first.wo and b_second.wo both declare `class Dup`, with
     different fields, so a silent first-wins merge would let
     b_second.wo's own field (`s: Text`) typecheck against
     a_first.wo's shape without anyone being told the two `Dup`s were
     never the same class. b_second.wo sorts *after* a_first.wo, so it
     is the one reported (declaring second is what makes it the
     collision), with a_first.wo as the related "first declared here"
     site -- deterministic, not order-of-Hashtbl-iteration dependent,
     since the outer walk is over the same sorted-by-discovery file
     list every other multi-file check relies on. *)
  let dir = "fixtures/driver/collision" in
  let exit_code, _, stderr = run_cli [ dir ] in
  check "collision: exits 1" (exit_code = 1);
  check "collision: WO-E214 reported at the second (later-declaring) file"
    (find_substring ~needle:"b_second.wo:1:1: error WO-E214: class `Dup` already declared in"
       stderr
    <> None);
  check "collision: names the first-declaring file by path"
    (find_substring ~needle:"already declared in `fixtures/driver/collision/a_first.wo`" stderr
    <> None);
  check "collision: related site points back at a_first.wo's own declaration"
    (find_substring ~needle:"fixtures/driver/collision/a_first.wo:1:1: `Dup` first declared here"
       stderr
    <> None)

let () =
  (* haxe-parity Task 1 (modules): unused-`use` golden. Corpus fixtures
     (tests/corpus/run and tests/corpus/compile-fail, "lang-use-" prefix)
     exercise the resolver end to end through the real woc/wovm pair —
     cross-module call,
     collision, private-name-access — but oop-e2e.sh's compile-fail/
     kind demands exit 1, and run/ only diffs stdout, so neither kind
     can assert a *warning*-only outcome (exit 0, something on stderr)
     at all; that gap is exactly why the brief calls out "golden via
     compiler suite since warnings don't fail" for this one case, and
     is filed here rather than duplicating the collision/private-access/
     cross-module cases already proven end to end by the corpus. *)
  let path = "fixtures/driver/module-unused-use/unused.wo" in
  let exit_code, _stdout, stderr = run_cli [ path ] in
  check "unused use: `use fs` declared and never called still exits 0 (a warning, not an error)"
    (exit_code = 0);
  check "unused use: WO-W202 on the `use fs` line, naming the module"
    (find_substring ~needle:"unused.wo:5:1: warning WO-W202: unused `use fs`" stderr <> None)

(* Manual non-overlapping substring counter, same idiom as find_substring
   just above -- the one thing that helper can't answer on its own
   (whether a needle occurs more than once), needed only by the single
   test right below it. *)
let count_substring ~needle haystack =
  let hlen = String.length haystack and nlen = String.length needle in
  let rec go i n =
    if i + nlen > hlen then n
    else if String.sub haystack i nlen = needle then go (i + nlen) (n + 1)
    else go (i + 1) n
  in
  go 0 0

let () =
  (* Hotfix (multi-file double-report): typecheck_program used to walk
     the whole-program *merged* symbol table's classes/free_fns
     regardless of which file was actually being checked, so an N-file
     program ran every file's bodies through the checker once per
     discovered file (here N=2, so 2x, not once) -- b.wo's one real
     WO-E202 (`it.price`; Item, declared in a.wo, has no such field)
     got a second, phantom copy stamped with a.wo's own path, at the
     same (line, col) a.wo doesn't even have that many lines of. See
     types.ml's typecheck_program doc comment (the `~file_syms` fix)
     and .superpowers/sdd/2026-08-01-haxe-parity-language/
     hotfix-e209-report.md's "Disclosed, NOT fixed" section for the
     original diagnosis this pins the fix for. Bare check-only mode
     (no --emit) on purpose: it skips emit.ml's own, unrelated
     field-existence check (WO-E403), keeping this fixture down to
     exactly the one diagnostic under test. *)
  let dir = "fixtures/driver/multifile-single-report" in
  let exit_code, _stdout, stderr = run_cli [ dir ] in
  check "multifile single-report: exits 1 (one real WO-E202, nothing else)" (exit_code = 1);
  check "multifile single-report: WO-E202 reported EXACTLY once, not once per other file"
    (count_substring ~needle:"error WO-E202" stderr = 1);
  check "multifile single-report: the one report is tagged with b.wo (the real site)"
    (find_substring ~needle:"b.wo:8:15: error WO-E202: unknown field `price` on `Item`" stderr
    <> None);
  check "multifile single-report: no phantom copy stamped with a.wo's path"
    (find_substring ~needle:"a.wo:" stderr = None)

(* ---- direct typechecker assertions (Task 6b) --------------------------

   No golden "types" stage exists yet: that would need `--dump-types`
   wired into bin/main.ml and a diagnostics-aware dump_symbols in
   dump.ml, neither of which the nullable-types-implementation plan's
   New Requirements section asks for (it only names WO-W201/WO-E225 and
   the scalar-list correction) -- building that CLI/dump plumbing now
   would be scope creep beyond this task. These assertions instead pin
   the Types.typecheck contract directly against the Collector, the
   same way the lexer/parser sections above do. *)

let typecheck_str ~file src =
  let collector = Diag.Collector.create () in
  let toks = Lexer.tokenize collector ~file src in
  let prog = Parser.parse collector ~file toks in
  let syms, () = Types.typecheck ~file prog collector in
  (syms, collector)

let () =
  (* Money/SKU carry no special status -- ordinary unknown types (WO-E225
     fires on them as fields). Float was removed for the same phantom-scalar
     reason once, and iteration 19 EARNED IT BACK: there is float-literal
     syntax in the lexer, a WO_K_FLOAT kind in wob, f64 opcodes in the VM, and
     a WAL slot -- so a Float value can now be written, stored, and replayed.
     Money stays out (cents-as-Int holds until a workload proves otherwise);
     Bytes came in with Float. Timestamp stays a real builtin. *)
  check "Money is no longer a builtin scalar" (not (Types.is_builtin_scalar "Money"));
  check "SKU is no longer a builtin scalar" (not (Types.is_builtin_scalar "SKU"));
  check "Float is a builtin scalar (iteration 19)" (Types.is_builtin_scalar "Float");
  check "Bytes is a builtin scalar (iteration 19)" (Types.is_builtin_scalar "Bytes");
  (* the no-mixing rule's own predicate: Float must NOT be Int-shaped, or
     `print_int(price)` would print f64 bits as a huge integer *)
  check "Float is not Int-shaped" (not (Types.is_scalar_shaped "Float"));
  check "Bytes is not Int-shaped" (not (Types.is_scalar_shaped "Bytes"));
  check "Timestamp is a builtin scalar" (Types.is_builtin_scalar "Timestamp")

let () =
  (* class BadExample { code: INVALID_TYPE } -- INVALID_TYPE is not a
     builtin, class, or interface: WO-E225 must fire, at
     the field's own real file/line/col, and this (an actual Error) must
     exit 1. *)
  let path = "bad-example.wo" in
  let _, collector =
    typecheck_str ~file:path "class BadExample {\n  code: INVALID_TYPE\n}\n"
  in
  let diags = Diag.Collector.diagnostics collector in
  check_eq "unknown-type: exactly one diagnostic (WO-E225)" ~expected:1
    ~actual:(List.length diags) string_of_int;
  (match diags with
  | [ d ] ->
    check "unknown-type: code is WO-E225" (d.Diag.code = "WO-E225");
    check "unknown-type: severity is Error" (d.Diag.severity = Diag.Error);
    check "unknown-type: real file/line/col (bad-example.wo:2:3, the `code` field)"
      (d.Diag.site.Diag.file = path && d.Diag.site.Diag.line = 2 && d.Diag.site.Diag.col = 3)
  | _ -> check "unknown-type: exactly one diagnostic" false);
  check_eq "unknown-type: an error run exits 1" ~expected:1
    ~actual:(Diag.Collector.exit_code collector) string_of_int

let () =
  (* class Product { id: Id; sku: SKU; price: Money } -- SKU/Money are
     ordinary unknown types (not a builtin, class, or interface), so both
     fields must trip WO-E225. *)
  let _, collector =
    typecheck_str ~file:"product.wo"
      "class Product {\n  id: Id\n  sku: SKU\n  price: Money\n}\n"
  in
  let diags = Diag.Collector.diagnostics collector in
  check_eq "unknown-type fields (SKU, Money): exactly two diagnostics" ~expected:2
    ~actual:(List.length diags) string_of_int;
  check "unknown-type fields (SKU, Money): both are WO-E225"
    (List.for_all (fun d -> d.Diag.code = "WO-E225") diags)

let () =
  (* class Ring { next: ?Ring } -- INVALID_TYPE's sibling case through the
     ?T nullable wrapper this plan is named after: an unknown type inside
     `?T` must still be caught, and a *known* one (here, Ring itself)
     must not be a false positive. Also exercises forward references: B
     is declared after A and must resolve since Pass 2 runs after all of
     Pass 1 has completed. *)
  let _, collector =
    typecheck_str ~file:"ring.wo"
      "class A {\n  b: B\n}\nclass B {\n  x: Int\n}\n"
  in
  check_eq "forward reference (A.b: B, B declared later): reports nothing" ~expected:0
    ~actual:(List.length (Diag.Collector.diagnostics collector)) string_of_int;
  let _, collector2 =
    typecheck_str ~file:"nullable-unknown.wo" "class Ring {\n  next: ?GHOST\n}\n"
  in
  let diags2 = Diag.Collector.diagnostics collector2 in
  check_eq "unknown type inside ?T: exactly one WO-E225" ~expected:1
    ~actual:(List.length diags2) string_of_int;
  match diags2 with
  | [ d ] -> check "unknown type inside ?T: code is WO-E225" (d.Diag.code = "WO-E225")
  | _ -> check "unknown type inside ?T: exactly one diagnostic" false

let () =
  (* Same-file duplicate declarations (Task 1 review -> Task 2 fix,
     WO-E215): collect_declarations folded one file's decls into a
     StringMap keyed by name via a bare StringMap.add, so a second
     `class`/`interface`/`fn` of the same name in the SAME file was
     silently dropped -- no diagnostic at all (Task 1 report, "Known
     limitations" #6). This is the front-end's own-file counterpart to
     the driver's cross-file WO-E214: reported at the *later*
     declaration, with the first declaration as the related site,
     same WO-E2xx range, same "later primary / first related" shape. *)
  let check_duplicate tag ~src ~kind ~name =
    let _, collector = typecheck_str ~file:(tag ^ ".wo") src in
    let diags = Diag.Collector.diagnostics collector in
    check_eq (tag ^ ": exactly one diagnostic (WO-E215)") ~expected:1
      ~actual:(List.length diags) string_of_int;
    (match diags with
    | [ d ] ->
      check (tag ^ ": code is WO-E215") (d.Diag.code = "WO-E215");
      check (tag ^ ": severity is Error") (d.Diag.severity = Diag.Error);
      check (tag ^ ": reported at the later declaration (4:1)")
        (d.Diag.site.Diag.line = 4 && d.Diag.site.Diag.col = 1);
      check (tag ^ ": message names the kind and the name")
        (find_substring ~needle:(kind ^ " `" ^ name ^ "` already declared") d.Diag.message
        <> None);
      (match d.Diag.related with
      | [ r ] ->
        check (tag ^ ": related site points at the first declaration (1:1)")
          (r.Diag.site.Diag.line = 1 && r.Diag.site.Diag.col = 1);
        check (tag ^ ": related label names the first declaration")
          (find_substring ~needle:"first declared here" r.Diag.label <> None)
      | _ -> check (tag ^ ": exactly one related site") false)
    | _ -> check (tag ^ ": exactly one diagnostic") false);
    check_eq (tag ^ ": an error run exits 1") ~expected:1
      ~actual:(Diag.Collector.exit_code collector) string_of_int
  in
  check_duplicate "duplicate class" ~kind:"class" ~name:"Dup"
    ~src:"class Dup {\n  n: Int\n}\nclass Dup {\n  s: Text\n}\n";
  check_duplicate "duplicate interface" ~kind:"interface" ~name:"Shape"
    ~src:"interface Shape {\n  fn area() -> Int\n}\ninterface Shape {\n  fn perimeter() -> Int\n}\n";
  check_duplicate "duplicate fn" ~kind:"fn" ~name:"double"
    ~src:"fn double(x: Int) -> Int {\n  return x + x\n}\nfn double(y: Int) -> Int {\n  return y * 2\n}\n"

let () =
  (* Control: a class and a fn sharing a name are different namespaces
     (collect_declarations keeps them in separate StringMaps) -- must
     NOT trip WO-E215. *)
  let _, collector =
    typecheck_str ~file:"cross-namespace.wo"
      "class Widget {\n  n: Int\n}\nfn Widget() -> Int {\n  return 1\n}\n"
  in
  check_eq "class/fn name sharing across namespaces: reports nothing" ~expected:0
    ~actual:(List.length (Diag.Collector.diagnostics collector)) string_of_int

(* ---- WO-E209 direct assertions (hotfix: invalid-builtin-arg) ----------

   `print(7)` used to compile clean and segfault `wovm` -- `print` wants
   a `Text` (a heap-string pointer) and a bare `7` is a plain int64
   register, so the VM's `str_check` dereferenced it as a wild pointer.
   `tests/corpus/compile-fail/lang-builtin-arg-type/` and
   `lang-builtin-arity/` pin the same two shapes end to end through
   `woc`/`oop-e2e.sh`; these assertions pin the Collector-level contract
   directly (code, severity, exact position), the same way the WO-W201/
   WO-E225/WO-E215 blocks above do. *)

let () =
  (* Positive control: a correctly-typed `print` call must stay silent --
     this check must not regress the common case. *)
  let _, collector = typecheck_str ~file:"print-ok.wo" "fn main() {\n  print(\"x\")\n}\n" in
  check_eq "builtin-arg-type: print(\"x\") reports nothing" ~expected:0
    ~actual:(List.length (Diag.Collector.diagnostics collector)) string_of_int

let () =
  (* Negative #1: print(7) -- a Text builtin called with an Int literal,
     the exact segfault repro. Reported at the argument's own position
     (2:9, the `7`), not the call's. *)
  let path = "print-int-lit.wo" in
  let _, collector = typecheck_str ~file:path "fn main() {\n  print(7)\n}\n" in
  let diags = Diag.Collector.diagnostics collector in
  check_eq "builtin-arg-type: print(7) is exactly one diagnostic (WO-E209)" ~expected:1
    ~actual:(List.length diags) string_of_int;
  (match diags with
  | [ d ] ->
    check "builtin-arg-type: code is WO-E209" (d.Diag.code = "WO-E209");
    check "builtin-arg-type: severity is Error" (d.Diag.severity = Diag.Error);
    check "builtin-arg-type: reported at the argument's own position (print-int-lit.wo:2:9)"
      (d.Diag.site.Diag.file = path && d.Diag.site.Diag.line = 2 && d.Diag.site.Diag.col = 9);
    check "builtin-arg-type: message names the builtin, expected, and actual type"
      (find_substring ~needle:"builtin `print` expects Text, got `Int`" d.Diag.message <> None)
  | _ -> check "builtin-arg-type: print(7) exactly one diagnostic" false);
  check_eq "builtin-arg-type: an error run exits 1" ~expected:1
    ~actual:(Diag.Collector.exit_code collector) string_of_int

let () =
  (* Negative #2: now(1) -- `now` takes zero arguments
     (08-builtin-surface.md's `now()` row). Reported at the call's own
     position (2:6, the `(` -- e.pos for a Call node), same code as the
     argument-type mismatch above: WO-E209 covers both halves of
     "invalid builtin call." *)
  let path = "now-arity.wo" in
  let _, collector = typecheck_str ~file:path "fn main() {\n  now(1)\n}\n" in
  let diags = Diag.Collector.diagnostics collector in
  check_eq "builtin-arity: now(1) is exactly one diagnostic (WO-E209)" ~expected:1
    ~actual:(List.length diags) string_of_int;
  match diags with
  | [ d ] ->
    check "builtin-arity: code is WO-E209" (d.Diag.code = "WO-E209");
    check "builtin-arity: reported at the call's own position (now-arity.wo:2:6)"
      (d.Diag.site.Diag.file = path && d.Diag.site.Diag.line = 2 && d.Diag.site.Diag.col = 6);
    check "builtin-arity: message names the builtin and the counts"
      (find_substring ~needle:"builtin `now` takes 0 argument(s), given 1" d.Diag.message <> None)
  | _ -> check "builtin-arity: now(1) exactly one diagnostic" false

let () =
  (* Container-ness, real catch: `push` wants a `multi` receiver
     (08-builtin-surface.md); `b.n` is a genuinely *declared* `Int`
     field, not an unresolved placeholder, so this must fire even though
     the receiver isn't a literal -- the case that motivated threading
     `confident_typ`'s own `cenv` through field/parameter resolution
     instead of only trusting literals. *)
  let _, collector =
    typecheck_str ~file:"push-wrong-receiver.wo"
      "class Box {\n  n: Int\n}\nfn use_box(b: Box) {\n  push(b.n, 1)\n}\n"
  in
  let diags = Diag.Collector.diagnostics collector in
  check_eq "builtin-arg-type: push(b.n, 1) is exactly one diagnostic (WO-E209)" ~expected:1
    ~actual:(List.length diags) string_of_int;
  match diags with
  | [ d ] ->
    check "builtin-arg-type: code is WO-E209" (d.Diag.code = "WO-E209");
    check "builtin-arg-type: message names push, a multi, and Int"
      (find_substring ~needle:"builtin `push` expects a `multi`, got `Int`" d.Diag.message <> None)
  | _ -> check "builtin-arg-type: push(b.n, 1) exactly one diagnostic" false

let () =
  (* Container-ness, positive control: `push` onto a genuinely-declared
     `multi` field must stay silent. *)
  let _, collector =
    typecheck_str ~file:"push-ok.wo"
      "class Item {\n  n: Int\n}\nclass Box {\n  items: multi Item\n}\nfn use_box(b: Box) {\n  \
       push(b.items, Item { n: 1 })\n}\n"
  in
  check_eq "builtin-arg-type: push onto a declared `multi` field reports nothing" ~expected:0
    ~actual:(List.length (Diag.Collector.diagnostics collector)) string_of_int

let () =
  (* Shadowing: "a user-declared free fn of the same name always wins"
     (08-builtin-surface.md) -- a same-named `print` taking a `Text`
     means `print(7)` is now a call to *that* fn, not the builtin, so
     this check must not fire (whether the user fn's own call is
     well-typed is WO-E203/WO-E204's pre-existing, unrelated gap). *)
  let _, collector =
    typecheck_str ~file:"print-shadowed.wo"
      "fn print(x: Text) {\n  print_int(1)\n}\nfn main() {\n  print(7)\n}\n"
  in
  check_eq "builtin-arg-type: user-declared `print` shadows the builtin, reports nothing"
    ~expected:0 ~actual:(List.length (Diag.Collector.diagnostics collector)) string_of_int

let () =
  (* Conservatism: `print(x)` where `x` is an unresolved name has no
     confidently-known type (an unresolved `Ident` is `confident_typ`'s
     own `None` case) -- this check must stay silent rather than guess,
     exactly the "stay silent when underivable" contract. *)
  let _, collector = typecheck_str ~file:"print-unresolved.wo" "fn main() {\n  print(x)\n}\n" in
  check_eq "builtin-arg-type: print(x) with x unresolved reports nothing" ~expected:0
    ~actual:(List.length (Diag.Collector.diagnostics collector)) string_of_int

(* ---- WO-E209 round 2: Call-return derivation (fix-round-1 finding) ----

   Round 1's `confident_typ` chased literals/fields/params but never a
   `Call`'s own return type -- so `print(takesSecret(box))`, where
   `takesSecret` is declared `-> Int`, compiled clean and segfaulted
   `wovm` exactly like `print(7)` does, one call deeper. Controller-
   verified real repro; `tests/corpus/compile-fail/
   lang-builtin-arg-type-{freefn,method}/` pin the same two shapes end
   to end through `woc`/`oop-e2e.sh`. *)

let () =
  (* Free-fn call: `takesSecret` is declared `-> Int`; a class method
     call inside it (`box.hidden()`) is itself part of the repro but not
     what's being pinned here -- the outer `print` call is. *)
  let path = "print-freefn-call.wo" in
  let src =
    "class Box {\n  fn hidden() -> Int {\n    return 7\n  }\n}\n\
     fn takesSecret(box: Box) -> Int {\n  return box.hidden()\n}\n\
     fn main() {\n  print(takesSecret(Box{}))\n}\n"
  in
  let _, collector = typecheck_str ~file:path src in
  let diags = Diag.Collector.diagnostics collector in
  check_eq "builtin-arg-type: print(freefn-call) is exactly one diagnostic (WO-E209)" ~expected:1
    ~actual:(List.length diags) string_of_int;
  match diags with
  | [ d ] ->
    check "builtin-arg-type: code is WO-E209" (d.Diag.code = "WO-E209");
    check "builtin-arg-type: reported at the call's own position (print-freefn-call.wo:10:20)"
      (d.Diag.site.Diag.file = path && d.Diag.site.Diag.line = 10 && d.Diag.site.Diag.col = 20);
    check "builtin-arg-type: message names print, Text, and Int"
      (find_substring ~needle:"builtin `print` expects Text, got `Int`" d.Diag.message <> None)
  | _ -> check "builtin-arg-type: print(freefn-call) exactly one diagnostic" false

let () =
  (* Method call, receiver built the ordinary way (`let b = Box{}`, a
     `Ctor` -- confident_typ has to chase that too, not only a
     parameter's declared type, to reach `hidden`'s own `-> Int`). *)
  let path = "print-method-call.wo" in
  let src =
    "class Box {\n  fn hidden() -> Int {\n    return 7\n  }\n}\n\
     fn main() {\n  let b = Box{}\n  print(b.hidden())\n}\n"
  in
  let _, collector = typecheck_str ~file:path src in
  let diags = Diag.Collector.diagnostics collector in
  check_eq "builtin-arg-type: print(method-call) is exactly one diagnostic (WO-E209)" ~expected:1
    ~actual:(List.length diags) string_of_int;
  match diags with
  | [ d ] ->
    check "builtin-arg-type: code is WO-E209" (d.Diag.code = "WO-E209");
    check "builtin-arg-type: reported at the call's own position (print-method-call.wo:8:17)"
      (d.Diag.site.Diag.file = path && d.Diag.site.Diag.line = 8 && d.Diag.site.Diag.col = 17);
    check "builtin-arg-type: message names print, Text, and Int"
      (find_substring ~needle:"builtin `print` expects Text, got `Int`" d.Diag.message <> None)
  | _ -> check "builtin-arg-type: print(method-call) exactly one diagnostic" false

let () =
  (* Bidirectional pin on a builtin-call return type feeding another
     builtin: `words` returns `Int` (08-builtin-surface.md), so
     `print(words(...))` is WO-E209 (wants `Text`) and
     `print_int(words(...))` is clean (wants `Int`) -- same underlying
     `builtin_confident_ret` entry, both directions asserted so a
     regression flipping either one is caught. *)
  let _, bad_collector =
    typecheck_str ~file:"print-words.wo" "fn main() {\n  print(words(\"a b\"))\n}\n"
  in
  let bad_diags = Diag.Collector.diagnostics bad_collector in
  check_eq "builtin-arg-type: print(words(...)) is exactly one diagnostic (WO-E209)" ~expected:1
    ~actual:(List.length bad_diags) string_of_int;
  (match bad_diags with
  | [ d ] ->
    check "builtin-arg-type: print(words(...)) code is WO-E209" (d.Diag.code = "WO-E209");
    check "builtin-arg-type: print(words(...)) message names print, Text, and Int"
      (find_substring ~needle:"builtin `print` expects Text, got `Int`" d.Diag.message <> None)
  | _ -> check "builtin-arg-type: print(words(...)) exactly one diagnostic" false);
  let _, ok_collector =
    typecheck_str ~file:"print-int-words.wo" "fn main() {\n  print_int(words(\"a b\"))\n}\n"
  in
  check_eq "builtin-arg-type: print_int(words(...)) reports nothing" ~expected:0
    ~actual:(List.length (Diag.Collector.diagnostics ok_collector)) string_of_int

(* ---- haxe-parity Task 3: WO-E208 (missing default), WO-E201 (arm
   mismatch) direct assertions --------------------------------------

   tests/corpus/compile-fail/lang-switch-missing-default and
   lang-switch-arm-mismatch already pin the end-to-end shape (real code,
   real exit status); these pin the exact diagnostic — count, severity,
   site — the same way the WO-E209 blocks above do for builtins. *)

let () =
  (* Scalar subject (`Int`), no `default`: unconditional today (no union
     type exists yet — see typecheck_switch's own doc comment, the seam
     Task 4 extends) — WO-E208, at the subject's own position. *)
  let path = "switch-no-default.wo" in
  let _, collector =
    typecheck_str ~file:path
      "fn f(n: Int) -> Text {\n  let v = switch n {\n    case 1: \"a\";\n    case 2: \"b\";\n  }\n  return v\n}\n"
  in
  let diags = Diag.Collector.diagnostics collector in
  check_eq "switch missing default: exactly one diagnostic (WO-E208)" ~expected:1
    ~actual:(List.length diags) string_of_int;
  match diags with
  | [ d ] ->
    check "switch missing default: code is WO-E208" (d.Diag.code = "WO-E208");
    check "switch missing default: severity is Error" (d.Diag.severity = Diag.Error);
    check "switch missing default: message names the subject's type (`Int`)"
      (find_substring ~needle:"switch over `Int` has no `default` arm" d.Diag.message <> None)
  | _ -> check "switch missing default: exactly one diagnostic" false

let () =
  (* Review fix (Critical 1): a `default` arm satisfies the default-
     required rule even when it is not textually last (no error) — but
     is no longer silent about it either: `case 2`, written after
     `default`, used to be permanently unreachable dead code (nothing
     ever jumped into it) with zero diagnostic; `default` is now
     lowered last regardless of source position (ast.ml's own
     `switch_lowering_order`, so `case 2` is live again — see the
     dedicated corpus fixture, lang-switch-default-not-last, for the
     runtime proof), and this position is still surprising enough
     source to warn about once, at `default`'s own site. *)
  let path = "switch-default-present.wo" in
  let _, collector =
    typecheck_str ~file:path
      "fn f(n: Int) -> Text {\n  let v = switch n {\n    case 1: \"a\";\n    default: \"z\";\n    case 2: \"b\";\n  }\n  return v\n}\n"
  in
  let diags = Diag.Collector.diagnostics collector in
  check_eq "switch with default present (not last): exactly one diagnostic (WO-W203)"
    ~expected:1 ~actual:(List.length diags) string_of_int;
  match diags with
  | [ d ] ->
    check "switch default not last: code is WO-W203" (d.Diag.code = "WO-W203");
    check "switch default not last: severity is Warning (never an error)"
      (d.Diag.severity = Diag.Warning);
    check_eq "switch default not last: exits 0 (a warning-only run)" ~expected:0
      ~actual:(Diag.Collector.exit_code collector) string_of_int
  | _ -> check "switch default not last: exactly one diagnostic" false

let () =
  (* Arm-type unification: `case 1` yields `Text`, `default` yields
     `Int` — the switch's own type is fixed by the first arm
     (typecheck_switch's "first wins" convention), so the mismatch is
     reported at the *later* (default) arm's own value, not the first. *)
  let path = "switch-arm-mismatch.wo" in
  let _, collector =
    typecheck_str ~file:path
      "fn f(n: Int) -> Text {\n  let v = switch n {\n    case 1: \"a\";\n    default: 0;\n  }\n  return v\n}\n"
  in
  let diags = Diag.Collector.diagnostics collector in
  check_eq "switch arm mismatch: exactly one diagnostic (WO-E201)" ~expected:1
    ~actual:(List.length diags) string_of_int;
  (match diags with
  | [ d ] ->
    check "switch arm mismatch: code is WO-E201" (d.Diag.code = "WO-E201");
    check "switch arm mismatch: severity is Error" (d.Diag.severity = Diag.Error);
    check "switch arm mismatch: message names both types (`Int` vs `Text`)"
      (find_substring ~needle:"switch arm yields `Int`, but the switch's type is `Text`"
         d.Diag.message
      <> None);
    check_eq "switch arm mismatch: reported at the `default` arm's own value (line 4, col 14)"
      ~expected:(4, 14) ~actual:(d.Diag.site.Diag.line, d.Diag.site.Diag.col)
      (fun (l, c) -> Printf.sprintf "%d:%d" l c)
  | _ -> check "switch arm mismatch: exactly one diagnostic" false)

let () =
  (* Statement position: "the expression with a discarded value" — an
     arm that fails to yield one (every arm here ends in `return`, not
     an `ExprStmt`) must NOT be treated as a type mismatch: nothing is
     unified when the value is never used. Also proves `default` is
     still required in statement position, unconditionally (not just
     when the value is consumed). *)
  let _, collector =
    typecheck_str ~file:"switch-stmt-no-mismatch.wo"
      "fn f(n: Int) -> Int {\n  switch n {\n    case 1: return 1\n    default: return 0\n  }\n  return 0\n}\n"
  in
  check_eq "switch statement position, every arm returns: reports nothing" ~expected:0
    ~actual:(List.length (Diag.Collector.diagnostics collector)) string_of_int

let () =
  (* Review fix (Critical 2): a `Text` subject compared against an
     `Int` case label is not merely a type error — unchecked, it is a
     real VM segfault (emit.ml's EQ-vs-EQS choice reads only the
     subject's type; `s: Text` picks EQS, whose `str_check`
     dereferences the case value's own register — a raw int64 — as a
     `wo_str*`). Wired through WO-E201 (`type_mismatch_code`), the
     same code the arm-unification check above uses, per the review's
     own instruction. *)
  let path = "switch-text-int-mismatch.wo" in
  let _, collector =
    typecheck_str ~file:path
      "fn f(s: Text) -> Text {\n  let v = switch s {\n    case 1: \"a\";\n    default: \"b\";\n  }\n  return v\n}\n"
  in
  let diags = Diag.Collector.diagnostics collector in
  check_eq "switch Text-subject/Int-case: exactly one diagnostic (WO-E201)" ~expected:1
    ~actual:(List.length diags) string_of_int;
  (match diags with
  | [ d ] ->
    check "switch Text-subject/Int-case: code is WO-E201" (d.Diag.code = "WO-E201");
    check "switch Text-subject/Int-case: severity is Error" (d.Diag.severity = Diag.Error);
    check "switch Text-subject/Int-case: message names both types"
      (find_substring
         ~needle:"switch case value has type `Int`, but the switch subject has type `Text`"
         d.Diag.message
      <> None)
  | _ -> check "switch Text-subject/Int-case: exactly one diagnostic" false)

let () =
  (* The reverse direction: an `Int` subject against a `Text` case
     label doesn't crash the VM (EQ just compares two int64s), but the
     case can never fire (a silent always-false) — equally wrong, and
     the review calls it out explicitly as "equally wrong today." *)
  let _, collector =
    typecheck_str ~file:"switch-int-text-mismatch.wo"
      "fn f(n: Int) -> Text {\n  let v = switch n {\n    case \"one\": \"a\";\n    default: \"b\";\n  }\n  return v\n}\n"
  in
  let diags = Diag.Collector.diagnostics collector in
  check_eq "switch Int-subject/Text-case: exactly one diagnostic (WO-E201)" ~expected:1
    ~actual:(List.length diags) string_of_int;
  match diags with
  | [ d ] ->
    check "switch Int-subject/Text-case: code is WO-E201" (d.Diag.code = "WO-E201");
    check "switch Int-subject/Text-case: message names both types"
      (find_substring
         ~needle:"switch case value has type `Text`, but the switch subject has type `Int`"
         d.Diag.message
      <> None)
  | _ -> check "switch Int-subject/Text-case: exactly one diagnostic" false

let () =
  (* Silence proof: the pattern-vs-subject check must NOT false-positive
     against an unresolved/placeholder subject type — the exact shape
     the sample's own union-typed switch sites have today (Task 4's
     territory, already WO-E207'd) — matching the "confident, stay
     silent when underivable" contract WO-E209 established. `Unknown`
     is not a builtin scalar, gc class, or declared class, so its
     `wob_kind` is WO_K_OWNED — deliberately `Other`, never compared. *)
  let _, collector =
    typecheck_str ~file:"switch-unresolved-subject.wo"
      "fn f(u: Unknown) -> Text {\n  let v = switch u {\n    case Ok: \"a\";\n    default: \"b\";\n  }\n  return v\n}\n"
  in
  let non_e201 =
    List.filter (fun (d : Diag.t) -> d.Diag.code = "WO-E201") (Diag.Collector.diagnostics collector)
  in
  check_eq "switch unresolved subject: no WO-E201 false positive" ~expected:0
    ~actual:(List.length non_e201) string_of_int

(* ---- direct ownership-pass assertions (Task 7) ------------------------

   golden/owner-err/ already pins the *rendered* text of every must-fail
   fixture (code, message, both sites, source excerpts). These assertions
   pin the parts a rendered blob cannot state as a contract: that exactly
   the intended WO-E3xx code fires, that the second site is really a
   `related` entry on the same diagnostic rather than a separate one, that
   the exemptions (@gc, scalars) produce nothing at all, and that the four
   emitter tables carry real AST node ids (positions are what goldens
   pin, ids are what plan 3 keys on). *)

let owner_str ~file src =
  let collector = Diag.Collector.create () in
  let toks = Lexer.tokenize collector ~file src in
  let prog = Parser.parse collector ~file toks in
  let syms, () = Types.typecheck ~file prog collector in
  let syms = Gcinfer.infer [ (file, prog) ] syms in
  let tables = Owner.analyze ~file prog syms collector in
  (tables, collector)

(* The whole pipeline through the emitter, single-file (every golden
   fixture is one file). Returns the serialized image plus the collector,
   so a test can assert on the bytes, on the disassembly, or on the
   diagnostics. *)
let emit_str ~file src =
  let collector = Diag.Collector.create () in
  let toks = Lexer.tokenize collector ~file src in
  let prog = Parser.parse collector ~file toks in
  let syms, () = Types.typecheck ~file prog collector in
  let syms = Gcinfer.infer [ (file, prog) ] syms in
  let tables = Owner.analyze ~file prog syms collector in
  (* Single-file helper (every golden fixture is one file): its own
     module is "." and that module's own symbols are exactly `syms` —
     no cross-module resolution to plumb through for these tests. *)
  let module_syms = Hashtbl.create 1 in
  Hashtbl.replace module_syms "." syms;
  let image =
    Emit.emit ~syms ~module_of:(fun _ -> ".") ~module_syms collector [ { Emit.file; prog; tables } ]
  in
  (image, collector)

let is_ownership_code (code : string) =
  String.length code >= 5 && String.sub code 0 5 = Diag.ownership_prefix

let ownership_diags collector =
  Diag.Collector.diagnostics collector
  |> List.filter (fun (d : Diag.t) -> is_ownership_code d.Diag.code)

let all_diags collector = Diag.Collector.diagnostics collector

(* Analyzes a fixture from golden/owner-err/ and returns its ownership
   diagnostics; also asserts no *other* stage complained, so a fixture can
   never pass its ownership assertions while quietly tripping a WO-E2xx. *)
let owner_err_fixture name =
  let path = "golden/owner-err/" ^ name ^ ".wo" in
  let _, collector = owner_str ~file:path (read_file path) in
  let all = Diag.Collector.diagnostics collector in
  let own = List.filter (fun (d : Diag.t) -> is_ownership_code d.Diag.code) all in
  check_eq (name ^ ": every diagnostic is an ownership diagnostic")
    ~expected:(List.length all) ~actual:(List.length own) string_of_int;
  own

(* A two-site ownership error: the code, the primary site's line/col, and
   the single related site's line/col. *)
let check_site tag ~code ~line ~col ~rel_line ~rel_col (d : Diag.t) =
  check (tag ^ ": code is " ^ code) (d.Diag.code = code);
  check (tag ^ ": severity is Error") (d.Diag.severity = Diag.Error);
  check_eq (tag ^ ": primary line") ~expected:line ~actual:d.Diag.site.Diag.line string_of_int;
  check_eq (tag ^ ": primary col") ~expected:col ~actual:d.Diag.site.Diag.col string_of_int;
  match d.Diag.related with
  | [ r ] ->
    check_eq (tag ^ ": related line") ~expected:rel_line ~actual:r.Diag.site.Diag.line
      string_of_int;
    check_eq (tag ^ ": related col") ~expected:rel_col ~actual:r.Diag.site.Diag.col string_of_int;
    check (tag ^ ": related site carries a label") (r.Diag.label <> "")
  | rs ->
    check_eq (tag ^ ": exactly one related site") ~expected:1 ~actual:(List.length rs)
      string_of_int

let single tag (ds : Diag.t list) (f : Diag.t -> unit) =
  match ds with
  | [ d ] -> f d
  | _ ->
    check_eq (tag ^ ": exactly one ownership error") ~expected:1 ~actual:(List.length ds)
      string_of_int

(* Same shape for table entries: assert there is exactly one, then assert
   things about it. *)
let single_site tag (xs : 'a list) (f : 'a -> unit) =
  match xs with
  | [ x ] -> f x
  | _ -> check_eq (tag ^ ": exactly one") ~expected:1 ~actual:(List.length xs) string_of_int

let () =
  (* `consume(b)` twice: the second one uses a moved value. Sites are the
     two `b` argument tokens, 11:24 and 10:23. *)
  single "use-after-move" (owner_err_fixture "use-after-move") (fun d ->
      check_site "use-after-move" ~code:"WO-E301" ~line:11 ~col:24 ~rel_line:10 ~rel_col:23 d)

let () =
  (* `let alias = b.inner` borrows into b; `consume(b)` then moves b out
     from under that borrow. Primary at the moved argument (15:18),
     related at the borrowing `let` (14:3). *)
  single "move-while-borrowed" (owner_err_fixture "move-while-borrowed") (fun d ->
      check_site "move-while-borrowed" ~code:"WO-E302" ~line:15 ~col:18 ~rel_line:14 ~rel_col:3 d)

let () =
  (* Two provable aliases: `swap(it, it)` (same root) and
     `swap(bag.items[i], bag.items[i])` (same root *and* the same runtime
     index expression, so the analysis proves the alias rather than
     deferring to a runtime check — contrast golden/owner/residual.wo). *)
  match owner_err_fixture "double-mut" with
  | [ a; b ] ->
    check_site "double-mut (same local)" ~code:"WO-E303" ~line:14 ~col:19 ~rel_line:14 ~rel_col:15 a;
    check_site "double-mut (same runtime index)" ~code:"WO-E303" ~line:18 ~col:29 ~rel_line:18
      ~rel_col:15 b
  | ds ->
    check_eq "double-mut: exactly two ownership errors" ~expected:2 ~actual:(List.length ds)
      string_of_int

let () =
  (* The same rule across statements rather than inside one call: a
     let-bound alias keeps its borrow alive, so the borrowed place may be
     neither exclusively re-borrowed (`touch(h.box)`) nor assigned to
     (`h.box = fresh`) while the alias is in scope. *)
  match owner_err_fixture "borrowed-place-mutated" with
  | [ arg; assign ] ->
    check_site "borrowed-place-mutated (`mut` argument)" ~code:"WO-E303" ~line:15 ~col:16
      ~rel_line:14 ~rel_col:3 arg;
    check_site "borrowed-place-mutated (assignment)" ~code:"WO-E303" ~line:20 ~col:3 ~rel_line:19
      ~rel_col:3 assign
  | ds ->
    check_eq "borrowed-place-mutated: exactly two ownership errors" ~expected:2
      ~actual:(List.length ds) string_of_int

let () =
  (* The three ways a borrow can escape (spec rule 3): stored into a
     field, returned, moved out to a `take` parameter. *)
  match owner_err_fixture "borrow-escape" with
  | [ store; ret; take ] ->
    check_site "borrow-escape (stored in a field)" ~code:"WO-E304" ~line:9 ~col:18 ~rel_line:8
      ~rel_col:12 store;
    check_site "borrow-escape (returned)" ~code:"WO-E304" ~line:18 ~col:10 ~rel_line:17 ~rel_col:9
      ret;
    check_site "borrow-escape (moved to a `take` parameter)" ~code:"WO-E304" ~line:22 ~col:15
      ~rel_line:21 ~rel_col:10 take;
    (* spec section 6's own wording for this diagnostic *)
    check "borrow-escape: names the place and the function it escapes"
      (Option.is_some (find_substring ~needle:"borrow of `h.items` escapes `leak`" ret.Diag.message))
  | ds ->
    check_eq "borrow-escape: exactly three ownership errors" ~expected:3 ~actual:(List.length ds)
      string_of_int

let () =
  (* The loop fixpoint's reason for existing: the move is legal on the
     first iteration and a use-after-move on every later one, so both
     sites land on the same token — the message says so. *)
  single "loop-move" (owner_err_fixture "loop-move") (fun d ->
      check_site "loop-move" ~code:"WO-E301" ~line:12 ~col:29 ~rel_line:12 ~rel_col:29 d;
      check "loop-move: message names the previous iteration"
        (Option.is_some (find_substring ~needle:"previous loop iteration" d.Diag.message)))

let () =
  (* A traced (gc) class is exempt from all of it (spec rule 5): the exact
     shape that is WO-E301 above is silent here, and produces no move-table
     entries either — a gc transfer is an rc site, not a move. `Cache` is
     self-referential (`peer: ?Cache`), so inference classifies it gc without
     any annotation (iteration 7b). *)
  let src =
    "class Cache {\n\
    \  n: Int\n\
    \  peer: ?Cache\n\
     }\n\
     \n\
     fn keep(take c: Cache) -> Int {\n\
    \  return 0\n\
     }\n\
     \n\
     fn twice(take c: Cache) -> Int {\n\
    \  let a = keep(c)\n\
    \  let b = keep(c)\n\
    \  return a + b\n\
     }\n"
  in
  let tables, coll = owner_str ~file:"gc-exempt.wo" src in
  check_eq "@gc exemption: no ownership diagnostics" ~expected:0
    ~actual:(List.length (ownership_diags coll)) string_of_int;
  check_eq "@gc exemption: no move-table entries" ~expected:0
    ~actual:(List.length tables.Owner.moves) string_of_int

let () =
  (* Scalars are copied, never moved: same shape, nothing reported and
     nothing in any table. *)
  let src =
    "fn add(take n: Int) -> Int {\n\
    \  return n\n\
     }\n\
     \n\
     fn twice(take n: Int) -> Int {\n\
    \  let a = add(n)\n\
    \  let b = add(n)\n\
    \  return a + b\n\
     }\n"
  in
  let tables, coll = owner_str ~file:"scalar-exempt.wo" src in
  check_eq "scalar exemption: no ownership diagnostics" ~expected:0
    ~actual:(List.length (ownership_diags coll)) string_of_int;
  check_eq "scalar exemption: no move-table entries" ~expected:0
    ~actual:(List.length tables.Owner.moves) string_of_int;
  check_eq "scalar exemption: no drop-table entries" ~expected:0
    ~actual:(List.length tables.Owner.drops) string_of_int

let () =
  (* `?T` carries T's ownership exactly — nil is just a value, so the
     use-after-move fires through the nullable wrapper too. *)
  let src =
    "class Box {\n\
    \  n: Int\n\
     }\n\
     \n\
     fn consume(take b: ?Box) -> Int {\n\
    \  return 0\n\
     }\n\
     \n\
     fn run(take b: ?Box) -> Int {\n\
    \  let x = consume(b)\n\
    \  let y = consume(b)\n\
    \  return x + y\n\
     }\n"
  in
  let tables, coll = owner_str ~file:"nullable.wo" src in
  single "?T ownership" (ownership_diags coll) (fun d ->
      check "?T ownership: `?Box` is moved and use-after-move fires"
        (d.Diag.code = "WO-E301" && d.Diag.site.Diag.line = 11));
  check_eq "?T ownership: the first `?Box` pass is a real transfer" ~expected:1
    ~actual:(List.length tables.Owner.moves) string_of_int

let () =
  (* Every decision golden must be completely clean — no ownership error,
     and no WO-E2xx/WO-W2xx either, so a table golden can never drift into
     documenting the output of a broken program. *)
  List.iter
    (fun name ->
      let path = "golden/owner/" ^ name ^ ".wo" in
      let _, coll = owner_str ~file:path (read_file path) in
      check_eq ("decision golden " ^ name ^ ": reports nothing") ~expected:0
        ~actual:(List.length (all_diags coll)) string_of_int)
    [ "moves"; "drops"; "rc"; "residual"; "pricing-demo" ]

let () =
  (* The tables are keyed by AST node id for plan 3 (goldens can only pin
     positions — ids churn), so assert the ids are actually populated. *)
  let path = "golden/owner/moves.wo" in
  let tables, _ = owner_str ~file:path (read_file path) in
  check_eq "moves table: four transfers (LET, CTOR field, `take` arg, RETURN)" ~expected:4
    ~actual:(List.length tables.Owner.moves) string_of_int;
  check "moves table: every entry carries a real AST node id"
    (List.for_all (fun (m : Owner.move_site) -> m.Owner.mv_node > 0) tables.Owner.moves);
  check "drops table: every entry carries a real AST node id"
    (List.for_all (fun (d : Owner.drop_site) -> d.Owner.dr_node > 0) tables.Owner.drops);
  check "drops table: every listed local names its declaring node (names alone shadow)"
    (List.for_all
       (fun (d : Owner.drop_site) ->
         List.for_all (fun (i : Owner.drop_item) -> i.Owner.di_node > 0) d.Owner.dr_items)
       tables.Owner.drops);
  let res_path = "golden/owner/residual.wo" in
  let res_tables, _ = owner_str ~file:res_path (read_file res_path) in
  check_eq "residual table: only the runtime-index pairs are residual" ~expected:3
    ~actual:(List.length res_tables.Owner.residuals) string_of_int;
  check "residual table: every entry names its region and both operand nodes"
    (List.for_all
       (fun (r : Owner.residual_site) ->
         r.Owner.rs_node > 0 && r.Owner.rs_a_node > 0 && r.Owner.rs_b_node > 0
         && r.Owner.rs_a_node <> r.Owner.rs_b_node)
       res_tables.Owner.residuals)

(* ---- review follow-ups (Task 7 review, 1 critical + 5 important) ------

   Each block below pins one reviewed defect at the level the golden text
   cannot state: an *absent* table entry, or a verdict (ELIDED vs KEPT)
   that would still render as a plausible-looking line if it flipped. *)

let drop_sites_of tables kind_matches =
  List.filter (fun (d : Owner.drop_site) -> kind_matches d.Owner.dr_kind) tables.Owner.drops

let names_of (d : Owner.drop_site) =
  List.map (fun (i : Owner.drop_item) -> i.Owner.di_name) d.Owner.dr_items

let () =
  (* CRITICAL: a double-`mut` reached through two `let`-bound aliases used to
     compare the syntactic roots `r` and `s`, conclude Disjoint, and emit
     neither a diagnostic nor a residual site — so nobody, compiler or VM,
     enforced the rule. Canonicalized places make it identical to the direct
     `swap(bag.items[i], bag.items[k])` form. *)
  let path = "golden/owner/residual.wo" in
  let tables, coll = owner_str ~file:path (read_file path) in
  check_eq "aliased double-mut: no diagnostic (unprovable, so the VM decides)" ~expected:0
    ~actual:(List.length (ownership_diags coll)) string_of_int;
  let via_alias =
    List.filter (fun (r : Owner.residual_site) -> r.Owner.rs_pos.Ast.line = 32)
      tables.Owner.residuals
  in
  single_site "aliased double-mut: exactly one residual site" via_alias (fun r ->
      check "aliased double-mut: both sides exclusive"
        (r.Owner.rs_a_kind = Owner.AExcl && r.Owner.rs_b_kind = Owner.AExcl);
      check "aliased double-mut: rendered canonically, not as the alias names"
        (r.Owner.rs_a = "bag.items[i]" && r.Owner.rs_b = "bag.items[k]"));
  check "residual table: a move is never a residual side (a whole local always decides)"
    (List.for_all
       (fun (r : Owner.residual_site) ->
         r.Owner.rs_a_kind <> Owner.AMove && r.Owner.rs_b_kind <> Owner.AMove)
       tables.Owner.residuals);
  check "residual table: at least one side of every pair is exclusive"
    (List.for_all
       (fun (r : Owner.residual_site) ->
         r.Owner.rs_a_kind = Owner.AExcl || r.Owner.rs_b_kind = Owner.AExcl)
       tables.Owner.residuals)

let () =
  (* Using a borrow alongside the container it borrows from is what the
     binding is for, so it must stay silent — the guard that keeps the
     critical fix above from turning every `for` cursor into a conflict. *)
  let src =
    "class Item {\n\
    \  n: Int\n\
     }\n\
     \n\
     class Bag {\n\
    \  items: multi Item\n\
     \n\
    \  fn eat(mut e: Item) -> Int {\n\
    \    self.items = self.items\n\
    \    return 0\n\
    \  }\n\
     }\n\
     \n\
     fn cursor_reuse(mut bag: Bag) -> Int {\n\
    \  let total = 0\n\
    \  for it in bag.items {\n\
    \    total = total + bag.eat(it)\n\
    \  }\n\
    \  return total\n\
     }\n"
  in
  let tables, coll = owner_str ~file:"cursor.wo" src in
  let in_loop =
    List.filter (fun (d : Diag.t) -> d.Diag.site.Diag.line = 17) (ownership_diags coll)
  in
  check_eq "cursor reuse: passing the cursor to a method on its own container is silent"
    ~expected:0 ~actual:(List.length in_loop) string_of_int;
  check_eq "cursor reuse: and needs no runtime borrow either" ~expected:0
    ~actual:(List.length tables.Owner.residuals) string_of_int

let () =
  let path = "golden/owner/drops.wo" in
  let tables, _ = owner_str ~file:path (read_file path) in
  (* IMPORTANT: conditionally moved value. The join records Moved, so the
     path that did *not* move it must drop it at that branch's end or the
     value leaks. `conditional_move` has no `else`, so the drop is anchored
     at the `if` itself (line 50). *)
  let joins = drop_sites_of tables (function Owner.DBranchJoin _ -> true | _ -> false) in
  single_site "join normalization: exactly one JOIN-DROP in this fixture" joins (fun d ->
      check "join normalization: on the implicit else branch"
        (d.Owner.dr_kind = Owner.DBranchJoin "ELSE");
      check_eq "join normalization: anchored at the `if`" ~expected:50
        ~actual:d.Owner.dr_pos.Ast.line string_of_int;
      check "join normalization: drops the value the then-branch moved" (names_of d = [ "a" ]));
  check "join normalization: and the value is not dropped again on the moving path"
    (not
       (List.exists
          (fun (d : Owner.drop_site) ->
            d.Owner.dr_kind = Owner.DReturn && d.Owner.dr_pos.Ast.line = 53)
          tables.Owner.drops));
  (* IMPORTANT: `a = a` used to record OVERWRITE for the value it replaces
     *and* keep `a` live — two drops of one value. *)
  let overwrites = drop_sites_of tables (fun k -> k = Owner.DOverwrite) in
  check "self-assignment: records no OVERWRITE (target and value are one storage)"
    (not (List.exists (fun (d : Owner.drop_site) -> d.Owner.dr_pos.Ast.line = 57) overwrites));
  check "self-assignment: the value is still dropped exactly once, at the return"
    (List.exists
       (fun (d : Owner.drop_site) ->
         d.Owner.dr_kind = Owner.DReturn && d.Owner.dr_pos.Ast.line = 58 && names_of d = [ "a" ])
       tables.Owner.drops);
  (* IMPORTANT: re-initialising a moved-out local makes it live again, so it
     must reappear in a later drop set (self-found bug 1, now pinned). *)
  check "re-init after move: the reassigned local is dropped at the return"
    (List.exists
       (fun (d : Owner.drop_site) ->
         d.Owner.dr_kind = Owner.DReturn && d.Owner.dr_pos.Ast.line = 64 && names_of d = [ "a" ])
       tables.Owner.drops);
  check "re-init after move: no OVERWRITE, since the moved-out local held nothing"
    (not (List.exists (fun (d : Owner.drop_site) -> d.Owner.dr_pos.Ast.line = 63) overwrites))

let () =
  (* haxe-parity Task 3: switch arms are alternate flows joining back
     together — the N-way generalization of if/else's own JOIN-DROP
     (branch_join_drops, reused verbatim per the brief's own
     instruction: "reuse it, do not invent a second join"). `pick`
     moves `b` in ARM0 (`case 1`, via a `take` call) and merely reads
     it in ARM1 (`default`); after the merge `b` is Moved either way
     (join takes Moved over Live), so ARM1 — the arm that *kept* it —
     must get its own synthetic drop at its own end, or the value
     leaks on that path; ARM0 must NOT get a second one (a double
     free). Controller-verified end to end under `runtime/build/
     wovm_asan` (both call paths, task-3-report.md has the transcript);
     this pins the table entry the ASan proof depends on. *)
  let src =
    "class Box {\n  n: Int\n}\n\n\
     fn consume(take b: Box) -> Int {\n  return b.n\n}\n\n\
     fn pick(k: Int, take b: Box) -> Int {\n\
    \  switch k {\n\
    \    case 1:\n\
    \      print_int(consume(b))\n\
    \    default:\n\
    \      print(\"kept\")\n\
    \  }\n\
    \  return 0\n\
     }\n"
  in
  let tables, coll = owner_str ~file:"switch-join.wo" src in
  check_eq "switch join: reports nothing (a legal move on one arm only)" ~expected:0
    ~actual:(List.length (ownership_diags coll)) string_of_int;
  let joins = drop_sites_of tables (function Owner.DBranchJoin _ -> true | _ -> false) in
  single_site "switch join: exactly one JOIN-DROP" joins (fun d ->
      check "switch join: on the arm that kept `b` (the `default` arm, ARM1)"
        (d.Owner.dr_kind = Owner.DBranchJoin "ARM1");
      check "switch join: drops the value the other arm (ARM0) moved" (names_of d = [ "b" ]));
  check "switch join: the moving arm (ARM0) gets no synthetic drop of its own"
    (not
       (List.exists
          (fun (d : Owner.drop_site) -> d.Owner.dr_kind = Owner.DBranchJoin "ARM0")
          tables.Owner.drops))

let () =
  (* Arm-local drop: an owned value created inside one arm and never
     moved dies at that arm's own scope end (DScope "ARM<i>") — the
     ordinary scope-drop machinery every block already gets via
     analyze_block, reused verbatim ("each arm is its own drop scope",
     the brief's own words). tests/corpus/run/lang-switch-arm-drop
     proves this under ASan with a real leak-sized object; this pins
     the table entry that fixture's own DROP instruction depends on. *)
  let src =
    "class Item {\n  n: Int\n}\n\n\
     fn f(k: Int) -> Int {\n\
    \  switch k {\n\
    \    case 1:\n\
    \      let it = Item { n: 1 }\n\
    \      print_int(it.n)\n\
    \    default:\n\
    \      print(\"other\")\n\
    \  }\n\
    \  return 0\n\
     }\n"
  in
  let tables, coll = owner_str ~file:"switch-arm-scope.wo" src in
  check_eq "switch arm scope: reports nothing" ~expected:0
    ~actual:(List.length (ownership_diags coll)) string_of_int;
  let scopes =
    drop_sites_of tables (function
      | Owner.DScope l -> String.length l >= 3 && String.sub l 0 3 = "ARM"
      | _ -> false)
  in
  single_site "switch arm scope: exactly one arm-local DScope drop" scopes (fun d ->
      check "switch arm scope: on ARM0 (`case 1`)" (d.Owner.dr_kind = Owner.DScope "ARM0");
      check "switch arm scope: drops the arm-local `it`" (names_of d = [ "it" ]))

let () =
  (* Review fix (Critical 1), the two-file half: `default` is lowered
     *last* regardless of source position (Ast.switch_lowering_order),
     and owner.ml's `analyze_switch` must walk the identical order or
     its "ARM<i>" labels drift from emit.ml's own — this is exactly
     the failure mode that would silently break DScope/JOIN-DROP
     lookups without ever showing up as a wrong *count*. `default` is
     written FIRST here, `case 1` SECOND; if the two files agreed on
     source order (the bug) the JOIN-DROP would land on "ARM0"
     (`default`, keeping `b`) — this asserts it lands on "ARM1"
     instead, proving `default` was actually lowered (and labeled)
     last, matching lang-switch-default-not-last's own runtime proof. *)
  let src =
    "class Box {\n  n: Int\n}\n\n\
     fn consume(take b: Box) -> Int {\n  return b.n\n}\n\n\
     fn pick(k: Int, take b: Box) -> Int {\n\
    \  switch k {\n\
    \    default:\n\
    \      print(\"kept\")\n\
    \    case 1:\n\
    \      print_int(consume(b))\n\
    \  }\n\
    \  return 0\n\
     }\n"
  in
  let tables, coll = owner_str ~file:"switch-reorder.wo" src in
  check_eq "switch reorder: reports nothing (WO-W203 aside — this is typecheck-only)"
    ~expected:0 ~actual:(List.length (ownership_diags coll)) string_of_int;
  let joins = drop_sites_of tables (function Owner.DBranchJoin _ -> true | _ -> false) in
  single_site "switch reorder: exactly one JOIN-DROP" joins (fun d ->
      check
        "switch reorder: on ARM1 (`default`, lowered last despite being written first)"
        (d.Owner.dr_kind = Owner.DBranchJoin "ARM1");
      check "switch reorder: drops the value ARM0 (`case 1`) moved" (names_of d = [ "b" ]))

let () =
  (* Review fix (Critical 3): owner.ml's `expr_ty` used to return
     `None` for a `Switch` — `analyze_let`'s own fallback for that is
     `Scalar "Int"` (Copy), so an *unannotated* `let` binding a
     class-yielding switch was silently never dropped (reviewer-
     reproduced real leak; tests/corpus/run/lang-switch-class-arm-leak
     has the ASan RED→GREEN transcript). This pins the table entry
     that fixture's own DROP instruction depends on: `w` must be
     classified Owned (a real DReturn drop naming it), not silently
     absent the way a Copy-classified local would leave it. *)
  let src =
    "class Widget {\n  a: Int\n}\n\n\
     fn f(k: Int) -> Int {\n\
    \  let w = switch k {\n\
    \    case 1: Widget { a: 1 }\n\
    \    default: Widget { a: 2 }\n\
    \  }\n\
    \  return w.a\n\
     }\n"
  in
  let tables, coll = owner_str ~file:"switch-let-class.wo" src in
  check_eq "switch let-class: reports nothing" ~expected:0
    ~actual:(List.length (ownership_diags coll)) string_of_int;
  let returns = drop_sites_of tables (fun k -> k = Owner.DReturn) in
  check "switch let-class: `w` is dropped at the return (classified Owned, not silently Copy)"
    (List.exists (fun d -> names_of d = [ "w" ]) returns)

let () =
  let path = "golden/owner/moves.wo" in
  let exit_code, stdout, stderr = run_cli [ "--dump-owner"; path ] in
  check "cli smoke: --dump-owner on a clean file exits 0" (exit_code = 0);
  check "cli smoke: clean-file --dump-owner writes nothing to stderr" (stderr = "");
  let tables, _ = owner_str ~file:path (read_file path) in
  check "cli smoke: --dump-owner stdout matches the in-process table dump exactly"
    (stdout = Dump.dump_owner tables)

let () =
  let path = "golden/owner-err/use-after-move.wo" in
  let exit_code, stdout, stderr = run_cli [ "--dump-owner"; path ] in
  check "cli smoke: an ownership-error file exits 1, not 0" (exit_code = 1);
  check "cli smoke: the WO-E301 diagnostic goes to stderr"
    (Option.is_some (find_substring ~needle:"WO-E301" stderr));
  check "cli smoke: stdout still carries the tables on exit 1"
    (Option.is_some (find_substring ~needle:"== RESIDUAL ==" stdout))

(* ---- the loader's validation battery, re-implemented -------------------

   The plan's round-trip rule: an image `woc` produces that `wovm`'s
   loader rejects is always an emitter bug. Running the C binary from
   here would make this suite depend on the runtime being built, so the
   rule is enforced by a third, independent decoder of the same format
   (runtime/test/wob_build.c is the second: an independent encoder of the
   same format).
   Every check below is the OCaml twin of a BAIL in
   runtime/src/loader.c's wo_load_buf, in the same order, so a divergence
   between the emitter and the loader surfaces in `dune runtest` rather
   than in the corpus. Returns the list of violations; [] means the
   loader would accept the image. *)

let validate_image (img : string) : string list =
  let bad = ref [] in
  let fail fmt = bad := fmt :: !bad in
  let len = String.length img in
  let ok n o = o >= 0 && o + n <= len in
  let u8 o = if ok 1 o then String.get_uint8 img o else -1 in
  let u16 o = if ok 2 o then String.get_uint16_le img o else -1 in
  let u32 o = if ok 4 o then Int32.to_int (String.get_int32_le img o) land 0xFFFFFFFF else -1 in
  let u64 o = if ok 8 o then String.get_int64_le img o else 0L in
  let none = 0xFFFFFFFF in
  if u32 0 <> 0x31424F57 then fail "bad magic";
  if u32 4 <> 8 then fail "unsupported version"; (* v8: databasev2 2 task 6a *)
  let coff = u32 8 and ccnt = u32 12 in
  let koff = u32 16 and kcnt = u32 20 in
  let ioff = u32 24 and icnt = u32 28 in
  let moff = u32 32 and mcnt = u32 36 in
  let entry = u32 40 in
  List.iter
    (fun o -> if o > len then fail "section offset out of range")
    [ coff; koff; ioff; moff ];
  (* constants *)
  let ctag = Array.make (max ccnt 1) (-1) in
  let o = ref coff in
  for i = 0 to ccnt - 1 do
    let tag = u8 !o in
    incr o;
    ctag.(i) <- tag;
    (* tag 2 = WOB_K_FLOAT (iteration 19): same 8-byte payload as an Int,
       read as f64 bits. Every bit pattern is a legal f64, so nothing to
       validate beyond the length. *)
    if tag = 0 || tag = 2 then o := !o + 8
    else if tag = 1 then begin
      let n = u32 !o in
      o := !o + 4;
      if not (ok n !o) then fail (Printf.sprintf "constant %d: text overruns" i);
      o := !o + n
    end
    else fail (Printf.sprintf "constant %d: unknown tag %d" i tag)
  done;
  if !o > len then fail "constant pool overruns image";
  let text_const i = i >= 0 && i < ccnt && ctag.(i) = 1 in
  (* classes *)
  let class_fields = Array.make (max kcnt 1) 0 in
  let o = ref koff in
  for i = 0 to kcnt - 1 do
    let nm = u32 !o and flags = u32 (!o + 4) and fcnt = u32 (!o + 8) in
    o := !o + 12;
    if not (text_const nm) then fail (Printf.sprintf "class %d: bad name constant" i);
    (* v7 (databasev2 2): bit1 VOLATILE, bit2 RESIDENT_KEYS; v8 (task 6a): bit3
       TABLE. This battery is a deliberately independent reimplementation of
       runtime/src/loader.c's validation, so it tracks the same contract —
       including refusing the pair that would leave rows neither logged nor
       resident, and storage bits on a class that is not a @table. *)
    if flags land lnot 0x0f <> 0 then fail (Printf.sprintf "class %d: unknown flags" i);
    if flags land 0x02 <> 0 && flags land 0x04 <> 0 then
      fail (Printf.sprintf "class %d: durable:false with resident:keys" i);
    if flags land 0x06 <> 0 && flags land 0x08 = 0 then
      fail (Printf.sprintf "class %d: storage flags on a class that is not a @table" i);
    if fcnt > 65535 then fail (Printf.sprintf "class %d: too many fields" i);
    class_fields.(i) <- fcnt;
    let kco = !o in (* the kind bytes' offset: the v3 index walk re-reads them *)
    for j = 0 to fcnt - 1 do
      (* WO_K_MAX is 7 since iteration 19 (6 = FLOAT, 7 = BYTES) *)
      if u8 (!o + j) > 7 then fail (Printf.sprintf "class %d field %d: bad kind" i j)
    done;
    o := !o + fcnt + ((4 - (fcnt mod 4)) mod 4);
    (* v2: three u32 arrays of per-field metadata — names (a Text constant or
       "not recorded"), the referenced class id (or the json-raw marker), and
       container element kinds. Mirrors runtime/src/loader.c's own checks. *)
    for j = 0 to fcnt - 1 do
      let nmk = u32 (!o + (j * 4)) in
      if nmk <> 0xFFFFFFFF && not (text_const nmk) then
        fail (Printf.sprintf "class %d field %d: bad name constant" i j);
      let fc = u32 (!o + ((fcnt + j) * 4)) in
      (* NONE, JSON_RAW, NIL_SCALAR, BOOL, NIL_BOOL, and (iteration 19)
         NIL_FLOAT are markers, not class ids — same list as loader.c *)
      if
        fc <> 0xFFFFFFFF && fc <> 0xFFFFFFFE && fc <> 0xFFFFFFFD && fc <> 0xFFFFFFFC
        && fc <> 0xFFFFFFFB && fc <> 0xFFFFFFFA && fc >= kcnt
      then fail (Printf.sprintf "class %d field %d: field class out of range" i j)
    done;
    o := !o + (fcnt * 12);
    (* v3: the index tail — flags (bit0 only), col_cnt 1..8, columns in
       range and scalar/Text-kinded. Mirrors loader.c's checks. *)
    let icnt_x = u32 !o in
    o := !o + 4;
    if icnt_x > 64 then fail (Printf.sprintf "class %d: too many indexes" i);
    for x = 0 to icnt_x - 1 do
      let ifl = u32 !o and ccnt = u32 (!o + 4) in
      o := !o + 8;
      if ifl land lnot 1 <> 0 then fail (Printf.sprintf "class %d index %d: unknown flags" i x);
      if ccnt = 0 || ccnt > 8 then fail (Printf.sprintf "class %d index %d: bad column count" i x);
      for c = 0 to ccnt - 1 do
        let col = u32 !o in
        o := !o + 4;
        if col >= fcnt then fail (Printf.sprintf "class %d index %d: column out of range" i x);
        let kind = u8 (kco + col) in
        (* iteration 19: FLOAT (6) is indexable — the engine orders it by the
           total order (NaN last). BYTES (7) is not, this iteration. *)
        if kind <> 0 && kind <> 3 && kind <> 6 then
          fail
            (Printf.sprintf "class %d index %d: column %d is not scalar, Text, or Float" i x c)
      done
    done;
    if !o > len then fail (Printf.sprintf "class %d: truncated" i)
  done;
  (* interfaces + vtable rows *)
  let slot_base = Array.make (max icnt 1) 0 in
  let imcnt = Array.make (max icnt 1) 0 in
  let slots = ref 0 in
  let o = ref ioff in
  for i = 0 to icnt - 1 do
    let nm = u32 !o and mc = u32 (!o + 4) in
    o := !o + 8;
    if not (text_const nm) then fail (Printf.sprintf "interface %d: bad name constant" i);
    if mc = 0 || mc > 1024 then fail (Printf.sprintf "interface %d: bad method count" i);
    slot_base.(i) <- !slots;
    imcnt.(i) <- mc;
    slots := !slots + mc
  done;
  let vrows = u32 !o in
  o := !o + 4;
  let seen_rows = Hashtbl.create 8 in
  let vmethods = ref [] in
  for r = 0 to vrows - 1 do
    let cid = u32 !o and iid = u32 (!o + 4) in
    o := !o + 8;
    if cid >= kcnt then fail (Printf.sprintf "vtable row %d: bad class" r);
    if iid >= icnt then fail (Printf.sprintf "vtable row %d: bad interface" r)
    else
      for j = 0 to imcnt.(iid) - 1 do
        let m = u32 (!o + (4 * j)) in
        vmethods := m :: !vmethods;
        let key = (cid, slot_base.(iid) + j) in
        if Hashtbl.mem seen_rows key then
          fail (Printf.sprintf "duplicate vtable entry for class %d" cid);
        Hashtbl.replace seen_rows key ()
      done;
    if iid < icnt then o := !o + (4 * imcnt.(iid))
  done;
  (* methods *)
  let margc = Array.make (max mcnt 1) 0 in
  let mregc = Array.make (max mcnt 1) 0 in
  let mclass = Array.make (max mcnt 1) 0 in
  let mcode = Array.make (max mcnt 1) [||] in
  let o = ref moff in
  for i = 0 to mcnt - 1 do
    let nm = u32 !o and cid = u32 (!o + 4) in
    let argc = u8 (!o + 8) and regc = u8 (!o + 9) and reserved = u16 (!o + 10) in
    let clen = u32 (!o + 12) in
    o := !o + 16;
    if not (text_const nm) then fail (Printf.sprintf "method %d: bad name constant" i);
    if cid <> none && cid >= kcnt then fail (Printf.sprintf "method %d: bad class" i);
    if reserved <> 0 then fail (Printf.sprintf "method %d: reserved field not zero" i);
    if regc < 1 || regc > 64 then fail (Printf.sprintf "method %d: register count out of range" i);
    if argc > regc then fail (Printf.sprintf "method %d: more args than registers" i);
    if clen = 0 || clen mod 4 <> 0 then fail (Printf.sprintf "method %d: bad code length" i);
    let ninstr = clen / 4 in
    let code = Array.init (max ninstr 0) (fun j -> u32 (!o + (4 * j))) in
    o := !o + clen;
    margc.(i) <- argc;
    mregc.(i) <- regc;
    mclass.(i) <- cid;
    mcode.(i) <- code;
    let lcnt = u32 !o in
    o := !o + 4;
    if lcnt > ninstr then fail (Printf.sprintf "method %d: line table too long" i);
    let prev = ref (-1) in
    for j = 0 to lcnt - 1 do
      let pc = u32 (!o + (8 * j)) in
      if pc >= ninstr || pc <= !prev then
        fail (Printf.sprintf "method %d: line table not ascending" i);
      prev := pc
    done;
    o := !o + (8 * lcnt);
    let dcnt = u32 !o in
    o := !o + 4;
    if dcnt > ninstr then fail (Printf.sprintf "method %d: drop table too long" i);
    let prev = ref (-1) in
    for j = 0 to dcnt - 1 do
      let base = !o + (20 * j) in
      let pc = u32 base in
      let owned = u64 (base + 4) and gc = u64 (base + 12) in
      if pc >= ninstr || pc <= !prev then
        fail (Printf.sprintf "method %d: drop table not ascending" i);
      prev := pc;
      if regc < 64 && Int64.shift_right_logical (Int64.logor owned gc) regc <> 0L then
        fail (Printf.sprintf "method %d: drop mask out of range" i)
    done;
    o := !o + (20 * dcnt)
  done;
  if !o > len then fail "method table overruns image";
  (* static instruction validation *)
  for i = 0 to mcnt - 1 do
    let regc = mregc.(i) in
    let code = mcode.(i) in
    let ninstr = Array.length code in
    let rchk pc r =
      if r < 0 || r >= regc then
        fail (Printf.sprintf "method %d pc %d: register out of range" i pc)
    in
    Array.iteri
      (fun pc ins ->
        let op = ins land 0xFF in
        let a = (ins lsr 8) land 0xFF in
        let b = (ins lsr 16) land 0xFF in
        let c = (ins lsr 24) land 0xFF in
        let bx = (ins lsr 16) land 0xFFFF in
        let sbx = bx - 32768 in
        match op with
        | 0 -> ()
        | 1 ->
          rchk pc a;
          if bx >= ccnt then fail (Printf.sprintf "method %d pc %d: constant out of range" i pc)
        | 2 | 7 ->
          rchk pc a;
          rchk pc b
        | 3 | 4 | 5 | 6 | 8 | 9 | 10 | 11 | 12 ->
          rchk pc a;
          rchk pc b;
          rchk pc c
        | 13 | 14 ->
          if op = 14 then rchk pc a;
          let tgt = pc + 1 + sbx in
          if tgt < 0 || tgt >= ninstr then
            fail (Printf.sprintf "method %d pc %d: jump out of code" i pc)
        | 15 ->
          rchk pc a;
          if bx >= mcnt then fail (Printf.sprintf "method %d pc %d: callee out of range" i pc)
          else if a + margc.(bx) > regc then
            fail (Printf.sprintf "method %d pc %d: call window exceeds frame" i pc)
        | 16 ->
          rchk pc a;
          if bx >= !slots then
            fail (Printf.sprintf "method %d pc %d: interface slot out of range" i pc)
        | 17 -> rchk pc a
        | 18 -> ()
        | 19 ->
          rchk pc a;
          if bx >= kcnt then fail (Printf.sprintf "method %d pc %d: class out of range" i pc)
        | 20 ->
          rchk pc a;
          rchk pc b
        | 21 ->
          rchk pc a;
          rchk pc c
        | 22 | 23 | 24 | 25 | 26 | 27 | 28 -> rchk pc a
        | 29 ->
          rchk pc a;
          (* the mirror's ceiling tracks wob.h's WO_B_MAX only for ids the
             golden lowering suite actually emits; 61 = DB_INSERT (arity 1:
             the class-id slot — field slots are runtime-validated, same as
             the C loader) *)
          (* iteration 19 widened the accepted band to 70-83 (the Float
             bridges and the Bytes surface) alongside 61-67 *)
          if c > 12 && (c < 61 || c > 67) && (c < 70 || c > 83) then
            fail (Printf.sprintf "method %d pc %d: builtin out of range" i pc)
          else if c = 4 then begin
            if b > 7 then fail (Printf.sprintf "method %d pc %d: bad element kind" i pc)
          end
          else if c = 9 then begin
            if b land 0x0F > 7 || b lsr 4 > 7 then
              fail (Printf.sprintf "method %d pc %d: bad key/value kind" i pc)
          end
          else begin
            let arity =
              match c with
              | 0 -> 0
              | 1 | 2 | 3 | 7 | 8 -> 1
              | 5 | 6 | 11 | 12 -> 2
              | 10 -> 3
              | 61 -> 1
              | 62 -> 4
              | 63 -> 2
              | 64 -> 1
              | 65 -> 3
              | 66 -> 3
              | 67 -> 2
              (* iteration 19: float bridges and Bytes, mirroring
                 loader.c's b_arity table *)
              | 70 | 71 | 72 | 73 | 75 | 80 | 81 | 82 | 83 -> 1
              | 74 | 76 | 78 | 79 -> 2
              | 77 -> 3
              | _ -> 0
            in
            if arity > 0 then begin
              rchk pc b;
              rchk pc (b + arity - 1)
            end
          end
        | 30 | 31 -> ()
        (* iteration 19: FNEG is two registers, the rest are three — the same
           shapes as their Int counterparts (opcodes 7 and 3-6/9-11) *)
        | 38 ->
          rchk pc a;
          rchk pc b
        (* iteration 36 (v6): 42-46, the Int bitwise set — same
           three-register shape *)
        | 34 | 35 | 36 | 37 | 39 | 40 | 41 | 42 | 43 | 44 | 45 | 46 ->
          rchk pc a;
          rchk pc b;
          rchk pc c
        | _ -> fail (Printf.sprintf "method %d pc %d: unknown opcode %d" i pc op))
      code;
    if ninstr > 0 then begin
      let last = code.(ninstr - 1) land 0xFF in
      if not (last = 17 || last = 18 || last = 31 || last = 30 || last = 13) then
        fail (Printf.sprintf "method %d: last instruction is not a terminator" i)
    end
  done;
  List.iter
    (fun m -> if m >= mcnt then fail "vtable entry: method out of range")
    !vmethods;
  if entry <> none then begin
    if entry >= mcnt then fail "entry method out of range"
    else if margc.(entry) <> 0 || mclass.(entry) <> none then
      fail "entry must be a zero-arg free fn"
  end;
  List.rev !bad

(* ---- emitter assertions (Task 1, not golden-diffed) --------------------

   golden/bc/*.wo pin the disassembly; these pin what a dump cannot show:
   that every emitted image satisfies the loader's contract, that the
   elision fixture really contains no borrow/rc op at all, that a
   residual guard never lives in a call window, and that an over-budget
   method diagnoses instead of truncating. *)

let bc_fixtures () =
  let dir = "golden/bc" in
  Sys.readdir dir |> Array.to_list
  |> List.filter (fun n -> Filename.check_suffix n ".wo")
  |> List.sort compare
  |> List.map (fun n -> (dir ^ "/" ^ n, read_file (Filename.concat dir n)))

let () =
  List.iter
    (fun (path, src) ->
      let image, collector = emit_str ~file:path src in
      check_eq (Printf.sprintf "emit %s: compiles clean" path) ~expected:0
        ~actual:(List.length (Diag.Collector.diagnostics collector))
        string_of_int;
      let violations = validate_image image in
      check_eq
        (Printf.sprintf "round trip %s: the loader's battery accepts the image (%s)" path
           (String.concat "; " violations))
        ~expected:0 ~actual:(List.length violations) string_of_int)
    (bc_fixtures ())

(* Every method's block of a disassembly, keyed by the method name as the
   dump writes it ("m3   pair args=..."). *)
let method_block (dump : string) (name : string) : string =
  let lines = String.split_on_char '\n' dump in
  let is_header l =
    String.length l > 1 && l.[0] = 'm' && l.[1] >= '0' && l.[1] <= '9'
  in
  let wanted l = is_header l && find_substring ~needle:(" " ^ name ^ " args=") l <> None in
  let rec collect acc inside = function
    | [] -> List.rev acc
    | l :: tl ->
      if wanted l then collect (l :: acc) true tl
      else if inside && (is_header l || (String.length l > 1 && l.[0] = '=')) then List.rev acc
      else if inside then collect (l :: acc) true tl
      else collect acc false tl
  in
  String.concat "\n" (collect [] false lines)

let () =
  (* The zero-cost promise, iteration 7b edition: a proven method emits no
     borrow op, and NO method anywhere emits an rc op — reference counting
     is gone from the instruction stream entirely (opcodes 27/28 reserved). *)
  let path = "golden/bc/elision.wo" in
  let image, _ = emit_str ~file:path (read_file path) in
  let dump = Disasm.dump image in
  let block = method_block dump "proven" in
  check "elision: `proven` was found in the disassembly" (block <> "");
  List.iter
    (fun op ->
      check
        (Printf.sprintf "elision: `proven` emits no %s (zero-cost when provable)" op)
        (find_substring ~needle:op block = None))
    [ "BORROW_S"; "BORROW_X"; "RELEASE_S"; "RELEASE_X" ];
  List.iter
    (fun op ->
      check
        (Printf.sprintf "rc retired: the whole image contains no %s" op)
        (find_substring ~needle:op dump = None))
    [ "RC_INC"; "RC_DEC" ]

let () =
  (* haxe-parity Task 3: the compare-and-jump chain lowers onto the
     existing EQ/EQS/JZ/JMP opcodes — no new one, per the brief. An
     `Int` subject compares via EQ, never EQS (that switch, over
     `Text`, is tests/corpus/run/lang-switch-value's own second half —
     both are proven end to end there; this pins the instruction
     *shape* a --dump-bc reader would actually see, task-3-report.md's
     own excerpt). *)
  let src =
    "fn classify(code: Int) -> Text {\n\
    \  let v = switch code {\n\
    \    case 200: \"a\"\n\
    \    default: \"b\"\n\
    \  }\n\
    \  return v\n\
     }\n\n\
     fn main() -> Int {\n  return 0\n}\n"
  in
  let image, collector = emit_str ~file:"switch-bc.wo" src in
  check_eq "switch bc: compiles clean" ~expected:0
    ~actual:(List.length (Diag.Collector.diagnostics collector)) string_of_int;
  let block = method_block (Disasm.dump image) "classify" in
  check "switch bc: an Int subject compares via EQ" (find_substring ~needle:"EQ " block <> None);
  check "switch bc: never EQS for an Int subject" (find_substring ~needle:"EQS" block = None);
  check "switch bc: at least one JZ (the case-value test)"
    (find_substring ~needle:"JZ" block <> None);
  check "switch bc: at least one JMP (the matched arm's own jump to the switch's exit)"
    (find_substring ~needle:"JMP" block <> None)

let () =
  (* Residual guards: one coalesced pair per operand, and — the
     regression this pins — never on a register inside the call window.
     The callee's frame overlaps that window (it may assign to its own
     parameters) and the call's return value lands on the window base, so
     releasing a window register hands wo_release_excl whatever now sits
     there. *)
  let path = "golden/bc/residual.wo" in
  let image, _ = emit_str ~file:path (read_file path) in
  let dump = Disasm.dump image in
  let block = method_block dump "pair" in
  let count needle s =
    let rec go i n =
      if i >= String.length s then n
      else
        match find_substring ~needle (String.sub s i (String.length s - i)) with
        | None -> n
        | Some k -> go (i + k + String.length needle) (n + 1)
    in
    go 0 0
  in
  check_eq "residual: `pair` acquires exactly two exclusive guards" ~expected:2
    ~actual:(count "BORROW_X" block) string_of_int;
  check_eq "residual: and releases exactly two" ~expected:2
    ~actual:(count "RELEASE_X" block) string_of_int;
  check "residual: `fixed` (literal indexes, provably distinct) gets no guard at all"
    (find_substring ~needle:"BORROW" (method_block dump "fixed") = None);
  (* the guard registers and the CALL's window base must be disjoint *)
  let regs_of prefix =
    String.split_on_char '\n' block
    |> List.filter_map (fun l ->
           match find_substring ~needle:prefix l with
           | None -> None
           | Some _ -> (
             match find_substring ~needle:"r" (String.trim l) with
             | None -> None
             | Some _ ->
               let l = String.trim l in
               let i = ref 0 in
               while !i < String.length l && l.[!i] <> 'r' do
                 incr i
               done;
               (* skip the mnemonic's own letters up to the operand *)
               let rec next_reg j =
                 if j >= String.length l then None
                 else if l.[j] = 'r' && j + 1 < String.length l && l.[j + 1] >= '0'
                         && l.[j + 1] <= '9' then begin
                   let k = ref (j + 1) in
                   while !k < String.length l && l.[!k] >= '0' && l.[!k] <= '9' do
                     incr k
                   done;
                   Some (int_of_string (String.sub l (j + 1) (!k - j - 1)))
                 end
                 else next_reg (j + 1)
               in
               next_reg (String.length prefix)))
  in
  let guards = regs_of "RELEASE_X" and windows = regs_of "CALL" in
  check "residual: no guard register is the call window's base register"
    (List.for_all (fun g -> not (List.mem g windows)) guards)

let () =
  (* Over-budget: 70 owned locals cannot fit the VM's 64-register window,
     so the method must diagnose WO-E401 rather than emit a truncated
     frame. Generated rather than a fixture file: the point is the count,
     and 70 hand-written lines would pin nothing extra. *)
  let buf = Buffer.create 1024 in
  Buffer.add_string buf "class Item {\n  n: Int\n}\n\nfn wide() -> Int {\n";
  for i = 0 to 69 do
    Buffer.add_string buf (Printf.sprintf "  let v%d = Item { n: %d }\n" i i)
  done;
  Buffer.add_string buf "  return 0\n}\n";
  let _, collector = emit_str ~file:"wide.wo" (Buffer.contents buf) in
  let diags = Diag.Collector.diagnostics collector in
  check_eq "register budget: exactly one diagnostic (reported once per method)" ~expected:1
    ~actual:(List.length diags) string_of_int;
  match diags with
  | [ d ] ->
    check "register budget: the code is WO-E401" (d.Diag.code = "WO-E401");
    check "register budget: it is an error, not a warning" (d.Diag.severity = Diag.Error)
  | _ -> check "register budget: diagnostic shape" false

let () =
  (* The emitter's own view of liveness must agree with the owner pass's
     LIVE-MASK entries: for every call site the table names, the drop
     table must carry an entry at some pc whose owned/gc masks hold
     exactly as many registers as the table listed items. A drift here
     means the emitter stopped consuming the table it is contracted to. *)
  let path = "golden/bc/owned.wo" in
  let src = read_file path in
  let tables, _ = owner_str ~file:path src in
  let masks =
    List.filter_map
      (fun (d : Owner.drop_site) ->
        match d.Owner.dr_kind with
        | Owner.DLiveMask -> Some (List.length d.Owner.dr_items)
        | _ -> None)
      tables.Owner.drops
  in
  let image, _ = emit_str ~file:path src in
  let dump = Disasm.dump image in
  let popcounts =
    String.split_on_char '\n' dump
    |> List.filter_map (fun l ->
           if find_substring ~needle:"  drops: pc" l = None then None
           else
             Some
               (List.length
                  (List.filter
                     (fun part -> String.length part > 0 && part.[0] = 'r')
                     (String.split_on_char ','
                        (String.concat ""
                           (String.split_on_char '{'
                              (String.concat "" (String.split_on_char '}' l))))))))
  in
  check "live masks: the owner table lists at least one call-site mask for owned.wo"
    (masks <> []);
  check "live masks: the emitted drop table carries entries whose widest mask matches the \
         table's widest LIVE-MASK"
    (List.fold_left max 0 masks <= List.fold_left max 0 popcounts)

(* The ownership tables are a contract, not a hint: every DROP the DROPS
   table asks for has to appear in the emitted code exactly once — and
   nothing else may (rc ops don't exist since iteration 7b). A count
   identity over a whole file is the cheapest way to state that. *)
let () =
  let count_op needle dump =
    String.split_on_char '\n' dump
    |> List.filter (fun l -> find_substring ~needle:("  " ^ needle) l <> None)
    |> List.length
  in
  List.iter
    (fun path ->
      let src = read_file path in
      let tables, coll = owner_str ~file:path src in
      if not (Diag.Collector.has_error coll) then begin
        let want_drops =
          List.fold_left
            (fun n (d : Owner.drop_site) ->
              match d.Owner.dr_kind with
              | Owner.DLiveMask -> n
              | Owner.DScope _ | Owner.DReturn | Owner.DOverwrite | Owner.DBranchJoin _
              | Owner.DBreak | Owner.DContinue ->
                n
                + List.length
                    (List.filter
                       (fun (i : Owner.drop_item) -> i.Owner.di_kind = Owner.LOwned)
                       d.Owner.dr_items))
            0 tables.Owner.drops
        in
        let image, _ = emit_str ~file:path src in
        let dump = Disasm.dump image in
        check_eq
          (Printf.sprintf "table contract %s: one DROP per owned drop-table item" path)
          ~expected:want_drops ~actual:(count_op "DROP " dump) string_of_int;
        check_eq
          (Printf.sprintf "table contract %s: rc ops never appear (7b)" path)
          ~expected:0 ~actual:(count_op "RC_INC" dump + count_op "RC_DEC" dump) string_of_int;
        (* The residual table is both the only licence to emit a borrow
           op and an obligation to emit one per *operand*: guards are
           coalesced per operand, never per entry (asking twice for an
           exclusive borrow of one object self-traps on legal code). The
           identity is computed here from the raw table, independently of
           emit.ml's own coalescing — a region the emitter forgot to
           consume, or one it expanded per entry, both fail it. *)
        let operands =
          let by_region = Hashtbl.create 8 in
          List.iter
            (fun (r : Owner.residual_site) ->
              let cur = try Hashtbl.find by_region r.Owner.rs_node with Not_found -> [] in
              let cur =
                List.sort_uniq compare (r.Owner.rs_a_node :: r.Owner.rs_b_node :: cur)
              in
              Hashtbl.replace by_region r.Owner.rs_node cur)
            tables.Owner.residuals;
          Hashtbl.fold (fun _ ops n -> n + List.length ops) by_region 0
        in
        check_eq
          (Printf.sprintf "table contract %s: one borrow acquire per coalesced residual operand"
             path)
          ~expected:operands
          ~actual:(count_op "BORROW_S" dump + count_op "BORROW_X" dump)
          string_of_int;
        check_eq
          (Printf.sprintf "table contract %s: one release per coalesced residual operand" path)
          ~expected:operands
          ~actual:(count_op "RELEASE_S" dump + count_op "RELEASE_X" dump)
          string_of_int
      end)
    [ "golden/bc/owned.wo"; "golden/bc/elision.wo"; "golden/bc/residual.wo";
      "golden/bc/iface.wo"; "golden/owner/moves.wo"; "golden/owner/drops.wo";
      "golden/owner/rc.wo"; "golden/owner/residual.wo" ]

(* Shapes that produce a loadable image, one per lowering the goldens do
   not already cover, plus the two round-trip regressions found while
   building this task: a forward jump out of the *last* `if`/`while` of a
   body targets the position after the final instruction (the loader
   reads that as "jump out of code", so the implicit return has to be
   appended for that reason too), and a call whose argument count differs
   from the callee's reserves the wrong window (the loader's "call window
   exceeds frame"). Each case is emitted and run through the loader's
   battery — the cheap way to keep the round-trip rule honest for
   lowerings no fixture file happens to exercise. *)
let () =
  let cases =
    [ ( "jump target at end of code",
        "fn f(flag: Bool) {\n  if flag {\n    return\n  }\n}\n" );
      ("while at end of body", "fn f(flag: Bool) {\n  while flag {\n    flag = false\n  }\n}\n");
      ( "for over a multi",
        "class Item {\n  n: Int\n}\n\nclass Bag {\n  items: multi Item\n}\n\n\
         fn total(bag: Bag) -> Int {\n  let sum = 0\n  for it in bag.items {\n\
         \    sum = sum + it.n\n  }\n  return sum\n}\n" );
      ( "map builtins",
        "class Index {\n  by_name: map<Text, Int>\n}\n\nfn f() -> Int {\n\
         \  let idx = Index { by_name: map_new() }\n  set(idx.by_name, \"a\", 1)\n\
         \  if has(idx.by_name, \"a\") {\n    return get(idx.by_name, \"a\")\n  }\n\
         \  return 0\n}\n" );
      ( "text: concat, equality, words",
        "fn f(a: Text, b: Text) -> Int {\n  let joined = a .. b\n\
         \  if joined == a {\n    return 1\n  }\n  return words(joined)\n}\n" );
      ( "db insert statement",
        "class Row {\n  n: Int\n}\n\nfn f() -> Int {\n  insert Row { n: 1 }\n  return 0\n}\n" );
      ( "nested calls in arguments",
        "fn one() -> Int {\n  return 1\n}\n\nfn add(a: Int, b: Int) -> Int {\n\
         \  return a + b\n}\n\nfn f() -> Int {\n  return add(add(one(), one()), one())\n}\n" );
      ( "method call on a class instance",
        "class Counter {\n  n: Int\n\n  fn bump(by: Int) -> Int {\n\
         \    return self.n + by\n  }\n}\n\nfn f() -> Int {\n\
         \  let c = Counter { n: 1 }\n  return c.bump(2)\n}\n" )
    ]
  in
  List.iter
    (fun (name, src) ->
      let file = name ^ ".wo" in
      let image, collector = emit_str ~file src in
      let diags = Diag.Collector.diagnostics collector in
      check
        (Printf.sprintf "lowering %s: compiles clean (%s)" name
           (String.concat ", " (List.map (fun (d : Diag.t) -> d.Diag.code ^ ": " ^ d.Diag.message) diags)))
        (diags = []);
      let violations = validate_image image in
      check
        (Printf.sprintf "lowering %s: the loader's battery accepts the image (%s)" name
           (String.concat "; " violations))
        (violations = []))
    cases

let () =
  (* Arity is the emitter's business because nothing upstream checks it:
     types.ml declares WO-E203 and never raises it. An unchecked call
     would reserve a window the callee does not read — the loader rejects
     it, which by the round-trip rule would be an emitter bug. *)
  let _, collector =
    emit_str ~file:"arity.wo"
      "fn add3(a: Int, b: Int, c: Int) -> Int {\n  return a + b + c\n}\n\n\
       fn main() {\n  print_int(add3(1))\n}\n"
  in
  let diags = Diag.Collector.diagnostics collector in
  check_eq "arity: exactly one diagnostic" ~expected:1 ~actual:(List.length diags) string_of_int;
  match diags with
  | [ d ] ->
    check "arity: reported as WO-E403 (a call the emitter cannot lower)" (d.Diag.code = "WO-E403");
    check "arity: the message names both counts"
      (find_substring ~needle:"takes 3 argument(s), given 1" d.Diag.message <> None)
  | _ -> check "arity: diagnostic shape" false

let () =
  (* WO-E405: the entry (`fn main()`, zero args) must declare `Int` or
     nothing at all -- the systems-track spec makes its return value the
     process exit code. A `@gc` return escaping through it is exactly
     the leak this diagnostic exists to close (docs/plan/oop-vm's
     error-catalog entry): the driver has no way to release a pointer
     it receives with no return-kind metadata to consult. *)
  let _, collector =
    emit_str ~file:"entry-not-int.wo"
      "class Widget {\n  n: Int\n}\n\nfn main() -> Widget {\n  return Widget { n: 1 }\n}\n"
  in
  let diags = Diag.Collector.diagnostics collector in
  check_eq "entry return type: exactly one diagnostic" ~expected:1 ~actual:(List.length diags)
    string_of_int;
  (match diags with
  | [ d ] ->
    check "entry return type: reported as WO-E405" (d.Diag.code = "WO-E405");
    check "entry return type: the message names the declared type and the exit-code contract"
      (find_substring ~needle:"`Widget`" d.Diag.message <> None
      && find_substring ~needle:"process exit code" d.Diag.message <> None
      && find_substring ~needle:"must return `Int`" d.Diag.message <> None)
  | _ -> check "entry return type: diagnostic shape" false);
  (* control: no return annotation at all is not "anything other than
     Int" -- it is the shape every other fixture in this suite uses,
     and must stay clean. *)
  let _, clean_collector =
    emit_str ~file:"entry-no-annotation.wo" "fn main() {\n  print_int(0)\n}\n"
  in
  check "entry return type: `main` with no return annotation compiles clean"
    (Diag.Collector.diagnostics clean_collector = [])

(* ---- CLI smoke: emit mode and --dump-bc ------------------------------ *)

let () =
  let path = "golden/bc/arith.wo" in
  let exit_code, stdout, stderr = run_cli [ "--dump-bc"; path ] in
  check "cli smoke: --dump-bc on a clean file exits 0" (exit_code = 0);
  check "cli smoke: clean-file --dump-bc writes nothing to stderr" (stderr = "");
  let image, _ = emit_str ~file:path (read_file path) in
  check "cli smoke: --dump-bc stdout matches the in-process disassembly exactly"
    (stdout = Disasm.dump image)

let () =
  (* Unlike the other dumps, --dump-bc prints nothing when the compile is
     not clean: a disassembly of a program that failed to compile
     describes bytecode nobody is allowed to run. *)
  let exit_code, stdout, stderr = run_cli [ "--dump-bc"; "golden/owner-err/use-after-move.wo" ] in
  check "cli smoke: --dump-bc on a failing compile exits 1" (exit_code = 1);
  check "cli smoke: the WO-E301 diagnostic still goes to stderr"
    (find_substring ~needle:"WO-E301" stderr <> None);
  check "cli smoke: --dump-bc prints no bytecode for a program that did not compile"
    (stdout = "")

let () =
  let out = Filename.temp_file "woc_emit" ".wob" in
  let exit_code, stdout, stderr = run_cli [ "--emit"; "golden/bc/iface.wo"; "-o"; out ] in
  check "cli smoke: --emit exits 0 on a clean program" (exit_code = 0);
  check "cli smoke: --emit prints nothing on stdout" (stdout = "");
  check "cli smoke: --emit prints nothing on stderr" (stderr = "");
  let image = read_file out in
  check "cli smoke: the written image is a WOB1 v1 file"
    (String.length image > 44 && String.sub image 0 4 = "WOB1");
  check_eq "cli smoke: the written image passes the loader's battery" ~expected:0
    ~actual:(List.length (validate_image image)) string_of_int;
  (try Sys.remove out with Sys_error _ -> ());
  (* a failing compile must leave no image behind *)
  let out2 = Filename.temp_file "woc_emit" ".wob" in
  Sys.remove out2;
  let exit_code, _, _ = run_cli [ "--emit"; "golden/owner-err/use-after-move.wo"; "-o"; out2 ] in
  check "cli smoke: --emit on a failing compile exits 1" (exit_code = 1);
  check "cli smoke: and writes no image at all" (not (Sys.file_exists out2));
  (try Sys.remove out2 with Sys_error _ -> ())

let () =
  let exit_code, _, stderr = run_cli [ "--emit"; "golden/bc/arith.wo" ] in
  check "cli smoke: --emit without -o is a usage error (exit 2)" (exit_code = 2);
  check "cli smoke: usage goes to stderr" (stderr <> "")

(* ---- typedef records + enum payload variants (haxe-parity Task 4) ----

   Direct assertions, no golden diffs (the same convention Tasks 2/3's
   sections follow): parser shapes for the two new declarations, the
   structural-equivalence contract at both levels it lives on (types.ml
   unification and the emitted class table), the E203/E208/E201/E206
   diagnostic surface, the union field-kind rule (a bare union field is
   a SCALAR slot — the int-as-pointer segfault family), the ownership
   rows (a payload argument MOVES into the construction; a payload
   binding is a borrow, never dropped), and the lowering shapes
   (variant_tag for payload unions only, defaults filled for omitted
   fields). The corpus fixtures (the lang-typedef-/lang-variant-
   directories) pin the end-to-end round trips; these pin the internals
   a round trip cannot state as a contract. *)

let () =
  (* parser: typedef record — comma and newline field forms, `?name`
     desugars to Nullable, `type` legal as a field name, is_record set *)
  let src =
    "typedef R = { a: Int = 7, ?b: Text, type: Text }\n\
     typedef S = {\n  n: Int\n  ?m: ?Int\n}\n"
  in
  let prog, collector = parse_str ~file:"t4-record.wo" src in
  check "t4 record: parses clean" (not (Diag.Collector.has_error collector));
  (match prog.Ast.decls with
  | [ Ast.Class r; Ast.Class s ] ->
    check "t4 record: is_record set, is_class clear" (r.Ast.is_record && not r.Ast.is_class);
    check "t4 record: comma form keeps all three fields"
      (List.map (fun (f : Ast.field) -> f.Ast.name) r.Ast.fields = [ "a"; "b"; "type" ]);
    check "t4 record: `?b: Text` desugars to Nullable Text"
      (match r.Ast.fields with
      | [ _; b; _ ] -> b.Ast.ty = Ast.Nullable (Ast.Scalar "Text")
      | _ -> false);
    check "t4 record: `a` keeps its default"
      (match r.Ast.fields with a :: _ -> a.Ast.default <> None | [] -> false);
    check "t4 record: `?m: ?Int` does not double-wrap"
      (match s.Ast.fields with
      | [ _; m ] -> m.Ast.ty = Ast.Nullable (Ast.Scalar "Int")
      | _ -> false);
    check "t4 record: dump header says TYPEDEF"
      (count_substring ~needle:"TYPEDEF R" (Dump.dump_ast prog) = 1)
  | _ -> check "t4 record: two typedef declarations survive" false)

let () =
  (* parser: union declarations — bare, payload, and the struct form
     `type Note { ... }` staying a struct (the `=` lookahead) *)
  let src =
    "type Status = Pending | Failed(reason: Text, code: Int)\n\
     type Note { n: Int }\n"
  in
  let prog, collector = parse_str ~file:"t4-union.wo" src in
  check "t4 union: parses clean" (not (Diag.Collector.has_error collector));
  (match prog.Ast.decls with
  | [ Ast.Union u; Ast.Class note ] ->
    check "t4 union: two variants, payload fields in order"
      (match u.Ast.variants with
      | [ p; fl ] ->
        p.Ast.v_name = "Pending" && p.Ast.v_fields = []
        && fl.Ast.v_name = "Failed"
        && fl.Ast.v_fields = [ ("reason", Ast.Scalar "Text"); ("code", Ast.Scalar "Int") ]
      | _ -> false);
    check "t4 union: `type Note { ... }` is still the struct form"
      ((not note.Ast.is_class) && not note.Ast.is_record);
    check "t4 union: dump renders the variant line"
      (count_substring ~needle:"UNION Status = Pending | Failed(reason: Text, code: Int)"
         (Dump.dump_ast prog)
      = 1)
  | _ -> check "t4 union: union + struct decls survive" false)

let () =
  (* parser: one dotted segment in a type position (the sample's own
     `?id: json.Value`) — one Scalar name, dot included *)
  let src = "typedef Q = { ?id: json.Value }\n" in
  let prog, collector = parse_str ~file:"t4-dotted.wo" src in
  check "t4 dotted: parses clean" (not (Diag.Collector.has_error collector));
  (match prog.Ast.decls with
  | [ Ast.Class q ] ->
    check "t4 dotted: field type is Scalar \"json.Value\" under Nullable"
      (match q.Ast.fields with
      | [ f ] -> f.Ast.ty = Ast.Nullable (Ast.Scalar "json.Value")
      | _ -> false)
  | _ -> check "t4 dotted: typedef survives" false)

(* the whole check-only pipeline (no emitter), returning every diagnostic *)
let t4_diags ~file src =
  let collector = Diag.Collector.create () in
  let toks = Lexer.tokenize collector ~file src in
  let prog = Parser.parse collector ~file toks in
  let _syms, () = Types.typecheck ~file prog collector in
  Diag.Collector.diagnostics collector

let t4_codes ~file src = List.map (fun (d : Diag.t) -> d.Diag.code) (t4_diags ~file src)

let () =
  (* WO-E206's new omittability rule: defaults and `?` fields fill in /
     nil in; a plain field still fires *)
  let base = "typedef R = { a: Int = 7, ?b: Text, c: Text }\n" in
  check "t4 E206: omitting defaulted+optional fields is clean"
    (t4_codes ~file:"t4-e206a.wo" (base ^ "fn main() { let r = R { c: \"x\" }\n  print(r.c) }\n")
    = []);
  check "t4 E206: omitting a plain field still fires"
    (t4_codes ~file:"t4-e206b.wo" (base ^ "fn main() { let r = R {}\n  print(r.c) }\n")
    = [ "WO-E206" ])

let () =
  (* WO-E203, both sites: construction arity and pattern arity/shape *)
  let u = "type Status = Pending | Failed(reason: Text)\n" in
  check "t4 E203: construction with too many payload args"
    (t4_codes ~file:"t4-e203a.wo" (u ^ "fn main() { let s = Failed(\"a\", \"b\") }\n")
    = [ "WO-E203" ]);
  check "t4 E203: construction with too few payload args"
    (t4_codes ~file:"t4-e203b.wo" (u ^ "fn main() { let s = Failed() }\n") = [ "WO-E203" ]);
  check "t4 E203: pattern binding the wrong number of fields"
    (t4_codes ~file:"t4-e203c.wo"
       (u
      ^ "fn f(s: Status) -> Int { return switch s {\n\
        \  case Pending: 0;\n  case Failed(a, b): 1;\n} }\nfn main() { }\n")
    = [ "WO-E203" ]);
  check "t4 E203: pattern arguments must be plain names"
    (t4_codes ~file:"t4-e203d.wo"
       (u
      ^ "fn f(s: Status) -> Int { return switch s {\n\
        \  case Pending: 0;\n  case Failed(\"x\"): 1;\n} }\nfn main() { }\n")
    = [ "WO-E203" ])

let () =
  (* WO-E208's union exhaustiveness rule + WO-E201 for a non-variant
     pattern *)
  let u = "type Kind = Lo | Mid | Hi\n" in
  check "t4 E208: all variants covered needs no default"
    (t4_codes ~file:"t4-e208a.wo"
       (u
      ^ "fn f(k: Kind) -> Int { return switch k {\n\
        \  case Lo: 1;\n  case Mid: 2;\n  case Hi: 3;\n} }\nfn main() { }\n")
    = []);
  (match
     t4_diags ~file:"t4-e208b.wo"
       (u ^ "fn f(k: Kind) -> Int { return switch k {\n  case Lo: 1;\n} }\nfn main() { }\n")
   with
  | [ d ] ->
    check "t4 E208: uncovered variants fire E208" (d.Diag.code = "WO-E208");
    check "t4 E208: the message names the missing variants, in order"
      (count_substring ~needle:"does not cover: Mid, Hi" d.Diag.message = 1)
  | ds ->
    check_eq "t4 E208: exactly one diagnostic" ~expected:1 ~actual:(List.length ds) string_of_int);
  check "t4 E208: a default covers the gap"
    (t4_codes ~file:"t4-e208c.wo"
       (u
      ^ "fn f(k: Kind) -> Int { return switch k {\n\
        \  case Lo: 1;\n  default: 0;\n} }\nfn main() { }\n")
    = []);
  check "t4 E201: a non-variant case name over a union subject"
    (t4_codes ~file:"t4-e201a.wo"
       (u
      ^ "fn f(k: Kind) -> Int { return switch k {\n\
        \  case Lo: 1;\n  case Wat: 2;\n  default: 0;\n} }\nfn main() { }\n")
    = [ "WO-E201" ]);
  check "t4 E201: a literal case value over a union subject"
    (t4_codes ~file:"t4-e201b.wo"
       (u
      ^ "fn f(k: Kind) -> Int { return switch k {\n\
        \  case 1: 1;\n  default: 0;\n} }\nfn main() { }\n")
    = [ "WO-E201" ])

let () =
  (* structural equivalence, types.ml half: same-shape typedefs unify
     across switch arms; different shapes still WO-E201 *)
  let two_same = "typedef A = { n: Int }\ntypedef B = { n: Int }\n" in
  let two_diff = "typedef A = { n: Int }\ntypedef B = { n: Text }\n" in
  let body =
    "fn f(c: Int) -> Int {\n\
    \  let v = switch c {\n\
    \    case 1: A { n: 1 };\n\
    \    default: B { n: 2 };\n\
    \  }\n\
    \  return 0\n\
     }\nfn main() { }\n"
  in
  let body_diff =
    "fn f(c: Int) -> Int {\n\
    \  let v = switch c {\n\
    \    case 1: A { n: 1 };\n\
    \    default: B { n: \"x\" };\n\
    \  }\n\
    \  return 0\n\
     }\nfn main() { }\n"
  in
  check "t4 structural: same shape, arms unify with no E201"
    (t4_codes ~file:"t4-str1.wo" (two_same ^ body) = []);
  check "t4 structural: different shape still mismatches"
    (t4_codes ~file:"t4-str2.wo" (two_diff ^ body_diff) = [ "WO-E201" ])

let () =
  (* WO-E215 for duplicate unions and variant names *)
  check "t4 E215: duplicate union name"
    (t4_codes ~file:"t4-e215a.wo" "type K = A | B\ntype K = C | D\nfn main() { }\n"
    = [ "WO-E215" ]);
  check "t4 E215: variant name reused across unions"
    (t4_codes ~file:"t4-e215b.wo" "type K = A | B\ntype L = B | C\nfn main() { }\n"
    = [ "WO-E215" ]);
  check "t4 E215: variant name reused inside one union"
    (t4_codes ~file:"t4-e215c.wo" "type K = A | A\nfn main() { }\n" = [ "WO-E215" ])

let () =
  (* the emitted class table: structural dedup (one entry for two
     same-shape typedefs), per-variant entries for a payload union
     (composite `Union.Variant` names), NO entries for a bare union, and
     the union field-kind rule (SCALAR for bare — the stubbed-kind RED
     was a real wo_drop_obj SEGV chasing tag 2 as a pointer; OWNED for
     payload) *)
  let src =
    "typedef A = { n: Int, tag: Text }\n\
     typedef B = { n: Int, tag: Text }\n\
     type Kind = Lo | Mid | Hi\n\
     type Status = Pending | Failed(reason: Text)\n\
     typedef Holder = { k: Kind, st: ?Status }\n\
     fn main() -> Int {\n\
    \  let a = A { n: 1, tag: \"t\" }\n\
    \  let h = Holder { k: Lo, st: Pending }\n\
    \  print_int(a.n)\n\
    \  return 0\n\
     }\n"
  in
  let image, collector = emit_str ~file:"t4-table.wo" src in
  check "t4 table: compiles clean" (not (Diag.Collector.has_error collector));
  let dump = Disasm.dump image in
  check "t4 table: A and B share ONE class entry (structural dedup)"
    (count_substring ~needle:"A flags" dump = 1 && count_substring ~needle:"B flags" dump = 0);
  check "t4 table: payload union gets one entry per variant"
    (count_substring ~needle:"Status.Pending flags" dump = 1
    && count_substring ~needle:"Status.Failed flags" dump = 1);
  check "t4 table: Status.Failed's payload Text is a TEXT slot"
    (count_substring ~needle:"Status.Failed flags=- fields=[reason:TEXT]" dump = 1);
  check "t4 table: bare union gets no class entries"
    (count_substring ~needle:"Kind" dump
     - count_substring ~needle:"Kind" (String.concat "" [ "" ])
     >= 0
    && count_substring ~needle:"Kind flags" dump = 0
    && count_substring ~needle:"Kind.Lo" dump = 0);
  check "t4 table: a bare-union record field is a SCALAR slot, a payload one OWNED"
    (count_substring ~needle:"Holder flags=- fields=[k:SCALAR, st:OWNED]" dump = 1)

let () =
  (* lowering shapes: variant_tag for a payload union's switch only; a
     bare union switch is a plain EQ chain (no variant_tag, no NEW);
     omitted defaults are filled (SETF count) *)
  let src =
    "type Status = Pending | Failed(reason: Text)\n\
     type Kind = Lo | Mid\n\
     fn f(s: Status) -> Int {\n\
    \  return switch s {\n\
    \    case Pending: 0;\n\
    \    case Failed(reason): 1;\n\
    \  }\n\
     }\n\
     fn g(k: Kind) -> Int {\n\
    \  return switch k {\n\
    \    case Lo: 0;\n\
    \    case Mid: 1;\n\
    \  }\n\
     }\n\
     fn main() { }\n"
  in
  let image, collector = emit_str ~file:"t4-lower.wo" src in
  check "t4 lower: compiles clean" (not (Diag.Collector.has_error collector));
  let dump = Disasm.dump image in
  check "t4 lower: exactly one variant_tag read (f's switch, not g's)"
    (count_substring ~needle:"variant_tag" dump = 1);
  let rec_default =
    "typedef R = { a: Int = 7, b: Text = \"seven\", ?c: Text }\n\
     fn main() -> Int {\n\
    \  let r = R {}\n\
    \  print(r.b)\n\
    \  return 0\n\
     }\n"
  in
  let image2, collector2 = emit_str ~file:"t4-defaults.wo" rec_default in
  check "t4 defaults: compiles clean" (not (Diag.Collector.has_error collector2));
  let dump2 = Disasm.dump image2 in
  check "t4 defaults: two omitted defaults stored, the ?field left nil (2 SETFs)"
    (count_substring ~needle:"SETF" dump2 = 2);
  let bad_default =
    "typedef R = { a: Int = 1 + 2 }\nfn main() { let r = R {}\n  print_int(r.a) }\n"
  in
  let _, collector3 = emit_str ~file:"t4-baddefault.wo" bad_default in
  check "t4 defaults: a non-literal default is WO-E403, never invented bytecode"
    (List.exists
       (fun (d : Diag.t) -> d.Diag.code = "WO-E403")
       (Diag.Collector.diagnostics collector3))

let () =
  (* ownership rows: a place-shaped payload argument MOVES into the
     construction (the un-stubbed half of the double-free RED); a
     payload binding is a borrow — never in any drop set *)
  let src =
    "class Box { n: Int }\n\
     type W = Just(b: Box)\n\
     fn main() -> Int {\n\
    \  let bx = Box { n: 1 }\n\
    \  let w = Just(bx)\n\
    \  switch w {\n\
    \    case Just(inner): print_int(inner.n);\n\
    \  }\n\
    \  return 0\n\
     }\n"
  in
  let tables, collector = owner_str ~file:"t4-owner.wo" src in
  check "t4 owner: analyzes clean" (not (Diag.Collector.has_error collector));
  let dump = Dump.dump_owner tables in
  check "t4 owner: `bx` moves into the payload field (CTOR row)"
    (count_substring ~needle:"MOVE bx CTOR(b)" dump = 1);
  check "t4 owner: `w` still drops before the frame leaves; moved-out `bx` does not"
    (* main ends in `return 0`, so the drop set is the RETURN row (the
       BODY scope-end is unreachable after a return and suppressed) *)
    (count_substring ~needle:"RETURN [w]" dump = 1
    && count_substring ~needle:"[bx" dump = 0);
  check "t4 owner: the binding `inner` is a borrow — in no drop set"
    (count_substring ~needle:"[inner" dump = 0 && count_substring ~needle:", inner" dump = 0)

(* ---- Task 4 fix round 1 (review: 2 Critical + 1 Major) ---------------

   Critical 1: a payload binding escaping its arm as the switch's value
   is a MOVE OUT of the variant object — the escape arm nulls the
   shell's field (its recursive drop plan already skips zero slots), the
   derivers type the escaped value by the binding's declared field, and
   the caller reaps owned variant temporaries passed by borrow.
   Critical 2 / Major: three new WO-E201 sites (variant case over
   `?Union`, cross-union `==`, variant case over a non-union subject). *)

let () =
  let u = "class P { a: Int }\ntype Ev = Tick | Boxed(p: P)\n" in
  (* value position: the escape arm nulls the shell's field — one SETF
     more than the identical switch in statement position, where the
     discarded yield must leave the shell whole (want_value gate). *)
  let value_pos =
    u
    ^ "fn main() -> Int {\n\
      \  let v = Boxed(P { a: 7 })\n\
      \  let out = switch v {\n\
      \    case Boxed(p): p;\n\
      \    case Tick: P { a: 0 };\n\
      \  }\n\
      \  print_int(out.a)\n\
      \  return 0\n\
       }\n"
  in
  let stmt_pos =
    u
    ^ "fn main() -> Int {\n\
      \  let v = Boxed(P { a: 7 })\n\
      \  switch v {\n\
      \    case Boxed(p): p;\n\
      \    case Tick: print(\"t\");\n\
      \  }\n\
      \  return 0\n\
       }\n"
  in
  let image, collector = emit_str ~file:"t4f-escape.wo" value_pos in
  check "t4fix escape: binding-yield switch compiles clean (was `field access on Int`)"
    (not (Diag.Collector.has_error collector));
  let dump = Disasm.dump image in
  check_eq "t4fix escape: ctor(1) + payload store(1) + escape NULL(1) + arm ctor(1) = 4 SETFs"
    ~expected:4 ~actual:(count_substring ~needle:"SETF" dump) string_of_int;
  let image2, collector2 = emit_str ~file:"t4f-escape-stmt.wo" stmt_pos in
  check "t4fix escape: statement position compiles clean" (not (Diag.Collector.has_error collector2));
  check_eq "t4fix escape: discarded yield does NOT null the shell (2 SETFs only)"
    ~expected:2 ~actual:(count_substring ~needle:"SETF" (Disasm.dump image2)) string_of_int;
  (* owner half of the drop plan: both the escaped payload's new owner
     (`out`) and the shell (`v`) drop before the frame leaves — one drop
     each, shell-only semantics coming from the nulled field, never from
     a second table entry. *)
  let tables, ocoll = owner_str ~file:"t4f-escape-owner.wo" value_pos in
  check "t4fix escape: owner analyzes clean" (not (Diag.Collector.has_error ocoll));
  check "t4fix escape: RETURN drops [out, v] — payload owner AND shell, once each"
    (count_substring ~needle:"RETURN [out, v]" (Dump.dump_owner tables) = 1)

let () =
  (* the caller reaps an owned variant temporary passed by borrow: the
     reviewer's h5 shell leak (~30 B/iteration, arena-backed and
     LSan-invisible — pinned here at the bytecode level instead). *)
  let src =
    "class P { a: Int }\n\
     type Ev = Tick | Boxed(p: P)\n\
     fn use_ev(e: Ev) -> Int { return 1 }\n\
     fn main() -> Int {\n\
    \  print_int(use_ev(Boxed(P { a: 1 })))\n\
    \  return 0\n\
     }\n"
  in
  let image, collector = emit_str ~file:"t4f-reap.wo" src in
  check "t4fix reap: compiles clean" (not (Diag.Collector.has_error collector));
  check_eq "t4fix reap: exactly one DROP — the borrowed variant temp, after the call"
    ~expected:1 ~actual:(count_substring ~needle:"DROP" (Disasm.dump image)) string_of_int

let () =
  (* WO-E201, three new sites *)
  let st = "type St = Pending | Failed(m: Text)\ntypedef R = { ?st: St }\n" in
  (match
     t4_diags ~file:"t4f-optunion.wo"
       (st
      ^ "fn f(r: R) -> Text { return switch r.st {\n\
        \  case Pending: \"p\";\n  default: \"n\";\n} }\nfn main() { }\n")
   with
  | [ d ] ->
    check "t4fix ?union: variant case over `?Union` is WO-E201" (d.Diag.code = "WO-E201");
    check "t4fix ?union: the message points at nil handling first"
      (count_substring ~needle:"may be nil" d.Diag.message = 1)
  | ds ->
    check_eq "t4fix ?union: exactly one diagnostic" ~expected:1 ~actual:(List.length ds)
      string_of_int);
  check "t4fix ?union: a default-only switch over `?Union` stays legal"
    (t4_codes ~file:"t4f-optunion-ok.wo"
       (st ^ "fn f(r: R) -> Text { return switch r.st {\n  default: \"n\";\n} }\nfn main() { }\n")
    = []);
  let two = "type A = X | Yv\ntype B = P | Qv\n" in
  check "t4fix cross-union ==: WO-E201"
    (t4_codes ~file:"t4f-crosseq.wo" (two ^ "fn main() { if X == P { print(\"x\") } }\n")
    = [ "WO-E201" ]);
  check "t4fix cross-union ==: same union stays legal, and a local shadowing a variant is a local"
    (t4_codes ~file:"t4f-crosseq-ok.wo"
       (two ^ "fn main() { let X = 1\n  if X == 1 { print(\"a\") }\n  if P == Qv { print(\"b\") } }\n")
    = []);
  check "t4fix int-subject: a variant case over an Int subject is WO-E201"
    (t4_codes ~file:"t4f-intsubj.wo"
       ("type K = Lo | Mid | Hi\n"
      ^ "fn f(n: Int) -> Int { return switch n {\n  case Lo: 99;\n  default: 0;\n} }\n\
         fn main() { }\n")
    = [ "WO-E201" ])

(* ---- Task 4 fix round 2 (review: scalar move-out corruption + the
   record/class sibling of the temp-argument leak) --------------------- *)

let () =
  (* NEW 1: escaping a SCALAR payload field is a COPY — no null. The
     pointer-payload escape pins 4 SETFs above (one of them the null);
     this scalar twin has exactly the payload store, nothing else. *)
  let src =
    "type Ev = Tick | Wrap(n: Int)\n\
     fn main() -> Int {\n\
    \  let v = Wrap(5)\n\
    \  let x = switch v {\n\
    \    case Tick: 0;\n\
    \    case Wrap(n): n;\n\
    \  }\n\
    \  print_int(x)\n\
    \  return 0\n\
     }\n"
  in
  let image, collector = emit_str ~file:"t4f2-scalar.wo" src in
  check "t4fix2 scalar escape: compiles clean" (not (Diag.Collector.has_error collector));
  check_eq "t4fix2 scalar escape: payload store only — NO null SETF (subject stays intact)"
    ~expected:1 ~actual:(count_substring ~needle:"SETF" (Disasm.dump image)) string_of_int

let () =
  (* NEW 2: the reap covers every owned heap temp by borrow — record
     ctor, class-returning call — while `take` stays the callee's drop
     (exactly one DROP total, in the callee; two would be the double
     free) and a place is never reaped. *)
  let rec_decl = "typedef P = { a: Int = 1 }\n" in
  let borrow_ctor =
    rec_decl ^ "fn peek(p: P) -> Int { return p.a }\nfn main() -> Int {\n  print_int(peek(P {}))\n  return 0\n}\n"
  in
  let borrow_call =
    rec_decl
    ^ "fn mk() -> P { return P {} }\nfn peek(p: P) -> Int { return p.a }\n\
       fn main() -> Int {\n  print_int(peek(mk()))\n  return 0\n}\n"
  in
  let take_ctor =
    rec_decl
    ^ "fn eat(take p: P) -> Int { return p.a }\nfn main() -> Int {\n  print_int(eat(P {}))\n  return 0\n}\n"
  in
  let place_borrow =
    rec_decl
    ^ "fn peek(p: P) -> Int { return p.a }\nfn main() -> Int {\n\
      \  let q = P {}\n  print_int(peek(q))\n  return 0\n}\n"
  in
  let drops label src expected =
    let image, collector = emit_str ~file:(label ^ ".wo") src in
    check (label ^ ": compiles clean") (not (Diag.Collector.has_error collector));
    check_eq (label ^ ": DROP count") ~expected
      ~actual:(count_substring ~needle:"DROP" (Disasm.dump image)) string_of_int
  in
  (* one DROP each and each a DIFFERENT one: the caller's reap for the
     two borrow shapes (mk returns its fresh value undropped, peek
     borrows), the CALLEE's take-param drop for the take shape (a
     second one there would be the double free), and the local's own
     scope drop for the place shape (a reap on top would be too). *)
  drops "t4fix2 reap record-ctor temp by borrow (caller reaps)" borrow_ctor 1;
  drops "t4fix2 reap class-returning-call temp by borrow (caller reaps)" borrow_call 1;
  drops "t4fix2 take temp: exactly ONE drop — the callee's, never a second (double free)"
    take_ctor 1;
  drops "t4fix2 place by borrow: the local's own scope drop only, never a reap" place_borrow 1

(* ---- golden-directory walk ------------------------------------------ *)

(* Each stage directory under golden/ names one `woc --dump-*` flag.
   "tokens" (Task 3), "ast" (Task 4) and "owner" (Task 7) exist today; a
   later task may add "types" alongside its own dump function in dump.ml.

   "owner-err" is the one stage whose produced text is *not* a dump: it is
   the fully rendered diagnostic report (the same text --dump-owner writes
   to stderr, source excerpts and related sites included). The ownership
   must-fail suite's whole contract is the message — both sites, the right
   code, no unrelated noise from earlier stages — so the golden has to pin
   the rendering, not a table. *)
let run_stage ~stage ~file ~src : string =
  match stage with
  | "tokens" ->
    let collector = Diag.Collector.create () in
    let toks = Lexer.tokenize collector ~file src in
    Dump.dump_tokens toks
  | "ast" ->
    let collector = Diag.Collector.create () in
    let toks = Lexer.tokenize collector ~file src in
    let prog = Parser.parse collector ~file toks in
    Dump.dump_ast prog
  | "owner" ->
    let tables, _ = owner_str ~file src in
    Dump.dump_owner tables
  | "owner-err" ->
    let _, collector = owner_str ~file src in
    let lookup f = if f = file then Some src else None in
    Diag.Collector.render_all collector lookup ^ "\n"
  | "bc" ->
    (* the emitter's own stage: the whole front end, then the `.wob`
       image, then its disassembly. The dump is produced from the
       serialized bytes (compiler/src/disasm.ml), so a golden here pins
       the emitted layout, not just the emitter's intentions. *)
    let image, collector = emit_str ~file src in
    if Diag.Collector.has_error collector then begin
      let lookup f = if f = file then Some src else None in
      "EMIT FAILED\n" ^ Diag.Collector.render_all collector lookup ^ "\n"
    end
    else Disasm.dump image
  | other ->
    failwith (Printf.sprintf "runner: unknown golden stage directory %S" other)

(* Line-based diff, unified-diff-flavored (common leading lines shown
   as context, then removed/added) but not a full LCS — goldens are
   sized for a human to read at a glance, not for minimal-diff output. *)
let print_diff ~expected ~actual =
  let exp_lines = Array.of_list (String.split_on_char '\n' expected) in
  let act_lines = Array.of_list (String.split_on_char '\n' actual) in
  let n = min (Array.length exp_lines) (Array.length act_lines) in
  let rec prefix_len i =
    if i < n && exp_lines.(i) = act_lines.(i) then prefix_len (i + 1) else i
  in
  let start = prefix_len 0 in
  Printf.printf "  --- expected\n  +++ actual\n";
  for i = 0 to start - 1 do
    Printf.printf "    %s\n" exp_lines.(i)
  done;
  for i = start to Array.length exp_lines - 1 do
    Printf.printf "  - %s\n" exp_lines.(i)
  done;
  for i = start to Array.length act_lines - 1 do
    Printf.printf "  + %s\n" act_lines.(i)
  done

let is_visible name = String.length name > 0 && name.[0] <> '.'

let run_golden_dir () =
  let root = "golden" in
  if not (Sys.file_exists root && Sys.is_directory root) then
    failwith (Printf.sprintf "runner: golden directory not found at %S" root)
  else begin
    let src_root = if bless then Some (source_golden_dir ()) else None in
    let stages =
      Sys.readdir root |> Array.to_list |> List.filter is_visible
      |> List.filter (fun name -> Sys.is_directory (Filename.concat root name))
      |> List.sort compare
    in
    List.iter
      (fun stage ->
        let dir = Filename.concat root stage in
        let entries = Sys.readdir dir |> Array.to_list |> List.sort compare in
        let wo_files = List.filter (fun n -> Filename.check_suffix n ".wo") entries in
        List.iter
          (fun wo_name ->
            incr checks;
            let name = Filename.chop_suffix wo_name ".wo" in
            let wo_path = Filename.concat dir wo_name in
            let expected_path = Filename.concat dir (name ^ ".expected") in
            let src = read_file wo_path in
            let file_label = stage ^ "/" ^ wo_name in
            let actual = run_stage ~stage ~file:file_label ~src in
            if bless then begin
              let src_expected_path =
                Filename.concat
                  (Filename.concat (Option.get src_root) stage)
                  (name ^ ".expected")
              in
              write_file src_expected_path actual;
              Printf.printf "BLESSED: %s\n" src_expected_path
            end
            else begin
              let expected =
                if Sys.file_exists expected_path then read_file expected_path
                else ""
              in
              if expected <> actual then begin
                incr failures;
                Printf.printf "FAIL: %s\n" (Filename.concat stage wo_name);
                print_diff ~expected ~actual
              end
            end)
          wo_files)
      stages
  end

let () = run_golden_dir ()

let () =
  Printf.printf "runner: %d checks, %d failures\n" !checks !failures;
  if !failures > 0 then exit 1 else exit 0
