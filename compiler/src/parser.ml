(* parser.ml — declaration-level recursive-descent parser for `.wo`
   OOP source (Task 4 of compiler/plan/2026-08-01-woc-compiler-front.md).

   Ported from crates/rt/src/parser.rs's structure and conventions
   wherever they still apply, with two deliberate divergences forced by
   this task's own contract:

   1. **Multi-error recovery, not first-error-stop.** rt's parser
      returns `anyhow::Result` and bails the *entire* parse on the
      first problem. This front end's diagnostics contract (diag.ml,
      since Task 2) is multi-error, and the Task 4 brief requires it
      explicitly: "a broken declaration syncs to the next top-level
      keyword and parsing continues, so one bad class yields one
      diagnostic, not a cascade." Every low-level expectation failure
      ([fail]/[unexpected]) reports exactly one diagnostic through the
      given collector, then raises [Parse_error] — an exception used
      purely for unwinding control flow back to [parse_program]'s
      top-level loop, never exposed outside this module. Recovery
      granularity is the whole enclosing top-level declaration: any
      failure anywhere inside a class/type/interface/fn body discards
      that entire declaration and resyncs, matching "one bad class
      yields one diagnostic" literally (not just "one bad field").

   2. **`ref`/`multi`/`map` are not lexer keywords here.** rt's
      token.rs carries KwRef/KwMulti as real keywords; Task 3's lexer
      deliberately dropped that whole schema-keyword zoo (see
      token.ml's module doc). So field-type recognition matches on
      `Token.Ident "ref"` / `"multi"` / `"map"` by name, exactly the
      same positional-keyword trick rt itself uses for `insert`/
      `select` in method bodies (crates/rt/src/parser.rs parse_stmt).
      Same story for `service`/`policy`/`on`, which drive the
      skip-on-block behavior below.

   Kept faithfully from rt: newline-significant skipping
   ([skip_newlines]); the `looks_like_field` two-token lookahead
   (Ident then Colon) that disambiguates a field line from a
   service/policy/on line; `@table`'s known-keys parsing including its
   exact error shape (unknown key, duplicate name, non-string name,
   empty index list); `skip_block_line`'s and `skip_on_block`'s
   brace/paren/bracket-depth counters — the on-block one in particular
   is the load-bearing piece the Task 4 brief calls out by name: object
   literals like `{ article_id: self.id }` inside an `on` block must
   not be mistaken for the close of the enclosing class/type body.

   New relative to rt (this task's own grammar, not a port): `interface`
   (signatures only, no bodies); `@gc`; parameter conventions (bare =
   borrow, `mut`, `take` — `KwMut`/`KwTake` are real Task-3 keywords);
   free top-level `fn` (Task 6's brief mentions "free-fn tables", so
   this is real grammar, not a rt carry-over); and capturing a method
   body as a verbatim token span rather than parsing or discarding it
   (Task 5 parses it for real).

   [Dump.kind_label] is reused for the "got X" half of syntax-error
   messages rather than writing a second exhaustive match over
   `Token.kind` here — dump.ml's match already has to stay exhaustive
   (a new token kind is a compile error there), so reusing it avoids a
   second copy of the same maintenance burden. This makes parser.ml
   depend on dump.ml, which otherwise only renders finished output;
   there's no cycle (dump.ml depends on token.ml/ast.ml only), but it's
   a deliberate, slightly unusual edge worth flagging. *)

exception Parse_error

type state = {
  toks : Token.t array;
  len : int;
  file : string;
  collector : Diag.Collector.t;
  mutable pos : int;
  mutable next_id : int;
  (* True while parsing an if/while condition or a for-loop's iterable
     expression — Task 5's fix for the identifier-then-brace ambiguity
     a bare condition shares with constructor literals (`if active { ...
     }`: is `active` the whole condition, or the start of `active { ...
     }` as a constructor literal swallowing the if's own block?). See
     [looks_like_ctor] and [parse_expr_no_brace]. Reset to `false` while
     inside a parenthesized sub-expression (parens make the boundary
     unambiguous again, e.g. `if (Widget { x: 1 }).ok { ... }`). *)
  mutable no_brace : bool;
}

let make (collector : Diag.Collector.t) ~(file : string) (toks : Token.t list) : state =
  let arr = Array.of_list toks in
  { toks = arr; len = Array.length arr; file; collector; pos = 0; next_id = 1; no_brace = false }

let fresh_id (st : state) : int =
  let id = st.next_id in
  st.next_id <- id + 1;
  id

(* Every token list ends with Token.Eof (lexer.ml's tokenize contract),
   so len >= 1 always and this clamp never reads past the real array. *)
let cur (st : state) : Token.t = st.toks.(min st.pos (st.len - 1))
let peek (st : state) : Token.kind = (cur st).kind

let peek_pos (st : state) : Ast.pos =
  let t = cur st in
  { Ast.line = t.line; col = t.col }

let tok_at (st : state) (i : int) : Token.t = st.toks.(min (max i 0) (st.len - 1))

let at_end (st : state) : bool = match peek st with Token.Eof -> true | _ -> false

(* Never advances past Eof — mirrors rt's `advance` guard. *)
let advance (st : state) : Token.t =
  let t = cur st in
  if not (at_end st) then st.pos <- st.pos + 1;
  t

let skip_newlines (st : state) : unit =
  let continue_ = ref true in
  while !continue_ do
    match peek st with
    | Token.Newline -> ignore (advance st)
    | _ -> continue_ := false
  done

(* Reports one diagnostic at [site] and unwinds to the nearest
   recovery point via [Parse_error]. Polymorphic return type: callers
   use it in both `unit` context (expect) and `string` context
   (expect_ident), since a `raise` never actually returns. *)
let fail (st : state) (site : Ast.pos) (code : string) (message : string) : 'a =
  Diag.Collector.add st.collector
    (Diag.error ~code ~file:st.file ~line:site.Ast.line ~col:site.Ast.col ~message ());
  raise Parse_error

let syntax_code = Diag.parsing_prefix ^ "01" (* WO-E101: generic syntax error *)
let table_code = Diag.parsing_prefix ^ "02" (* WO-E102: invalid @table(...) configuration *)
let gc_removed_code = Diag.parsing_prefix ^ "04" (* WO-E104: `@gc` — GC-ness is inferred *)

(* haxe-parity Task 2: the haxe keyword verdict table's `inline` row —
   "adopt (values): const compile-time values; inline *functions*
   rejected — optimization is the compiler's job". `const` (below) is
   the adopted half; this is the reject half's own diagnostic, cited at
   `inline`'s own position, one per bad declaration (parse_program's
   existing try/with resyncs past the whole discarded `inline fn ...`
   body, same as any other bad top-level declaration). *)
let inline_fn_code = Diag.parsing_prefix ^ "03" (* WO-E103 *)

let unexpected (st : state) (what : string) : 'a =
  let p = peek_pos st in
  fail st p syntax_code
    (Printf.sprintf "expected %s, got %s" what (Dump.kind_label (peek st)))

let expect (st : state) (want : Token.kind) (what : string) : unit =
  if peek st = want then ignore (advance st) else unexpected st what

let accept (st : state) (want : Token.kind) : bool =
  if peek st = want then begin
    ignore (advance st);
    true
  end
  else false

let expect_ident (st : state) (what : string) : string =
  match peek st with
  | Token.Ident s ->
    ignore (advance st);
    s
  | _ -> unexpected st what

(* haxe-parity Task 4: `type` is a legal FIELD name (the sample's own
   wire-format key — mcp.wo's `ToolText = { type: Text, ... }` carries
   JSON's `type` verbatim), so the three field-NAME positions (a field
   declaration, a constructor-literal key, a `.field` access) accept the
   keyword and treat it as the plain name "type". Field positions only —
   everywhere else `type` stays the declaration keyword it is. *)
let expect_field_name (st : state) (what : string) : string =
  match peek st with
  | Token.KwType ->
    ignore (advance st);
    "type"
  | _ -> expect_ident st what

(* ---- field/service/policy/on disambiguation --------------------------

   Lookahead only: Ident immediately followed by Colon. Same shape rt
   uses (crates/rt/src/parser.rs looks_like_field) to tell a field line
   apart from a service/policy/on line without a reserved keyword. *)
let looks_like_field (st : state) : bool =
  match peek st with
  | Token.Ident _ -> (tok_at st (st.pos + 1)).kind = Token.Colon
  | _ -> false

let is_sync_ident (s : string) : bool =
  match s with
  | "service" | "policy" | "on" -> true
  | _ -> false

(* ---- declaration-level recovery ---------------------------------------

   Ported from rt's skip_top_level_chunk, extended with this grammar's
   extra top-level starters (interface, fn, service/policy/on as
   idents, @). Balances braces/brackets/parens while scanning; an
   RBrace that brings depth to <=0 stops the scan right after it (this
   specifically handles "we were skipping a broken class/type/interface
   body — its own closing brace ends the skip", distinct from
   RBracket/RParen which only ever decrement depth and never stop the
   scan by themselves — ported exactly as rt has it, not generalized,
   because that asymmetry is deliberate there too). The `st.pos > start`
   guard on every sync-keyword arm guarantees forward progress even
   when the parser is already sitting on a sync token when recovery
   begins. *)
let sync_to_next_top_level (st : state) : unit =
  let depth = ref 0 in
  let start = st.pos in
  let continue_ = ref true in
  while !continue_ do
    match peek st with
    | Token.Eof -> continue_ := false
    | Token.LBrace ->
      incr depth;
      ignore (advance st)
    | Token.RBrace ->
      decr depth;
      ignore (advance st);
      if !depth <= 0 then continue_ := false
    | Token.LBracket ->
      incr depth;
      ignore (advance st)
    | Token.RBracket ->
      decr depth;
      ignore (advance st)
    | Token.LParen ->
      incr depth;
      ignore (advance st)
    | Token.RParen ->
      decr depth;
      ignore (advance st)
    | Token.KwType when !depth = 0 && st.pos > start -> continue_ := false
    | Token.KwTypedef when !depth = 0 && st.pos > start -> continue_ := false
    | Token.KwClass when !depth = 0 && st.pos > start -> continue_ := false
    | Token.KwInterface when !depth = 0 && st.pos > start -> continue_ := false
    | Token.KwFn when !depth = 0 && st.pos > start -> continue_ := false
    | Token.KwUse when !depth = 0 && st.pos > start -> continue_ := false
    | Token.KwConst when !depth = 0 && st.pos > start -> continue_ := false
    (* KwPub deliberately NOT a sync point (unlike every other top-level
       starter above): `pub(read)` (Task 7's field-accessor marker, not
       this task's — ast.ml's method_decl.pub doc comment) recurs
       *inside* an already-broken class/type body one field at a time,
       and making KwPub a stop point would turn one coarse "whole class
       discarded" diagnostic into one diagnostic per `pub(read)` field
       line. Leaving it out preserves the pre-existing coarse-recovery
       behavior there — the brief's own "leave it unparsed" option for
       `pub(` — while a genuinely top-level `pub` still needs no sync
       help at all: it's handled inline by parse_program on the
       error-free path, and recovery only ever runs after a failure. *)
    | Token.At when !depth = 0 && st.pos > start -> continue_ := false
    | Token.Ident s when !depth = 0 && st.pos > start && is_sync_ident s -> continue_ := false
    | _ -> ignore (advance st)
  done

(* ---- type-level annotations (@gc, @table) ------------------------------

   Ported from rt's parse_type_annotations. `table` is the only
   annotation with structured, checked arguments; `gc` is bare (no
   arguments expected, but a stray `(...)` is tolerated rather than
   rejected — Task 4's brief only requires @table's keys to be
   strict); any other annotation name is unknown and skips silently
   (its own precedent, then this rt precedent) after consuming an
   optional `(...)` argument block without interpreting it. *)

let skip_paren_args (st : state) : unit =
  if peek st = Token.LParen then begin
    ignore (advance st);
    let depth = ref 1 in
    while !depth > 0 && not (at_end st) do
      match peek st with
      | Token.LParen ->
        incr depth;
        ignore (advance st)
      | Token.RParen ->
        decr depth;
        ignore (advance st)
      | _ -> ignore (advance st)
    done
  end

let parse_table_cfg (st : state) : Ast.table_cfg =
  let cfg = ref { Ast.table_name = None; indexes = [] } in
  if accept st Token.LParen then begin
    let continue_ = ref true in
    while !continue_ do
      skip_newlines st;
      if accept st Token.RParen then continue_ := false
      else begin
        let key = expect_ident st "@table argument" in
        expect st Token.Colon "':'";
        (match key with
         | "name" ->
           if !cfg.Ast.table_name <> None then
             fail st (peek_pos st) table_code "@table(name: ...) given twice";
           (match peek st with
            | Token.Str s ->
              ignore (advance st);
              cfg := { !cfg with Ast.table_name = Some s }
            | _ -> unexpected st "a string for @table name")
         | "index" ->
           expect st Token.LBracket "'['";
           let cols = ref [] in
           let more = ref true in
           while !more do
             cols := expect_ident st "index column" :: !cols;
             if not (accept st Token.Comma) then more := false
           done;
           expect st Token.RBracket "']'";
           if !cols = [] then
             fail st (peek_pos st) table_code "@table index needs at least one column";
           cfg := { !cfg with Ast.indexes = !cfg.Ast.indexes @ [ List.rev !cols ] }
         | other ->
           fail st (peek_pos st) table_code
             (Printf.sprintf "unknown @table argument `%s` (supported: name, index)" other));
        skip_newlines st;
        if not (accept st Token.Comma) then begin
          skip_newlines st;
          expect st Token.RParen "')' or ','";
          continue_ := false
        end
      end
    done
  end;
  !cfg

type type_annotations = {
  is_gc : bool;
  table : Ast.table_cfg option;
}

let no_annotations = { is_gc = false; table = None }

let parse_type_annotations (st : state) : type_annotations =
  let is_gc = ref false in
  let table = ref None in
  while peek st = Token.At do
    ignore (advance st);
    let at_pos = peek_pos st in
    let name = expect_ident st "annotation name" in
    (match name with
     | "gc" ->
       (* iteration 7b: `@gc` is not part of the language — GC-ness is inferred
          (structural cycles + demand promotion; see `woc --dump-gc`). Reject it
          rather than accept a meaningless annotation. `is_gc` stays false. *)
       Diag.Collector.add st.collector
         (Diag.error ~code:gc_removed_code ~file:st.file ~line:at_pos.Ast.line
            ~col:at_pos.Ast.col
            ~message:
              "`@gc` is not a valid annotation: GC-ness is inferred by the \
               compiler (run `woc --dump-gc`). Remove it." ());
       skip_paren_args st
     | "table" -> table := Some (parse_table_cfg st)
     | _ -> skip_paren_args st);
    skip_newlines st
  done;
  { is_gc = !is_gc; table = !table }

(* ---- field parsing ------------------------------------------------------ *)

let parse_field_ty (st : state) : Ast.field_ty =
  let nullable = ref false in
  if accept st Token.Question then nullable := true;
  let base_ty =
    match peek st with
    | Token.Ident "ref" ->
        ignore (advance st);
        Ast.Ref (expect_ident st "ref target type")
    | Token.Ident "multi" ->
        ignore (advance st);
        Ast.Multi (expect_ident st "multi target type")
    | Token.Ident "backlink" ->
        ignore (advance st);
        let cls = expect_ident st "backlink source class" in
        expect st Token.Dot "'.'";
        let fld = expect_ident st "backlink source field" in
        Ast.Backlink (cls, fld)
    | Token.Ident "map" ->
        ignore (advance st);
        expect st Token.Lt "'<'";
        let k = expect_ident st "map key type" in
        expect st Token.Comma "','";
        let v = expect_ident st "map value type" in
        expect st Token.Gt "'>'";
        Ast.Map (k, v)
    | Token.Ident name ->
        ignore (advance st);
        (* haxe-parity Task 4: one qualified segment (`json.Value` — the
           sample's own stdlib-reserved type in a record field). Kept as
           one dotted Scalar name; whether it resolves is types.ml's
           question (is_known_type_name treats a reserved-stdlib head as
           UNKNOWN-BUT-RESERVED, same convention as `fs.stat(...)` calls). *)
        if peek st = Token.Dot then begin
          ignore (advance st);
          let member = expect_ident st "qualified type name" in
          Ast.Scalar (name ^ "." ^ member)
        end
        else Ast.Scalar name
    | _ -> unexpected st "a field type"
  in
  if !nullable then Ast.Nullable base_ty else base_ty

(* Collects the raw tokens of a default expression up to (not
   including) a Newline/Comma/RBrace/Eof at depth 0 — the "opaque token
   span" the Task 4 brief asks for, mirroring rt's balanced-slurp loop
   in parse_default_expr but keeping tokens instead of flattening to a
   string. *)
let collect_default_tokens (st : state) : Token.t list =
  let buf = ref [] in
  let depth = ref 0 in
  let continue_ = ref true in
  while !continue_ do
    match peek st with
    | Token.Eof -> continue_ := false
    | Token.Newline when !depth = 0 -> continue_ := false
    | Token.Comma when !depth = 0 -> continue_ := false
    | Token.RBrace when !depth = 0 -> continue_ := false
    | Token.LBrace | Token.LBracket | Token.LParen ->
      incr depth;
      buf := advance st :: !buf
    | Token.RBrace | Token.RBracket | Token.RParen ->
      decr depth;
      buf := advance st :: !buf
    | _ -> buf := advance st :: !buf
  done;
  List.rev !buf

(* `now` / `now()` is recognized explicitly only when it is the WHOLE
   default expression (immediately followed by a field/expr terminator)
   — pure lookahead, no speculative advance-then-rewind, unlike rt's
   version (which mutates its cursor and restores it on failure). `now`
   followed by more tokens (`now + 5`, a field literally typed `now`
   followed by something else) falls through to the opaque path,
   exactly like rt. *)
let parse_default_expr (st : state) : Ast.default_expr =
  let ends_expr (k : Token.kind) : bool =
    match k with Token.Newline | Token.Comma | Token.RBrace | Token.Eof -> true | _ -> false
  in
  let looks_like_bare_now =
    match peek st with
    | Token.Ident "now" -> (
      match (tok_at st (st.pos + 1)).kind with
      | Token.LParen -> (
        match (tok_at st (st.pos + 2)).kind with
        | Token.RParen -> ends_expr (tok_at st (st.pos + 3)).kind
        | _ -> false)
      | k -> ends_expr k)
    | _ -> false
  in
  if looks_like_bare_now then begin
    ignore (advance st);
    (* now *)
    if peek st = Token.LParen then begin
      ignore (advance st);
      (* ( *)
      ignore (advance st) (* ) *)
    end;
    Ast.DefaultNow
  end
  else Ast.DefaultOpaque (collect_default_tokens st)

(* `~comma_ends` (haxe-parity Task 4): a typedef record body may list
   its fields on one line, comma-separated (`typedef HttpResp = {
   status: Int, body: Text }` — the sample's own shape), so a depth-0
   comma ends the field there exactly like a newline does in a
   class/type body; the comma itself is left for the record loop to
   consume. Every pre-existing call site passes nothing and keeps the
   class-body behavior byte-identical (a comma there is still the same
   "unexpected" error as before). *)
let parse_field ?(comma_ends = false) ?(pub_read = false) (st : state) : Ast.field =
  let pos = peek_pos st in
  let name = expect_field_name st "field name" in
  expect st Token.Colon "':'";
  let ty = parse_field_ty st in
  let default = ref None in
  let annotations = ref [] in
  let continue_ = ref true in
  while !continue_ do
    match peek st with
    | Token.At ->
      ignore (advance st);
      let ann_name = expect_ident st "annotation name" in
      skip_paren_args st;
      annotations := ann_name :: !annotations
    | Token.Eq ->
      ignore (advance st);
      default := Some (parse_default_expr st)
    | Token.Comma when comma_ends -> continue_ := false
    | Token.Newline | Token.RBrace | Token.Eof -> continue_ := false
    | _ -> unexpected st "an annotation, '=', or end of field"
  done;
  { Ast.id = fresh_id st; pos; name; ty; default = !default; annotations = List.rev !annotations;
    pub_read }

(* ---- param / signature parsing ------------------------------------------ *)

let parse_param (st : state) : Ast.param =
  let pos = peek_pos st in
  let conv =
    if accept st Token.KwMut then Ast.Mut
    else if accept st Token.KwTake then Ast.Take
    else Ast.Borrow
  in
  let name = expect_ident st "parameter name" in
  expect st Token.Colon "':'";
  let ty = parse_field_ty st in
  { Ast.id = fresh_id st; pos; name; conv; ty }

let parse_params (st : state) : Ast.param list =
  expect st Token.LParen "'('";
  skip_newlines st;
  let params = ref [] in
  let continue_ = ref (peek st <> Token.RParen) in
  while !continue_ do
    params := parse_param st :: !params;
    skip_newlines st;
    if accept st Token.Comma then skip_newlines st else continue_ := false
  done;
  expect st Token.RParen "')'";
  List.rev !params

let parse_ret_type (st : state) : Ast.field_ty option =
  let nullable = ref false in
  if accept st Token.Question then nullable := true;
  if accept st Token.Arrow then begin
    let ty = parse_field_ty st in
    Some (if !nullable then Ast.Nullable ty else ty)
  end else None

type sig_head = {
  s_id : int;
  s_pos : Ast.pos;
  s_name : string;
  s_params : Ast.param list;
  s_ret : Ast.field_ty option;
}

let parse_sig_head (st : state) : sig_head =
  let pos = peek_pos st in
  expect st Token.KwFn "`fn`";
  let name = expect_ident st "function/method name" in
  (* Id assigned here, before params are parsed (each of which mints its
     own id) — mirrors class_decl/interface_decl, whose id is likewise
     minted right after their head is confirmed and before their body
     is parsed. Keeps "parent id < every child id" true uniformly across
     every container node, not just some of them. *)
  let id = fresh_id st in
  let params = parse_params st in
  let ret = parse_ret_type st in
  { s_id = id; s_pos = pos; s_name = name; s_params = params; s_ret = ret }

(* ---- statement/expression parser (Task 5) ------------------------------

   Method bodies were a verbatim token span through Task 4; this parses
   them for real. Structured as one large mutually-recursive group
   (parse_stmt / parse_block / the if/while/for statement parsers / the
   whole expression precedence ladder) because every level of the
   ladder ultimately calls back into parse_expr (call args, constructor-
   literal field values, parenthesized sub-expressions), and every
   block-having statement calls parse_block, which calls parse_stmt.

   Precedence ladder, loosest to tightest (parse_expr is the entry
   point; each level's loop is left-associative):

     or           or                      (haxe-parity Task 2)
     and          and                     (haxe-parity Task 2)
     comparison   ==  !=  <  <=  >  >=
     concat       ..
     additive     +  -
     multiplicative  *  /  %
     unary minus  -x
     postfix      a.b   a(b)   a[b]
     primary      literals, idents, `(expr)`, constructor literals,
                  select-as-expression (DbStub)

   This ordering matches Lua's (concat binds looser than +/-, tighter
   than comparison) — see ast.ml's module doc for why `..`/Concat is
   this task's own addition, not a straight rt port. `and`/`or` sit
   above comparison per the spec amendment's own words ("or binds
   loosest, then and, then comparison") — real keywords (KwAnd/KwOr),
   never `&&`/`||`, so `a == 1 and b == 2` parses with no parens: `and`
   only ever sees fully-formed comparisons as its operands.

   End-of-statement convention mirrors parse_field's: a "simple"
   statement (let/assign/return/expr-statement/DbStub) must end at an
   optional `;` followed by a real terminator (Newline, the enclosing
   block's `}}`, or Eof) — [end_of_stmt] enforces this, so e.g.
   `let x = 1 let y = 2` on one line is a syntax error, not silently
   accepted, exactly like two fields can't share a line. Block-having
   statements (if/while/for) do NOT call it: their own closing `}` ends
   them, and requiring a terminator after it would wrongly reject
   `} else {` on one line, the normal style for chained if/else. *)

(* A `;` terminates the statement by itself — whatever follows on the same
   line is the next statement, which is how the driving workload writes a
   short guard body (`{ skip(res, ...); return; }`, `{ i = i + 1;
   continue; }`). Requiring a newline *after* the semicolon (the earlier
   rule) made one-line blocks a syntax error. With no `;`, a newline (or
   the enclosing block's close) is still what ends the statement. *)
let end_of_stmt (st : state) : unit =
  if accept st Token.Semicolon then ()
  else
    match peek st with
    | Token.Newline -> ignore (advance st)
    | Token.RBrace | Token.Eof -> ()
    | _ -> unexpected st "end of statement (newline or ';')"

(* ---- statement-level recovery -------------------------------------------

   One bad statement must yield one diagnostic, not stall or cascade
   (brief: "statement-level recovery syncs at newlines/semicolons").
   Scoped strictly to the enclosing block: unlike sync_to_next_top_level
   (which consumes the depth-0 RBrace it lands on, because a broken
   top-level declaration's own close is what it's abandoning), this
   leaves a depth-0 RBrace unconsumed — exactly skip_block_line's
   convention — so [parse_block]'s own loop sees it and ends the block
   normally instead of the recovery accidentally eating the block's
   close and stalling parse_block forever.

   Known gap, deliberately not chased: if the failure happens while
   parsing an if/while/for's *condition* (before its `{ body }` is ever
   reached), this can stop at a depth-0 `;`/newline that precedes that
   still-unconsumed block, leaving a bare `{ ... }` for the next loop
   iteration to choke on as a second, cascading diagnostic. Every
   fixture this task ships avoids that shape (its bad statements are
   plain let/return lines with no trailing block); a fully general fix
   would need the sync scan to know a block is still pending, which
   isn't worth the complexity this task's brief doesn't ask for. *)
let sync_to_next_stmt (st : state) : unit =
  let depth = ref 0 in
  let continue_ = ref true in
  while !continue_ do
    match peek st with
    | Token.Eof -> continue_ := false
    | Token.RBrace when !depth = 0 -> continue_ := false
    | Token.Newline when !depth = 0 ->
      ignore (advance st);
      continue_ := false
    | Token.Semicolon when !depth = 0 ->
      ignore (advance st);
      continue_ := false
    | Token.LBrace | Token.LBracket | Token.LParen ->
      incr depth;
      ignore (advance st)
    | Token.RBrace | Token.RBracket | Token.RParen ->
      decr depth;
      ignore (advance st)
    | _ -> ignore (advance st)
  done

(* ---- the SQL sublanguage: insert/select as one opaque DbStub node -------

   Spec section 3, "parses but traps": `insert`/`select` (lowercase
   Ident, positionally recognized — the same trick as rt's own
   `insert`/`select`, per the keyword-discipline note this task's brief
   opens with — or the uppercase KwInsert/KwSelect keyword tokens Task 3
   already lexes) are never re-parsed as this grammar. Every token from
   the trigger itself through the statement's own terminator is
   captured verbatim into one Ast.DbStub node — mirrors skip_block_line's
   depth-aware terminator rules (so a brace-enclosed predicate/field
   list spanning its own newlines is still captured whole) but collects
   tokens instead of discarding them.

   `insert` is a statement-only trigger (checked in parse_stmt, never
   reachable from parse_primary): it produces no value, so `let x =
   insert ...` must not parse. `select` is legal in general expression
   position too (checked in parse_primary), matching the brief's
   asymmetry: "statement-position lowercase insert/select (and
   expression-position select)". *)
let is_insert_trigger (k : Token.kind) : bool =
  match k with Token.KwInsert | Token.Ident "insert" -> true | _ -> false

let is_select_trigger (k : Token.kind) : bool =
  match k with Token.KwSelect | Token.Ident "select" -> true | _ -> false

(* Fix round 1/1 finding (CRITICAL 2): the original version only had a
   depth-0 stop guard on RBrace, so a `select`/`insert` nested inside an
   enclosing call or index expression (`wrap(select Foo { x > 1 })`)
   had no way to stop at that call's own `)` — it kept decrementing
   depth *below* zero and swallowing everything to Eof. RParen/RBracket
   now get the exact same "stop, don't consume, leave it for the
   enclosing construct" treatment RBrace already had. Comma also stops
   at depth 0 for the same reason (a comma-separated call argument or
   constructor-literal field: `wrap(select Foo { x > 1 }, 5)`), mirroring
   collect_default_tokens' own convention above. *)
let collect_dbstub_tokens (st : state) : Token.t list =
  let buf = ref [] in
  let depth = ref 0 in
  let continue_ = ref true in
  while !continue_ do
    match peek st with
    | Token.Eof -> continue_ := false
    | Token.Newline when !depth = 0 -> continue_ := false
    | Token.Semicolon when !depth = 0 -> continue_ := false
    | Token.Comma when !depth = 0 -> continue_ := false
    | Token.RBrace when !depth = 0 -> continue_ := false
    | Token.RParen when !depth = 0 -> continue_ := false
    | Token.RBracket when !depth = 0 -> continue_ := false
    | Token.LBrace | Token.LBracket | Token.LParen ->
      incr depth;
      buf := advance st :: !buf
    | Token.RBrace | Token.RBracket | Token.RParen ->
      decr depth;
      buf := advance st :: !buf
    | _ -> buf := advance st :: !buf
  done;
  List.rev !buf

let parse_dbstub_expr (st : state) : Ast.expr =
  let pos = peek_pos st in
  let id = fresh_id st in
  let toks = collect_dbstub_tokens st in
  { Ast.id; pos; kind = Ast.DbStub toks }

(* Exception-safe save/restore of state.no_brace (Fix round 1/1,
   CRITICAL 1). Every no_brace toggle below goes through this, using
   Fun.protect so the restore runs even when [f] raises Parse_error —
   the normal statement-recovery path. The original code restored only
   on a normal return (`st.no_brace <- v; let e = f () in st.no_brace <-
   saved; e`), so a failure anywhere inside a no_brace-toggled region
   (a broken if/while/for condition, or a broken expression nested
   inside one) left state.no_brace permanently stuck, corrupting
   constructor-literal recognition for the rest of the file — not just
   the rest of the current statement, since state.no_brace lives on the
   shared parser state, not a stack frame that unwinds with the
   exception.

   Also used (value = false) for call-argument lists and index
   expressions (CRITICAL 3): a `(`/`[` is a fresh nesting context whose
   own closing `)`/`]` unambiguously ends it, so a constructor literal
   inside one is never ambiguous with an enclosing if/while/for's block
   — exactly like the parenthesized-primary case, generalized to the
   other two "this token pair brackets a fresh sub-expression" shapes. *)
let with_no_brace (st : state) (value : bool) (f : unit -> 'a) : 'a =
  let saved = st.no_brace in
  st.no_brace <- value;
  Fun.protect ~finally:(fun () -> st.no_brace <- saved) f

(* ---- expression parsing -------------------------------------------------- *)

let rec parse_expr (st : state) : Ast.expr =
  match peek st with Token.KwTry -> parse_try st | _ -> parse_or st

(* haxe-parity Task 5: `try body catch (e) arm`. `try` binds looser than
   every operator, so the body is a full operator expression and `catch`
   is what ends it (`try a / b catch (e) 0` catches the division, not just
   `a`). A newline before `catch` is insignificant — the workload wraps
   long try bodies (mcp.wo's `try self.dispatch(...)` / `catch (e)
   err(...)`). The arm is either a braced block or one expression; both
   become a `stmt list`, so `{}` right after the catch variable is always
   the empty block, never the empty-map literal. *)
and parse_try (st : state) : Ast.expr =
  let pos = peek_pos st in
  let id = fresh_id st in
  ignore (advance st);
  (* 'try' *)
  let body = parse_or st in
  skip_newlines st;
  expect st Token.KwCatch "`catch` after a `try` expression";
  expect st Token.LParen "'(' before the catch variable";
  let ename = expect_ident st "catch variable name" in
  expect st Token.RParen "')' after the catch variable";
  let handler =
    if peek st = Token.LBrace then with_no_brace st false (fun () -> parse_block st)
    else
      let e = parse_expr st in
      [ { Ast.s_id = fresh_id st; s_pos = e.pos; s_kind = Ast.ExprStmt e } ]
  in
  { Ast.id; pos; kind = Ast.Try { body; ename; handler } }

and parse_or (st : state) : Ast.expr =
  let lhs = ref (parse_and st) in
  let continue_ = ref true in
  while !continue_ do
    match peek st with
    | Token.KwOr ->
      let pos = peek_pos st in
      let id = fresh_id st in
      ignore (advance st);
      let rhs = parse_and st in
      lhs := { Ast.id; pos; kind = Ast.Binary (Ast.Or, !lhs, rhs) }
    | _ -> continue_ := false
  done;
  !lhs

and parse_and (st : state) : Ast.expr =
  let lhs = ref (parse_comparison st) in
  let continue_ = ref true in
  while !continue_ do
    match peek st with
    | Token.KwAnd ->
      let pos = peek_pos st in
      let id = fresh_id st in
      ignore (advance st);
      let rhs = parse_comparison st in
      lhs := { Ast.id; pos; kind = Ast.Binary (Ast.And, !lhs, rhs) }
    | _ -> continue_ := false
  done;
  !lhs

and parse_comparison (st : state) : Ast.expr =
  let lhs = ref (parse_concat st) in
  let continue_ = ref true in
  while !continue_ do
    match peek st with
    | (Token.EqEq | Token.NotEq | Token.Lt | Token.LtEq | Token.Gt | Token.GtEq) as k ->
      let pos = peek_pos st in
      let op =
        match k with
        | Token.EqEq -> Ast.Eq
        | Token.NotEq -> Ast.Ne
        | Token.Lt -> Ast.Lt
        | Token.LtEq -> Ast.Le
        | Token.Gt -> Ast.Gt
        | _ -> Ast.Ge
      in
      let id = fresh_id st in
      ignore (advance st);
      let rhs = parse_concat st in
      lhs := { Ast.id; pos; kind = Ast.Binary (op, !lhs, rhs) }
    | _ -> continue_ := false
  done;
  !lhs

and parse_concat (st : state) : Ast.expr =
  let lhs = ref (parse_additive st) in
  let continue_ = ref true in
  while !continue_ do
    match peek st with
    | Token.DotDot ->
      let pos = peek_pos st in
      let id = fresh_id st in
      ignore (advance st);
      let rhs = parse_additive st in
      lhs := { Ast.id; pos; kind = Ast.Binary (Ast.Concat, !lhs, rhs) }
    | _ -> continue_ := false
  done;
  !lhs

and parse_additive (st : state) : Ast.expr =
  let lhs = ref (parse_multiplicative st) in
  let continue_ = ref true in
  while !continue_ do
    match peek st with
    | (Token.Plus | Token.Dash) as k ->
      let pos = peek_pos st in
      let op = if k = Token.Plus then Ast.Add else Ast.Sub in
      let id = fresh_id st in
      ignore (advance st);
      let rhs = parse_multiplicative st in
      lhs := { Ast.id; pos; kind = Ast.Binary (op, !lhs, rhs) }
    | _ -> continue_ := false
  done;
  !lhs

and parse_multiplicative (st : state) : Ast.expr =
  let lhs = ref (parse_unary st) in
  let continue_ = ref true in
  while !continue_ do
    match peek st with
    | (Token.Star | Token.Slash | Token.Percent) as k ->
      let pos = peek_pos st in
      let op = match k with Token.Star -> Ast.Mul | Token.Slash -> Ast.Div | _ -> Ast.Mod in
      let id = fresh_id st in
      ignore (advance st);
      let rhs = parse_unary st in
      lhs := { Ast.id; pos; kind = Ast.Binary (op, !lhs, rhs) }
    | _ -> continue_ := false
  done;
  !lhs

(* `as` binds tighter than every binary operator and looser than a call, so
   `json.decode(raw) as FileConfig` casts the call's result, and
   `x as T == y` compares the cast value. *)
and parse_as (st : state) (e : Ast.expr) : Ast.expr =
  if peek st <> Token.KwAs then e
  else begin
    ignore (advance st);
    let ty = parse_field_ty st in
    parse_as st { Ast.id = fresh_id st; pos = e.Ast.pos; kind = Ast.As (e, ty) }
  end

and parse_unary (st : state) : Ast.expr =
  match peek st with
  | Token.Dash ->
    let pos = peek_pos st in
    let id = fresh_id st in
    ignore (advance st);
    let operand = parse_unary st in
    { Ast.id; pos; kind = Ast.Unary (Ast.Neg, operand) }
  | _ -> parse_as st (parse_postfix st)

and parse_postfix (st : state) : Ast.expr =
  let base = ref (parse_primary st) in
  let continue_ = ref true in
  while !continue_ do
    match peek st with
    | Token.Dot ->
      let pos = peek_pos st in
      ignore (advance st);
      let name = expect_field_name st "field or method name" in
      base := { Ast.id = fresh_id st; pos; kind = Ast.Field (!base, name) }
    | Token.LParen ->
      let pos = peek_pos st in
      let args = parse_call_args st in
      base := { Ast.id = fresh_id st; pos; kind = Ast.Call (!base, args) }
    | Token.LBracket ->
      let pos = peek_pos st in
      ignore (advance st);
      let idx = with_no_brace st false (fun () -> parse_expr st) in
      expect st Token.RBracket "']'";
      base := { Ast.id = fresh_id st; pos; kind = Ast.Index (!base, idx) }
    | _ -> continue_ := false
  done;
  !base

and parse_call_args (st : state) : Ast.expr list =
  expect st Token.LParen "'('";
  with_no_brace st false (fun () ->
      skip_newlines st;
      let args = ref [] in
      let continue_ = ref (peek st <> Token.RParen) in
      while !continue_ do
        args := parse_expr st :: !args;
        skip_newlines st;
        if accept st Token.Comma then skip_newlines st else continue_ := false
      done;
      expect st Token.RParen "')'";
      List.rev !args)

(* Two-token lookahead, exactly like rt's own select-expression trick
   (this brief's own words): a bare Ident immediately followed by `{`
   in expression position is a constructor literal, UNLESS we're
   parsing an if/while condition or a for-loop's iterable expression
   (state.no_brace), where that same `{` is the statement's own
   required block, not a literal's opening brace. *)
and looks_like_ctor (st : state) : bool =
  (not st.no_brace)
  && (match peek st with Token.Ident _ -> true | _ -> false)
  && (tok_at st (st.pos + 1)).kind = Token.LBrace

and parse_ctor_literal (st : state) : Ast.expr =
  let pos = peek_pos st in
  let id = fresh_id st in
  let name = expect_ident st "constructor class name" in
  expect st Token.LBrace "'{'";
  skip_newlines st;
  let fields = ref [] in
  let continue_ = ref (peek st <> Token.RBrace) in
  while !continue_ do
    let fname = expect_field_name st "constructor field name" in
    expect st Token.Colon "':'";
    let fval = parse_expr st in
    fields := (fname, fval) :: !fields;
    skip_newlines st;
    if accept st Token.Comma then begin
      skip_newlines st;
      (* trailing comma before the close (haxe-parity Task 4 — the
         sample's own multi-line record literals end `..., }`) *)
      if peek st = Token.RBrace then continue_ := false
    end
    else continue_ := false
  done;
  skip_newlines st;
  expect st Token.RBrace "'}'";
  { Ast.id; pos; kind = Ast.Ctor (name, List.rev !fields) }

(* haxe-parity Task 3: `switch subject { case v1, v2: <stmts> ... default:
   <stmts> }` — the sample's own shape (grepped every `switch` site in
   docs/examples/log-watcher/*.wo first). The subject is parsed
   `no_brace` for the exact reason if/while/for's own conditions are:
   `switch res { ... }` must not read `res {` as a constructor literal
   swallowing the switch's own body. Arms have no brace of their own
   (the sample never wraps a case body in `{ }`) — [parse_switch_arm_body]
   is [parse_block]'s loop with `case`/`default`/`}` as its stop set
   instead of `}` alone, and no brace to expect/consume. `default` is
   grammar-optional here; whether it's *required* depends on the
   subject's type (scalar/Text: yes; a union: only if a variant is
   missing — Task 4's territory), which is a typecheck-time question
   (WO-E208), not a parse-time one. *)
and parse_switch_arm_body (st : state) : Ast.stmt list =
  let stmts = ref [] in
  let continue_ = ref true in
  while !continue_ do
    skip_newlines st;
    match peek st with
    | Token.KwCase | Token.KwDefault | Token.RBrace -> continue_ := false
    | Token.Eof -> fail st (peek_pos st) syntax_code "unexpected end of input inside switch arm"
    | _ -> (
      try stmts := parse_stmt st :: !stmts
      with Parse_error -> sync_to_next_stmt st)
  done;
  List.rev !stmts

and parse_switch_expr (st : state) : Ast.expr =
  let pos = peek_pos st in
  let id = fresh_id st in
  ignore (advance st);
  (* 'switch' *)
  let subject = parse_expr_no_brace st in
  expect st Token.LBrace "'{' to open switch body";
  let arms = ref [] in
  let continue_ = ref true in
  while !continue_ do
    skip_newlines st;
    match peek st with
    | Token.RBrace ->
      ignore (advance st);
      continue_ := false
    | Token.Eof -> fail st (peek_pos st) syntax_code "unexpected end of input inside switch body"
    | Token.KwCase ->
      let arm_pos = peek_pos st in
      ignore (advance st);
      let values = ref [ parse_expr st ] in
      while accept st Token.Comma do
        values := parse_expr st :: !values
      done;
      expect st Token.Colon "':' after switch case value(s)";
      let body = parse_switch_arm_body st in
      arms := { Ast.arm_pos; values = List.rev !values; is_default = false; body } :: !arms
    | Token.KwDefault ->
      let arm_pos = peek_pos st in
      ignore (advance st);
      expect st Token.Colon "':' after `default`";
      let body = parse_switch_arm_body st in
      arms := { Ast.arm_pos; values = []; is_default = true; body } :: !arms
    | _ -> unexpected st "`case`, `default`, or '}' in switch body"
  done;
  { Ast.id; pos; kind = Ast.Switch (subject, List.rev !arms) }

and parse_insert_expr (st : state) : Ast.expr =
  (* `insert` + a constructor literal, sharing parse_ctor_literal so the
     field-list grammar (trailing commas, newlines) can never drift from the
     ctor's. The literal's node is unwrapped into Insert — its id is reused,
     which is safe because the Ctor node itself is discarded whole. *)
  let pos = peek_pos st in
  ignore (advance st) (* the `insert` trigger token *);
  skip_newlines st;
  let lit = parse_ctor_literal st in
  (match lit.Ast.kind with
  | Ast.Ctor (cn, fields) -> { lit with Ast.pos; kind = Ast.Insert (cn, fields) }
  | _ -> lit (* unreachable: parse_ctor_literal only builds Ctor *))

and is_query_trigger (st : state) : bool =
  (* `from <ident> in` — positional, so `from` stays a usable identifier
     everywhere else (same discipline as insert/select) *)
  (match peek st with Token.Ident "from" -> true | _ -> false)
  && (match (tok_at st (st.pos + 1)).kind with Token.Ident _ -> true | _ -> false)
  && (tok_at st (st.pos + 2)).kind = Token.KwIn

and parse_query_expr (st : state) : Ast.expr =
  let pos = peek_pos st in
  let id = fresh_id st in
  ignore (advance st) (* from *);
  let var = expect_ident st "query range variable" in
  expect st Token.KwIn "`in`";
  (* source: a bare class name is a table scan; any other expression is a
     navigation (`d.staff`). One token of lookahead: Ident not followed by a
     `.`/`(`/`[` and sitting where a clause keyword follows is a table name. *)
  let src =
    match peek st with
    | Token.Ident cn
      when (match (tok_at st (st.pos + 1)).kind with
           | Token.Dot | Token.LParen | Token.LBracket -> false
           | _ -> true) ->
      ignore (advance st);
      Ast.QTable cn
    | _ -> Ast.QNav (parse_expr_no_brace st)
  in
  (* clauses may sit on their own lines; skip the separating newlines when
     looking for the next clause keyword (the query is one expression) *)
  let clause name =
    skip_newlines st;
    match peek st with Token.Ident n when n = name -> true | _ -> false
  in
  let wheres = ref [] in
  while clause "where" do
    ignore (advance st);
    wheres := parse_expr_no_brace st :: !wheres
  done;
  let group =
    if clause "group" then begin
      ignore (advance st);
      let key_elem = parse_expr_no_brace st in
      ignore key_elem (* the grouped element is the range var; `group e by k` *);
      if not (clause "by") then fail st (peek_pos st) syntax_code "expected `by` in a group clause";
      ignore (advance st);
      let key = parse_expr_no_brace st in
      if not (clause "into") then fail st (peek_pos st) syntax_code "expected `into` in a group clause";
      ignore (advance st);
      let gvar = expect_ident st "group variable" in
      Some (gvar, key)
    end
    else None
  in
  let order =
    if clause "order" then begin
      ignore (advance st);
      if not (clause "by") then fail st (peek_pos st) syntax_code "expected `by` after `order`";
      ignore (advance st);
      let key = parse_expr_no_brace st in
      let desc = clause "desc" in
      if desc then ignore (advance st);
      Some (key, desc)
    end
    else None
  in
  (* `take` is a reserved keyword (KwTake, the param convention), not an
     Ident — so match the token, not the name *)
  skip_newlines st;
  let take =
    if peek st = Token.KwTake then (ignore (advance st); Some (parse_expr_no_brace st)) else None
  in
  if not (clause "select") then fail st (peek_pos st) syntax_code "a query must end in `select`";
  ignore (advance st);
  let sel = parse_expr st in
  {
    Ast.id;
    pos;
    kind =
      Ast.Query
        {
          Ast.q_var = var;
          q_src = src;
          q_wheres = List.rev !wheres;
          q_group = group;
          q_order = order;
          q_take = take;
          q_select = sel;
          q_pos = pos;
        };
  }

and parse_primary (st : state) : Ast.expr =
  match peek st with
  | _ when is_query_trigger st -> parse_query_expr st
  | Token.Ident "delete" when (match (tok_at st (st.pos + 1)).kind with
                              | Token.Newline | Token.Semicolon | Token.Eof -> false | _ -> true) ->
    let pos = peek_pos st in
    let id = fresh_id st in
    ignore (advance st);
    let target = parse_expr st in
    { Ast.id; pos; kind = Ast.Delete target }
  | k when is_select_trigger k -> parse_dbstub_expr st
  | k when is_insert_trigger k -> parse_insert_expr st
  | Token.KwSwitch -> parse_switch_expr st
  | Token.Int n ->
    let pos = peek_pos st in
    let id = fresh_id st in
    ignore (advance st);
    { Ast.id; pos; kind = Ast.IntLit n }
  | Token.Str s ->
    let pos = peek_pos st in
    let id = fresh_id st in
    ignore (advance st);
    { Ast.id; pos; kind = Ast.StrLit s }
  | Token.InterpStr segs ->
    let pos = peek_pos st in
    ignore (advance st);
    desugar_interp st pos segs
  | Token.KwTrue ->
    let pos = peek_pos st in
    let id = fresh_id st in
    ignore (advance st);
    { Ast.id; pos; kind = Ast.BoolLit true }
  | Token.KwFalse ->
    let pos = peek_pos st in
    let id = fresh_id st in
    ignore (advance st);
    { Ast.id; pos; kind = Ast.BoolLit false }
  | Token.KwNil ->
    let pos = peek_pos st in
    let id = fresh_id st in
    ignore (advance st);
    { Ast.id; pos; kind = Ast.NilLit }
  | Token.LParen ->
    ignore (advance st);
    (* Parens make the enclosed expression unambiguous again, so a
       constructor literal is legal here even inside an if/while
       condition (`if (Widget { x: 1 }).ok { ... }`). *)
    let e = with_no_brace st false (fun () -> parse_expr st) in
    expect st Token.RParen "')'";
    e
  | Token.LBracket ->
    (* `[]` / `[a, b, c]` — a fresh `multi`. Newlines inside the brackets
       are insignificant (the workload writes single-line literals, but a
       long one must be allowed to wrap like a call's argument list). *)
    let pos = peek_pos st in
    let id = fresh_id st in
    ignore (advance st);
    let items = ref [] in
    skip_newlines st;
    while peek st <> Token.RBracket && not (at_end st) do
      items := with_no_brace st false (fun () -> parse_expr st) :: !items;
      skip_newlines st;
      if accept st Token.Comma then skip_newlines st
    done;
    expect st Token.RBracket "']' to close the list literal";
    { Ast.id; pos; kind = Ast.ListLit (List.rev !items) }
  | Token.LBrace when not st.no_brace ->
    (* `{}` — a fresh empty `map`. Only the empty form: see ast.ml's
       MapLit doc comment for why `{ k: v }` is not grammar. *)
    let pos = peek_pos st in
    let id = fresh_id st in
    ignore (advance st);
    skip_newlines st;
    if peek st <> Token.RBrace then
      fail st (peek_pos st) syntax_code
        "only the empty map literal `{}` is an expression — build entries with `set(m, k, v)`";
    ignore (advance st);
    { Ast.id; pos; kind = Ast.MapLit }
  | Token.Ident _ when looks_like_ctor st -> parse_ctor_literal st
  | Token.Ident s ->
    let pos = peek_pos st in
    let id = fresh_id st in
    ignore (advance st);
    { Ast.id; pos; kind = Ast.Ident s }
  | _ -> unexpected st "an expression"

(* haxe-parity Task 2: desugars one interpolated string's segments into a
   `..`/Concat chain of StrLit (text) and Interp (embedded expression)
   nodes — the "desugars at parse time to concatenation" the brief
   names. Each `Token.SExpr raw` segment is a full expression's *raw
   source*, captured verbatim by the lexer (token.ml/lexer.ml's own doc
   comments) — re-tokenized and re-parsed here via a fresh, nested
   lexer/parser state over just that substring. Every generated node
   (StrLit/Interp/the Concat spine) shares the outer string literal's
   own single position: this AST has no source *ranges* (ast.ml's
   module doc), and per-segment positions would need the lexer to track
   an offset into the interpolation that nothing downstream needs today.
   Known, disclosed imprecision: a malformed `${...}` expression's own
   error therefore reports at the whole string's start, not the
   sub-expression's real column — acceptable since the sub-parse still
   raises a real, correctly-coded diagnostic, just at a coarser site.

   Review fix (Important, post-Task-2): a genuine sub-parse failure
   (`"${1 +}"`, not just trailing garbage — `"${1 2}"`) used to add its
   OWN diagnostic straight to `st.collector` from inside `parse_expr
   sub_st`, at `sub_st`'s own uncorrected line/col (that lexer counts
   from 1:1 over the raw substring, so the reported position landed on
   some unrelated line of the *real* file), and then let `Parse_error`
   propagate straight past this function, skipping the "malformed
   ${...}" framing entirely. The sub-lex/sub-parse now runs against a
   private, throwaway collector — nothing it reports (a lex error, a
   parse error, or reaching a non-`Eof` leftover) ever touches the real
   collector — so every failure inside collapses to exactly the one
   diagnostic below, at the outer string's own position. *)
and desugar_interp (st : state) (pos : Ast.pos) (segs : Token.str_part list) : Ast.expr =
  let mk_str s = { Ast.id = fresh_id st; pos; kind = Ast.StrLit s } in
  let mk_interp inner = { Ast.id = fresh_id st; pos; kind = Ast.Interp inner } in
  let parse_segment_expr (raw : string) : Ast.expr =
    let sub_collector = Diag.Collector.create () in
    let sub_toks = Lexer.tokenize sub_collector ~file:st.file raw in
    let sub_st = make sub_collector ~file:st.file sub_toks in
    (* The sub-parse must mint ids from the OUTER id space and hand back what
       it used. A fresh state starts at 1, so every interpolation used to
       produce nodes whose ids collided with unrelated nodes of the same file —
       and every side table downstream (drops, moves, rc sites, masks,
       f_decl) is keyed by node id, so a collision silently attributes one
       construct's ownership table to another. That surfaced as a WO-E404
       ("ownership table names `headers`, which has no register") on a method
       whose interpolation happened to collide with a later local's node. *)
    sub_st.next_id <- st.next_id;
    let parsed =
      try
        let e = parse_expr sub_st in
        if peek sub_st = Token.Eof && not (Diag.Collector.has_error sub_collector) then Some e
        else None
      with Parse_error -> None
    in
    st.next_id <- sub_st.next_id;
    match parsed with
    | Some e -> e
    | None -> fail st pos syntax_code "malformed \"${...}\" interpolation expression"
  in
  let parts =
    List.filter_map
      (function
        | Token.SText "" -> None
        | Token.SText s -> Some (mk_str s)
        | Token.SExpr raw -> Some (mk_interp (parse_segment_expr raw)))
      segs
  in
  match parts with
  | [] -> mk_str ""
  | first :: rest ->
    List.fold_left
      (fun acc e -> { Ast.id = fresh_id st; pos; kind = Ast.Binary (Ast.Concat, acc, e) })
      first rest

(* ---- statement parsing --------------------------------------------------- *)

and parse_block (st : state) : Ast.stmt list =
  expect st Token.LBrace "'{' to open block";
  let stmts = ref [] in
  let continue_ = ref true in
  while !continue_ do
    skip_newlines st;
    match peek st with
    | Token.RBrace ->
      ignore (advance st);
      continue_ := false
    | Token.Eof -> fail st (peek_pos st) syntax_code "unexpected end of input inside block"
    | _ -> (
      try stmts := parse_stmt st :: !stmts
      with Parse_error -> sync_to_next_stmt st)
  done;
  List.rev !stmts

and parse_let_stmt (st : state) : Ast.stmt =
  let pos = peek_pos st in
  let id = fresh_id st in
  ignore (advance st);
  (* 'let' *)
  let name = expect_ident st "let-binding name" in
  let ty = if accept st Token.Colon then Some (parse_field_ty st) else None in
  expect st Token.Eq "'=' in let binding";
  let value = parse_expr st in
  end_of_stmt st;
  { Ast.s_id = id; s_pos = pos; s_kind = Ast.Let { name; ty; value } }

and parse_if_stmt (st : state) : Ast.stmt =
  let pos = peek_pos st in
  let id = fresh_id st in
  ignore (advance st);
  (* 'if' *)
  let cond = parse_expr_no_brace st in
  let then_body = parse_block st in
  skip_newlines st;
  let else_body =
    if peek st = Token.KwElse then begin
      let else_pos = peek_pos st in
      ignore (advance st);
      skip_newlines st;
      let body = if peek st = Token.KwIf then [ parse_if_stmt st ] else parse_block st in
      Some (else_pos, body)
    end
    else None
  in
  { Ast.s_id = id; s_pos = pos; s_kind = Ast.If { cond; then_body; else_body } }

and parse_while_stmt (st : state) : Ast.stmt =
  let pos = peek_pos st in
  let id = fresh_id st in
  ignore (advance st);
  (* 'while' *)
  let cond = parse_expr_no_brace st in
  let body = parse_block st in
  { Ast.s_id = id; s_pos = pos; s_kind = Ast.While { cond; body } }

and parse_for_stmt (st : state) : Ast.stmt =
  let pos = peek_pos st in
  let id = fresh_id st in
  ignore (advance st);
  (* 'for' *)
  let var = expect_ident st "loop variable name" in
  (* `for k, v in m` — the map form (see Ast.For.var2) *)
  let var2 =
    if accept st Token.Comma then Some (expect_ident st "second loop variable name") else None
  in
  expect st Token.KwIn "`in`";
  let iter = parse_expr_no_brace st in
  let body = parse_block st in
  { Ast.s_id = id; s_pos = pos; s_kind = Ast.For { var; var2; iter; body } }

and parse_return_stmt (st : state) : Ast.stmt =
  let pos = peek_pos st in
  let id = fresh_id st in
  ignore (advance st);
  (* 'return' *)
  let value =
    match peek st with
    | Token.Semicolon | Token.Newline | Token.RBrace | Token.Eof -> None
    | _ -> Some (parse_expr st)
  in
  end_of_stmt st;
  { Ast.s_id = id; s_pos = pos; s_kind = Ast.Return value }

(* haxe-parity Task 2: `break`/`continue`. Whether either actually sits
   inside a loop is not checked here (this parser has no loop-nesting
   state, unlike `state.no_brace`) — emit.ml is the gate, exactly the
   existing WO-E403 convention: a construct the emitter has no legal
   jump target for is a diagnostic, not invented bytecode. *)
and parse_break_stmt (st : state) : Ast.stmt =
  let pos = peek_pos st in
  let id = fresh_id st in
  ignore (advance st);
  end_of_stmt st;
  { Ast.s_id = id; s_pos = pos; s_kind = Ast.Break }

and parse_continue_stmt (st : state) : Ast.stmt =
  let pos = peek_pos st in
  let id = fresh_id st in
  ignore (advance st);
  end_of_stmt st;
  { Ast.s_id = id; s_pos = pos; s_kind = Ast.Continue }

(* `do { body } while cond` — no `parse_expr_no_brace` needed for `cond`:
   unlike `if`/`while`, nothing braced follows it (the statement just
   ends), so a constructor literal there is never ambiguous with a
   trailing block, the same reasoning `parse_return_stmt`'s value
   already relies on. *)
and parse_do_while_stmt (st : state) : Ast.stmt =
  let pos = peek_pos st in
  let id = fresh_id st in
  ignore (advance st);
  (* 'do' *)
  let body = parse_block st in
  skip_newlines st;
  expect st Token.KwWhile "`while` after `do { ... }`";
  let cond = parse_expr st in
  end_of_stmt st;
  { Ast.s_id = id; s_pos = pos; s_kind = Ast.DoWhile { body; cond } }

and parse_stmt (st : state) : Ast.stmt =
  match peek st with
  | k when is_insert_trigger k ->
    let pos = peek_pos st in
    let id = fresh_id st in
    let e = parse_insert_expr st in
    end_of_stmt st;
    { Ast.s_id = id; s_pos = pos; s_kind = Ast.ExprStmt e }
  | Token.KwLet -> parse_let_stmt st
  | Token.KwIf -> parse_if_stmt st
  | Token.KwWhile -> parse_while_stmt st
  | Token.KwFor -> parse_for_stmt st
  | Token.KwReturn -> parse_return_stmt st
  | Token.KwBreak -> parse_break_stmt st
  | Token.KwContinue -> parse_continue_stmt st
  | Token.KwDo -> parse_do_while_stmt st
  | _ ->
    let pos = peek_pos st in
    let id = fresh_id st in
    let e = parse_expr st in
    if accept st Token.Eq then begin
      let value = parse_expr st in
      end_of_stmt st;
      { Ast.s_id = id; s_pos = pos; s_kind = Ast.Assign { target = e; value } }
    end
    else begin
      end_of_stmt st;
      { Ast.s_id = id; s_pos = pos; s_kind = Ast.ExprStmt e }
    end

(* Saves/restores state.no_brace around an if/while condition or a
   for-loop's iterable expression — see the state.no_brace doc comment
   and [looks_like_ctor]. *)
and parse_expr_no_brace (st : state) : Ast.expr = with_no_brace st true (fun () -> parse_expr st)

(* `pub` (haxe-parity Task 1, modules) defaults to false: a class body's
   own methods always call this with no `~pub` argument (method-level
   visibility is a different, not-yet-designed question — see
   ast.ml's method_decl.pub doc comment), so this default is what keeps
   every existing call site's behavior byte-identical. *)
let parse_method ?(pub = false) ?(is_static = false) (st : state) : Ast.method_decl =
  let h = parse_sig_head st in
  let body = parse_block st in
  { Ast.id = h.s_id; pos = h.s_pos; name = h.s_name; params = h.s_params; ret = h.s_ret; body; pub;
    is_static }

(* A free top-level function is grammatically identical to a class
   method (signature + brace-delimited body span) — Task 6's brief
   ("free-fn tables") is why this exists as real grammar. *)
let parse_fn_decl ~(pub : bool) (st : state) : Ast.method_decl = parse_method ~pub st

(* Interface signatures have no body: the line ends at a Newline (which
   is consumed) or at the interface's own closing brace / EOF (left for
   the caller). *)
let end_of_sig_line (st : state) : unit =
  match peek st with
  | Token.Newline -> ignore (advance st)
  | Token.RBrace | Token.Eof -> ()
  | _ -> unexpected st "end of method signature (newline)"

let parse_iface_sig (st : state) : Ast.method_sig =
  let h = parse_sig_head st in
  end_of_sig_line st;
  { Ast.id = h.s_id; pos = h.s_pos; name = h.s_name; params = h.s_params; ret = h.s_ret }

(* ---- skip-on-block: service / policy / on -------------------------------

   Ported from rt's skip_block_line and skip_on_block. `service` and
   `policy` lines are skipped up to the end of their logical line
   (brace/bracket/paren-depth aware, so a `service rest "..." expose
   list, get` spanning a `(...)` doesn't end early); `on <event> ...`
   can span many lines and commonly contains `{ k: v }`-shaped object
   literals in its action, so it tracks brace depth across newlines and
   only treats a depth-0 RBrace, or the start of the next class/type-body
   item at depth 0, as its end. This on-block behavior — object literals
   must not be mistaken for the enclosing class/type's own closing brace
   — is the exact load-bearing case the Task 4 brief calls out; it has
   its own fixture. *)

let skip_block_line (st : state) : unit =
  let depth = ref 0 in
  let continue_ = ref true in
  while !continue_ do
    match peek st with
    | Token.Eof -> continue_ := false
    | Token.Newline when !depth = 0 ->
      ignore (advance st);
      continue_ := false
    | Token.RBrace when !depth = 0 -> continue_ := false
    | Token.LBrace | Token.LBracket | Token.LParen ->
      incr depth;
      ignore (advance st)
    | Token.RBrace | Token.RBracket | Token.RParen ->
      decr depth;
      ignore (advance st)
    | _ -> ignore (advance st)
  done

let skip_on_block (st : state) : unit =
  ignore (advance st);
  (* consume `on` *)
  let depth = ref 0 in
  let continue_ = ref true in
  while !continue_ do
    match peek st with
    | Token.Eof -> continue_ := false
    | Token.RBrace when !depth = 0 -> continue_ := false
    | Token.LBrace | Token.LBracket | Token.LParen ->
      incr depth;
      ignore (advance st)
    | Token.RBrace | Token.RBracket | Token.RParen ->
      decr depth;
      ignore (advance st)
    | Token.Newline when !depth = 0 ->
      ignore (advance st);
      skip_newlines st;
      (match peek st with
       | Token.RBrace | Token.KwFn | Token.Eof -> continue_ := false
       | Token.Ident s when is_sync_ident s -> continue_ := false
       | Token.Ident _ when looks_like_field st -> continue_ := false
       | _ -> ())
    | _ -> ignore (advance st)
  done

(* ---- const declaration (haxe-parity Task 2) -----------------------------

   `const NAME = <literal>` — top-level or (bare, no `static`) class-level.
   The brief's own wording is "= literal", not "= expr": restricted here
   to Int (optionally `-`-prefixed, folded directly into the literal —
   no `Unary` wrapper needed for a compile-time value), Text, or Bool.
   Resolved by a dedicated post-parse substitution pass (see `parse`,
   below) rather than threaded through typecheck/owner/emit as a new
   kind of name. *)
let parse_const_literal (st : state) : Ast.expr =
  let pos = peek_pos st in
  let id = fresh_id st in
  match peek st with
  | Token.Int n ->
    ignore (advance st);
    { Ast.id; pos; kind = Ast.IntLit n }
  | Token.Dash -> (
    ignore (advance st);
    match peek st with
    | Token.Int n ->
      ignore (advance st);
      { Ast.id; pos; kind = Ast.IntLit (-n) }
    | _ -> unexpected st "an integer literal after '-'")
  | Token.Str s ->
    ignore (advance st);
    { Ast.id; pos; kind = Ast.StrLit s }
  | Token.KwTrue ->
    ignore (advance st);
    { Ast.id; pos; kind = Ast.BoolLit true }
  | Token.KwFalse ->
    ignore (advance st);
    { Ast.id; pos; kind = Ast.BoolLit false }
  | _ -> unexpected st "a literal (Int, Text, or Bool)"

let parse_const_decl (st : state) : Ast.const_decl =
  let pos = peek_pos st in
  let id = fresh_id st in
  ignore (advance st);
  (* 'const' *)
  let name = expect_ident st "const name" in
  expect st Token.Eq "'=' in const declaration";
  let value = parse_const_literal st in
  end_of_stmt st;
  { Ast.id; pos; name; value }

(* ---- class / type declaration -------------------------------------------

   Task 4 brief: "class Name { ... } and type Name { ... } — identical
   field grammar" and "methods live inside class/type" (no mention of
   rt's plan-13 asymmetry where a plain `type`'s `fn` was
   skip-discarded) — so both keywords get the same body loop here,
   `is_class` recorded purely as data for later stages, never gating
   what's parsed. *)
let parse_class_or_type ?(pub = false) (st : state) (ann : type_annotations) : Ast.class_decl =
  let pos = peek_pos st in
  let is_class = peek st = Token.KwClass in
  if is_class then ignore (advance st) else expect st Token.KwType "`type` or `class`";
  let name = expect_ident st "type/class name" in
  expect st Token.LBrace "'{'";
  let id = fresh_id st in
  let fields = ref [] in
  let methods = ref [] in
  let consts = ref [] in
  let continue_ = ref true in
  while !continue_ do
    skip_newlines st;
    match peek st with
    | Token.RBrace ->
      ignore (advance st);
      continue_ := false
    | Token.Eof ->
      fail st (peek_pos st) syntax_code "unexpected end of input inside type/class body"
    | Token.KwFn -> methods := parse_method st :: !methods
    | Token.KwConst -> consts := parse_const_decl st :: !consts
    (* haxe-parity Task 7: `static`. It is not a keyword (it lexes as a
       plain Ident, so `static` stays a usable identifier elsewhere) —
       one token of lookahead separates the marker from a field named
       `static`, whose next token is a Colon. `static const` is a
       class-scoped constant: the same substitution a bare class `const`
       already gets, so both spellings land in the same list. `static fn`
       carries the marker into the method record. *)
    | Token.Ident "static" when (tok_at st (st.pos + 1)).kind = Token.KwConst ->
      ignore (advance st);
      consts := parse_const_decl st :: !consts
    | Token.Ident "static" when (tok_at st (st.pos + 1)).kind = Token.KwFn ->
      ignore (advance st);
      methods := parse_method ~is_static:true st :: !methods
    (* haxe-parity Task 7: `pub(read) field: T` — read-public, write-
       private. `pub` with no `(read)` is not class-member grammar (member
       visibility beyond this one accessor form is undesigned), so the
       error names what is accepted. *)
    | Token.KwPub ->
      ignore (advance st);
      expect st Token.LParen "'(' after `pub` in a class body — the only member form is `pub(read)`";
      (match peek st with
      | Token.Ident "read" -> ignore (advance st)
      | _ -> unexpected st "`read` — the only accessor form is `pub(read)`");
      expect st Token.RParen "')'";
      fields := parse_field ~pub_read:true st :: !fields
    (* looks_like_field MUST be checked before is_sync_ident: `on`/
       `service`/`policy` are plain Idents here (Task 3 deliberately kept
       them as usable identifiers, unlike rt where they're real keywords
       or otherwise shape-disambiguated), so a field genuinely named one
       of them (`on: Bool`) must win over the skip-block interpretation.
       A real on/service/policy block never has this shape — `on
       update...`, `service rest...`, `policy read...` all have a second
       Ident (not a Colon) right after the leading word — so this
       ordering never mis-classifies a genuine skip-block as a field. *)
    | Token.Ident _ when looks_like_field st -> fields := parse_field st :: !fields
    | Token.Ident s when is_sync_ident s -> if s = "on" then skip_on_block st else skip_block_line st
    | _ -> unexpected st "a field, method, or service/policy/on block"
  done;
  {
    Ast.id;
    pos;
    name;
    is_class;
    is_record = false;
    is_gc = ann.is_gc;
    table = ann.table;
    fields = List.rev !fields;
    methods = List.rev !methods;
    consts = List.rev !consts;
    pub;
  }

(* ---- typedef record declaration (haxe-parity Task 4) ---------------------

   `typedef Name = { field: Type [= default] [, ...] ?opt: Type ... }` —
   a structural record alias. Fields only (no methods, no consts, no
   service/policy/on leniency — a record is pure data shape); separators
   are newlines OR commas (the sample writes both: multi-line SupConfig,
   single-line HttpResp). A `?` prefixing the field NAME (`?detections:
   Text`, the Haxe `@:optional` spelling) desugars to the field typed
   `?T` — "?fields land as nullable-by-shape" (the task brief's own
   words): one representation, `Nullable`, whether the `?` was written
   on the name or on the type, so Task 6's forced-handling work has a
   single shape to tighten. *)
let parse_record_decl ?(pub = false) (st : state) : Ast.class_decl =
  let pos = peek_pos st in
  ignore (advance st);
  (* 'typedef' *)
  let name = expect_ident st "typedef name" in
  expect st Token.Eq "'=' in typedef declaration";
  expect st Token.LBrace "'{' to open typedef record body";
  let id = fresh_id st in
  let fields = ref [] in
  let continue_ = ref true in
  while !continue_ do
    skip_newlines st;
    match peek st with
    | Token.RBrace ->
      ignore (advance st);
      continue_ := false
    | Token.Eof -> fail st (peek_pos st) syntax_code "unexpected end of input inside typedef body"
    | _ ->
      let optional = accept st Token.Question in
      let f = parse_field ~comma_ends:true st in
      let f =
        if optional then
          match f.Ast.ty with
          | Ast.Nullable _ -> f (* `?opt: ?T` — already nullable, don't double-wrap *)
          | ty -> { f with Ast.ty = Ast.Nullable ty }
        else f
      in
      fields := f :: !fields;
      ignore (accept st Token.Comma)
  done;
  {
    Ast.id;
    pos;
    name;
    is_class = false;
    is_record = true;
    is_gc = false;
    table = None;
    fields = List.rev !fields;
    methods = [];
    consts = [];
    pub;
  }

(* ---- union declaration (haxe-parity Task 4) -------------------------------

   `type Name = V1 | V2 | V3(field: Type, ...)` — a tagged union,
   sharing the `type` keyword with the struct form (`type Name { ... }`)
   and disambiguated by the token after the name (`=` vs `{`, decided by
   parse_program's two-token lookahead before either parser runs). A
   variant's payload reuses the field grammar's `name: Type` pairs,
   comma-separated inside its parens; a newline is allowed after a `|`
   (so a long union can wrap) but the declaration otherwise ends the way
   a `use`/`let` line does (end_of_stmt). *)
let parse_union_decl ?(pub = false) (st : state) : Ast.union_decl =
  let pos = peek_pos st in
  ignore (advance st);
  (* 'type' *)
  let name = expect_ident st "union name" in
  let id = fresh_id st in
  expect st Token.Eq "'=' in union declaration";
  let parse_variant () : Ast.variant_decl =
    let v_pos = peek_pos st in
    let v_name = expect_ident st "variant name" in
    let v_fields = ref [] in
    if accept st Token.LParen then begin
      let more = ref (peek st <> Token.RParen) in
      while !more do
        let fname = expect_ident st "payload field name" in
        expect st Token.Colon "':'";
        let fty = parse_field_ty st in
        v_fields := (fname, fty) :: !v_fields;
        if not (accept st Token.Comma) then more := false
      done;
      expect st Token.RParen "')'"
    end;
    { Ast.v_pos; v_name; v_fields = List.rev !v_fields }
  in
  let variants = ref [ parse_variant () ] in
  while accept st Token.Pipe do
    skip_newlines st;
    variants := parse_variant () :: !variants
  done;
  end_of_stmt st;
  { Ast.id; pos; name; variants = List.rev !variants; pub }

(* ---- interface declaration ----------------------------------------------

   Signatures only — no fields, no bodies (Task 4 brief: "interface Name
   { fn sig... } (signatures only)"). Anything other than `fn` inside an
   interface body is a parse error; interfaces don't get the
   service/policy/on leniency class/type bodies get. *)
let parse_interface ?(pub = false) (st : state) : Ast.interface_decl =
  let pos = peek_pos st in
  expect st Token.KwInterface "`interface`";
  let name = expect_ident st "interface name" in
  expect st Token.LBrace "'{'";
  let id = fresh_id st in
  let methods = ref [] in
  let continue_ = ref true in
  while !continue_ do
    skip_newlines st;
    match peek st with
    | Token.RBrace ->
      ignore (advance st);
      continue_ := false
    | Token.Eof -> fail st (peek_pos st) syntax_code "unexpected end of input inside interface body"
    | Token.KwFn -> methods := parse_iface_sig st :: !methods
    | _ -> unexpected st "a method signature (`fn ...`)"
  done;
  { Ast.id; pos; name; methods = List.rev !methods; pub }

(* ---- use declaration (haxe-parity Task 1, modules) ----------------------

   `use fs` (a reserved stdlib namespace) or `use shared/util` (a
   project-relative path — slash-separated directory segments naming
   another discovered module's directory). Which of those two a given
   path actually is, and whether it resolves at all, is the resolver's
   job (types.ml) — this only demands at least one identifier segment,
   with `/`-separated continuations, ended the same way a `let`/return
   statement is (`end_of_stmt`: optional `;`, then newline/EOF — there
   is no enclosing block at top level, but end_of_stmt's RBrace arm is
   harmless dead code here, never reached). *)
let parse_use_decl (st : state) : Ast.use_decl =
  let pos = peek_pos st in
  let id = fresh_id st in
  ignore (advance st);
  (* 'use' *)
  let first = expect_ident st "module name" in
  let segments = ref [ first ] in
  while accept st Token.Slash do
    segments := expect_ident st "module path segment" :: !segments
  done;
  end_of_stmt st;
  { Ast.id; pos; segments = List.rev !segments }

(* ---- top-level program ---------------------------------------------------

   `@table`/`@gc` annotations (like rt) may only prefix a `type`/`class`
   — not `interface`, not a free `fn`. Every arm is wrapped by the same
   try/with: on Parse_error (already reported at its raise site, see
   [fail]), sync to the next top-level construct and keep going —
   exactly one diagnostic per broken declaration, never a cascade. *)
(* `type Name = ...` is a union; `type Name { ... }` stays the struct
   form — two-token lookahead past the name (haxe-parity Task 4),
   mirroring looks_like_ctor's identifier-then-brace trick one token
   further out. Anything else after the name falls to
   parse_class_or_type's own "expected '{'" error, unchanged. *)
let looks_like_union (st : state) : bool =
  (match (tok_at st (st.pos + 1)).kind with Token.Ident _ -> true | _ -> false)
  && (tok_at st (st.pos + 2)).kind = Token.Eq

let parse_program (st : state) : Ast.program =
  let decls = ref [] in
  let continue_ = ref true in
  while !continue_ do
    skip_newlines st;
    if at_end st then continue_ := false
    else begin
      (try
         match peek st with
         | Token.KwType when looks_like_union st ->
           decls := Ast.Union (parse_union_decl st) :: !decls
         | Token.KwType | Token.KwClass ->
           decls := Ast.Class (parse_class_or_type st no_annotations) :: !decls
         | Token.KwTypedef -> decls := Ast.Class (parse_record_decl st) :: !decls
         | Token.KwInterface -> decls := Ast.Interface (parse_interface st) :: !decls
         | Token.KwFn -> decls := Ast.Fn (parse_fn_decl ~pub:false st) :: !decls
         | Token.KwUse -> decls := Ast.Use (parse_use_decl st) :: !decls
         | Token.KwConst -> decls := Ast.Const (parse_const_decl st) :: !decls
         | Token.KwInline ->
           let ipos = peek_pos st in
           ignore (advance st);
           (match peek st with
            | Token.KwFn ->
              fail st ipos inline_fn_code
                "`inline fn` is rejected — optimization is the compiler's job (const values are \
                 the adopted half of this row)"
            | _ -> unexpected st "`fn` after `inline`")
         | Token.KwPub -> (
           ignore (advance st);
           (* consume 'pub'; a stray '(' here (`pub(read)` at top level —
              Task 7's field-accessor marker, not this task's) falls
              through to the same clean "expected ... after `pub`" error
              as any other unrecognized token, rather than being
              half-parsed. *)
           match peek st with
           | Token.KwType when looks_like_union st ->
             decls := Ast.Union (parse_union_decl ~pub:true st) :: !decls
           | Token.KwType | Token.KwClass ->
             decls := Ast.Class (parse_class_or_type ~pub:true st no_annotations) :: !decls
           | Token.KwTypedef -> decls := Ast.Class (parse_record_decl ~pub:true st) :: !decls
           | Token.KwInterface -> decls := Ast.Interface (parse_interface ~pub:true st) :: !decls
           | Token.KwFn -> decls := Ast.Fn (parse_fn_decl ~pub:true st) :: !decls
           | _ -> unexpected st "`type`, `class`, `interface`, or `fn` after `pub`")
         | Token.At ->
           let ann = parse_type_annotations st in
           skip_newlines st;
           (match peek st with
            | Token.KwType | Token.KwClass ->
              decls := Ast.Class (parse_class_or_type st ann) :: !decls
            | _ -> unexpected st "`type` or `class` after annotation")
         | _ -> unexpected st "a top-level declaration (type/class/interface/fn/@annotation)"
       with Parse_error -> sync_to_next_top_level st)
    end
  done;
  { Ast.decls = List.rev !decls }

(* ---- const substitution (haxe-parity Task 2) ----------------------------

   Runs once, over the whole freshly-parsed program, right before `parse`
   returns it. Replaces every unshadowed `Ident NAME` with the literal
   expr NAME's `const` declared, so typecheck/owner/emit see a plain
   literal and need zero const-specific code anywhere downstream — the
   same "desugar early, touch nothing later" shape string interpolation
   already uses in this file. Scope-aware exactly like types.ml's own
   local-shadows-a-`use`-alias fix (Task 1 review): a local, parameter,
   `for` variable, or `self` binding of the same name always wins over a
   const of that name, so `fn f(CHUNK: Int) { return CHUNK }` next to a
   top-level `const CHUNK = 65536` still returns the parameter, not
   65536. A const's own value is grammar-restricted to a literal
   (parse_const_literal) — never an `Ident` — so one const's value can
   never need substitution itself; no ordering/cycle question arises. *)
module StringMap = Map.Make (String)
module StringSet = Set.Make (String)

let rec subst_expr (consts : Ast.expr StringMap.t) (bound : StringSet.t) (e : Ast.expr) : Ast.expr =
  match e.Ast.kind with
  | Ast.IntLit _ | Ast.StrLit _ | Ast.BoolLit _ | Ast.DbStub _ -> e
  | Ast.Ident name ->
    if StringSet.mem name bound then e
    else ( match StringMap.find_opt name consts with Some v -> { e with Ast.kind = v.Ast.kind } | None -> e)
  | Ast.Field (base, fname) -> { e with Ast.kind = Ast.Field (subst_expr consts bound base, fname) }
  | Ast.Index (base, idx) ->
    { e with Ast.kind = Ast.Index (subst_expr consts bound base, subst_expr consts bound idx) }
  | Ast.Call (callee, args) ->
    { e with Ast.kind = Ast.Call (subst_expr consts bound callee, List.map (subst_expr consts bound) args) }
  | Ast.Unary (op, operand) -> { e with Ast.kind = Ast.Unary (op, subst_expr consts bound operand) }
  | Ast.Binary (op, l, r) ->
    { e with Ast.kind = Ast.Binary (op, subst_expr consts bound l, subst_expr consts bound r) }
  | Ast.Ctor (cn, fields) ->
    { e with Ast.kind = Ast.Ctor (cn, List.map (fun (n, v) -> (n, subst_expr consts bound v)) fields) }
  | Ast.Insert (cn, fields) ->
    { e with Ast.kind = Ast.Insert (cn, List.map (fun (n, v) -> (n, subst_expr consts bound v)) fields) }
  | Ast.Delete t -> { e with Ast.kind = Ast.Delete (subst_expr consts bound t) }
  | Ast.Query q ->
    (* the range/group vars shadow consts inside the query body *)
    let bound' = StringSet.add q.Ast.q_var bound in
    let bound' = match q.Ast.q_group with Some (g, _) -> StringSet.add g bound' | None -> bound' in
    let sub = subst_expr consts bound' in
    { e with Ast.kind = Ast.Query {
        q with Ast.q_src = (match q.Ast.q_src with
                            | Ast.QTable cn -> Ast.QTable cn
                            | Ast.QNav e2 -> Ast.QNav (subst_expr consts bound e2));
               q_wheres = List.map sub q.Ast.q_wheres;
               q_group = (match q.Ast.q_group with Some (g, k) -> Some (g, sub k) | None -> None);
               q_order = (match q.Ast.q_order with Some (k, d) -> Some (sub k, d) | None -> None);
               q_take = (match q.Ast.q_take with Some t -> Some (sub t) | None -> None);
               q_select = sub q.Ast.q_select } }
  | Ast.Interp inner -> { e with Ast.kind = Ast.Interp (subst_expr consts bound inner) }
  | Ast.ListLit items -> { e with Ast.kind = Ast.ListLit (List.map (subst_expr consts bound) items) }
  | Ast.MapLit | Ast.NilLit -> e
  | Ast.As (inner, ty) -> { e with Ast.kind = Ast.As (subst_expr consts bound inner, ty) }
  | Ast.Try { body; ename; handler } ->
    { e with
      Ast.kind =
        Ast.Try
          { body = subst_expr consts bound body; ename;
            handler = subst_block consts (StringSet.add ename bound) handler }
    }
  | Ast.Switch (subject, arms) ->
    { e with
      Ast.kind =
        Ast.Switch
          ( subst_expr consts bound subject,
            List.map
              (fun (a : Ast.switch_arm) ->
                { a with
                  Ast.values = List.map (subst_expr consts bound) a.Ast.values;
                  body = subst_block consts bound a.Ast.body;
                })
              arms )
    }

(* `let` extends the rest of *this* block only, exactly like types.ml's
   walk_stmt/walk_block: a nested block's own `let`s never leak back out
   to the caller's bound set.

   `subst_expr` now reaches into `Switch`'s own `stmt list` arm bodies
   (haxe-parity Task 3), so it and `subst_block`/`subst_stmt` are one
   `and`-chain from here on, not two separate `let rec` groups — the
   same merge ast.ml's own `expr`/`stmt` needed for the same reason. *)
and subst_block (consts : Ast.expr StringMap.t) (bound : StringSet.t) (body : Ast.stmt list) :
    Ast.stmt list =
  let bound_ref = ref bound in
  List.map
    (fun (s : Ast.stmt) ->
      let s' = subst_stmt consts !bound_ref s in
      (match s.Ast.s_kind with
      | Ast.Let { name; _ } -> bound_ref := StringSet.add name !bound_ref
      | _ -> ());
      s')
    body

and subst_stmt (consts : Ast.expr StringMap.t) (bound : StringSet.t) (s : Ast.stmt) : Ast.stmt =
  let e = subst_expr consts bound in
  match s.Ast.s_kind with
  | Ast.Let { name; ty; value } -> { s with Ast.s_kind = Ast.Let { name; ty; value = e value } }
  | Ast.Assign { target; value } -> { s with Ast.s_kind = Ast.Assign { target = e target; value = e value } }
  | Ast.If { cond; then_body; else_body } ->
    { s with
      Ast.s_kind =
        Ast.If
          { cond = e cond;
            then_body = subst_block consts bound then_body;
            else_body = Option.map (fun (p, b) -> (p, subst_block consts bound b)) else_body
          }
    }
  | Ast.While { cond; body } ->
    { s with Ast.s_kind = Ast.While { cond = e cond; body = subst_block consts bound body } }
  | Ast.For { var; var2; iter; body } ->
    let bound' =
      match var2 with
      | Some v2 -> StringSet.add v2 (StringSet.add var bound)
      | None -> StringSet.add var bound
    in
    { s with Ast.s_kind = Ast.For { var; var2; iter = e iter; body = subst_block consts bound' body } }
  | Ast.Return opt -> { s with Ast.s_kind = Ast.Return (Option.map e opt) }
  | Ast.ExprStmt ex -> { s with Ast.s_kind = Ast.ExprStmt (e ex) }
  | Ast.Break | Ast.Continue -> s
  | Ast.DoWhile { body; cond } ->
    { s with Ast.s_kind = Ast.DoWhile { body = subst_block consts bound body; cond = e cond } }

let params_bound (base : StringSet.t) (params : Ast.param list) : StringSet.t =
  List.fold_left (fun acc (p : Ast.param) -> StringSet.add p.Ast.name acc) base params

let const_map (consts : Ast.const_decl list) : Ast.expr StringMap.t =
  List.fold_left (fun acc (c : Ast.const_decl) -> StringMap.add c.Ast.name c.Ast.value acc) StringMap.empty consts

(* Every top-level `const` a program declares, for a caller that needs to
   substitute one file's constants into ANOTHER file of the same module —
   every file in a directory is unconditionally visible to every other
   (haxe-parity Task 1's discovery contract), so a `const CHUNK = 65536` in
   one file is in scope in its neighbours. *)
let top_level_consts (prog : Ast.program) : Ast.expr StringMap.t =
  const_map (List.filter_map (function Ast.Const c -> Some c | _ -> None) prog.Ast.decls)

(* [extra] holds constants declared elsewhere (a sibling file's); this
   program's own top-level ones win on a name collision, exactly as a
   class-level const outranks a top-level one below. *)
let subst_consts ?(extra = StringMap.empty) (prog : Ast.program) : Ast.program =
  let top_consts = StringMap.fold StringMap.add (top_level_consts prog) extra in
  let decls' =
    List.map
      (function
        | Ast.Class c ->
          (* class consts win over top-level ones on a name collision --
             innermost scope wins, matching how a param/local also
             outranks either. *)
          let merged = StringMap.fold StringMap.add (const_map c.Ast.consts) top_consts in
          let methods' =
            List.map
              (fun (m : Ast.method_decl) ->
                let bound = params_bound (StringSet.singleton "self") m.Ast.params in
                { m with Ast.body = subst_block merged bound m.Ast.body })
              c.Ast.methods
          in
          Ast.Class { c with Ast.methods = methods' }
        | Ast.Fn fn ->
          let bound = params_bound StringSet.empty fn.Ast.params in
          Ast.Fn { fn with Ast.body = subst_block top_consts bound fn.Ast.body }
        | (Ast.Interface _ | Ast.Use _ | Ast.Const _ | Ast.Union _) as d -> d)
      prog.Ast.decls
  in
  { Ast.decls = decls' }

let parse (collector : Diag.Collector.t) ~(file : string) (toks : Token.t list) : Ast.program =
  let st = make collector ~file toks in
  subst_consts (parse_program st)
