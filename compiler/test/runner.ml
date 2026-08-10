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

(* ---- golden-directory walk ------------------------------------------ *)

(* Each stage directory under golden/ names one `woc --dump-*` flag.
   "tokens" (Task 3) and "ast" (Task 4) exist today; later tasks add
   "types", "owner" alongside their own dump function in dump.ml. *)
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
