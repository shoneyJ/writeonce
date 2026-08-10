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
      | Some { Ast.table_name = Some "prices"; indexes = [ [ "sku"; "at" ] ] } -> true
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
        List.iter visit_method c.methods
      | Ast.Interface i ->
        check
          (Printf.sprintf "node ids: interface `%s` id < all its method ids" i.name)
          (List.for_all (fun (s : Ast.method_sig) -> i.id < s.id) i.methods);
        add i.id;
        List.iter visit_sig i.methods
      | Ast.Fn m -> visit_method m)
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
  (* The brief's stated asymmetry: `insert` is a statement-only trigger
     (parser.ml's is_insert_trigger, checked only in parse_stmt) — a
     bare `insert` reached from parse_primary is just an ordinary
     identifier reference, exactly like self/me/on/service/policy's own
     "recognized positionally, not a reserved word" rule (this task's
     own keyword-discipline note). `select` (is_select_trigger) is
     checked unconditionally *inside* parse_primary, so the same
     position always builds a DbStub instead. Neither is an error on
     its own — the difference shows up in which Ast.expr_kind comes
     back. *)
  let prog, collector =
    parse_str ~file:"insert-vs-select.wo" "fn f() {\n  let a = insert\n  let b = select\n}\n"
  in
  check_eq "insert vs. select as bare expressions: no diagnostics" ~expected:0
    ~actual:(List.length (Diag.Collector.diagnostics collector))
    string_of_int;
  match prog.Ast.decls with
  | [ Ast.Fn m ] -> (
    match m.body with
    | [
     { Ast.s_kind = Ast.Let { name = "a"; value = a_val; _ }; _ };
     { Ast.s_kind = Ast.Let { name = "b"; value = b_val; _ }; _ };
    ] ->
      check "bare `insert` in expression position is a plain Ident"
        (match a_val.Ast.kind with Ast.Ident "insert" -> true | _ -> false);
      check "bare `select` in expression position always becomes a DbStub"
        (match b_val.Ast.kind with Ast.DbStub _ -> true | _ -> false)
    | _ -> check "insert vs. select: exactly two `let` statements" false)
  | _ -> check "insert vs. select: exactly one free fn" false

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
  (* Money/SKU/Float carry no special status -- all three are ordinary
     unknown types now (WO-E225 fires on them as fields). Float went for
     the same phantom-scalar reason Money/SKU did: no float-literal syntax
     in the lexer and no float kind in wob, so no Float value could ever
     be written or represented. Timestamp stays a real builtin. *)
  check "Money is no longer a builtin scalar" (not (Types.is_builtin_scalar "Money"));
  check "SKU is no longer a builtin scalar" (not (Types.is_builtin_scalar "SKU"));
  check "Float is not a builtin scalar" (not (Types.is_builtin_scalar "Float"));
  check "Timestamp is a builtin scalar" (Types.is_builtin_scalar "Timestamp")

let () =
  (* class Node { next: Node } -- direct self-reference, no @gc, no
     @table, no @unique field: WO-W201 must fire, at the class's own
     (real) file/line/col, and a warning-only run must still exit 0
     (Diag.Collector's severity-keyed exit-code contract). *)
  let path = "node.wo" in
  let _, collector = typecheck_str ~file:path "class Node {\n  next: Node\n}\n" in
  let diags = Diag.Collector.diagnostics collector in
  check_eq "gc-suggestion: exactly one diagnostic (WO-W201)" ~expected:1
    ~actual:(List.length diags) string_of_int;
  (match diags with
  | [ d ] ->
    check "gc-suggestion: code is WO-W201" (d.Diag.code = "WO-W201");
    check "gc-suggestion: severity is Warning" (d.Diag.severity = Diag.Warning);
    check "gc-suggestion: real file/line/col (node.wo:1:1, the `class` token)"
      (d.Diag.site.Diag.file = path && d.Diag.site.Diag.line = 1 && d.Diag.site.Diag.col = 1)
  | _ -> check "gc-suggestion: exactly one diagnostic" false);
  check_eq "gc-suggestion: a warning-only run exits 0, not 1" ~expected:0
    ~actual:(Diag.Collector.exit_code collector) string_of_int

let () =
  (* @gc class Cache { next: Cache } -- same recursive shape as above,
     but already @gc: WO-W201 must NOT fire. *)
  let _, collector = typecheck_str ~file:"cache.wo" "@gc\nclass Cache {\n  next: Cache\n}\n" in
  check_eq "gc-suggestion: @gc class reports nothing" ~expected:0
    ~actual:(List.length (Diag.Collector.diagnostics collector)) string_of_int

let () =
  (* @table(...) class Node2 { next: Node2 } -- recursive, but DB-backed
     via @table: WO-W201 must NOT fire (plan: "@table -> must be owned"). *)
  let _, collector =
    typecheck_str ~file:"node2.wo"
      "@table(name: \"nodes\")\nclass Node2 {\n  next: Node2\n}\n"
  in
  check_eq "gc-suggestion: @table class reports nothing" ~expected:0
    ~actual:(List.length (Diag.Collector.diagnostics collector)) string_of_int

let () =
  (* class Node3 { id: Id @unique; next: Node3 } -- recursive, but has a
     @unique field (persistent identity): WO-W201 must NOT fire (plan's
     "When NOT to emit" list, second bullet). *)
  let _, collector =
    typecheck_str ~file:"node3.wo"
      "class Node3 {\n  id: Id @unique\n  next: Node3\n}\n"
  in
  check_eq "gc-suggestion: class with a @unique field reports nothing" ~expected:0
    ~actual:(List.length (Diag.Collector.diagnostics collector)) string_of_int

let () =
  (* class Point { x: Int; y: Int } -- a plain data struct, no
     recursive/shared fields: WO-W201 must NOT fire either. *)
  let _, collector =
    typecheck_str ~file:"point.wo" "class Point {\n  x: Int\n  y: Int\n}\n"
  in
  check_eq "gc-suggestion: simple data struct reports nothing" ~expected:0
    ~actual:(List.length (Diag.Collector.diagnostics collector)) string_of_int

let () =
  (* class Calc { items: multi Item } (golden/ast/body-statements.wo's own
     shape) -- a `multi` field of an UNRELATED type, not `multi Self`.
     has_recursive_structure must key off self-reference, not "any multi
     field": a bare `Ast.Multi _ -> true` would spuriously fire WO-W201
     on every plain data class that merely holds a collection. *)
  let _, collector =
    typecheck_str ~file:"calc.wo" "class Calc {\n  items: multi Item\n}\n"
  in
  check_eq "gc-suggestion: unrelated `multi Item` field reports nothing" ~expected:0
    ~actual:(List.length (Diag.Collector.diagnostics collector)) string_of_int

let () =
  (* class Bucket { entries: map<Text, Item> } -- same over-trigger risk
     for `map`, neither side self-referential. *)
  let _, collector =
    typecheck_str ~file:"bucket.wo" "class Bucket {\n  entries: map<Text, Item>\n}\n"
  in
  check_eq "gc-suggestion: unrelated `map<Text, Item>` field reports nothing" ~expected:0
    ~actual:(List.length (Diag.Collector.diagnostics collector)) string_of_int

let () =
  (* class Tree { children: multi Tree } -- `multi Self` must still fire
     (the plan's own literal example of the heuristic). *)
  let path = "tree.wo" in
  let _, collector =
    typecheck_str ~file:path "class Tree {\n  children: multi Tree\n}\n"
  in
  let diags = Diag.Collector.diagnostics collector in
  check_eq "gc-suggestion: `multi Self` still fires WO-W201" ~expected:1
    ~actual:(List.length diags) string_of_int;
  match diags with
  | [ d ] -> check "gc-suggestion: `multi Self` diagnostic is WO-W201" (d.Diag.code = "WO-W201")
  | _ -> check "gc-suggestion: `multi Self` exactly one diagnostic" false

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
  let tables = Owner.analyze ~file prog syms collector in
  (tables, collector)

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
    check_site "borrow-escape (stored in a field)" ~code:"WO-E304" ~line:9 ~col:16 ~rel_line:8
      ~rel_col:12 store;
    check_site "borrow-escape (returned)" ~code:"WO-E304" ~line:18 ~col:10 ~rel_line:17 ~rel_col:9
      ret;
    check_site "borrow-escape (moved to a `take` parameter)" ~code:"WO-E304" ~line:22 ~col:15
      ~rel_line:21 ~rel_col:10 take;
    (* spec section 6's own wording for this diagnostic *)
    check "borrow-escape: names the place and the function it escapes"
      (Option.is_some (find_substring ~needle:"borrow of `h.box` escapes `leak`" ret.Diag.message))
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
  (* @gc is exempt from all of it (spec rule 5): the exact shape that is
     WO-E301 above is silent here, and produces no move-table entries
     either — a @gc transfer is an rc site, not a move. *)
  let src =
    "@gc\n\
     class Cache {\n\
    \  n: Int\n\
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
  let rc_path = "golden/owner/rc.wo" in
  let rc_tables, _ = owner_str ~file:rc_path (read_file rc_path) in
  check "rc table: every entry carries a real AST node id"
    (List.for_all (fun (r : Owner.rc_site) -> r.Owner.rc_node > 0) rc_tables.Owner.rcs);
  check "rc table: the balanced pair is elided, the escaping one kept"
    (List.exists (fun (r : Owner.rc_site) -> r.Owner.rc_elided) rc_tables.Owner.rcs
    && List.exists (fun (r : Owner.rc_site) -> not r.Owner.rc_elided) rc_tables.Owner.rcs);
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
  (* IMPORTANT: a `mut` argument means the callee may replace what the place
     holds. For a @gc place that invalidates rc elision — the elided
     increment would leave the alias as the last reference to a freed
     object. The clobber therefore has to happen before the ownership class
     is consulted, since @gc arguments create no access entry at all. *)
  let path = "golden/owner/rc.wo" in
  let tables, _ = owner_str ~file:path (read_file path) in
  let at line =
    List.filter (fun (r : Owner.rc_site) -> r.Owner.rc_pos.Ast.line = line) tables.Owner.rcs
  in
  single_site "rc: `balanced` has one ACQUIRE" (at 15) (fun r ->
      check "rc: an alias whose source is never clobbered is ELIDED" r.Owner.rc_elided);
  single_site "rc: `clobbered` has one ACQUIRE" (at 34) (fun r ->
      check "rc: an alias whose source root is passed `mut` is KEPT"
        (not r.Owner.rc_elided))

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
