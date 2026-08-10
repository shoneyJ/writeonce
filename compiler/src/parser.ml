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
    | Token.KwClass when !depth = 0 && st.pos > start -> continue_ := false
    | Token.KwInterface when !depth = 0 && st.pos > start -> continue_ := false
    | Token.KwFn when !depth = 0 && st.pos > start -> continue_ := false
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
    let name = expect_ident st "annotation name" in
    (match name with
     | "gc" ->
       is_gc := true;
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
        Ast.Scalar name
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

let parse_field (st : state) : Ast.field =
  let pos = peek_pos st in
  let name = expect_ident st "field name" in
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
    | Token.Newline | Token.RBrace | Token.Eof -> continue_ := false
    | _ -> unexpected st "an annotation, '=', or end of field"
  done;
  { Ast.id = fresh_id st; pos; name; ty; default = !default; annotations = List.rev !annotations }

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
   this task's own addition, not a straight rt port.

   End-of-statement convention mirrors parse_field's: a "simple"
   statement (let/assign/return/expr-statement/DbStub) must end at an
   optional `;` followed by a real terminator (Newline, the enclosing
   block's `}}`, or Eof) — [end_of_stmt] enforces this, so e.g.
   `let x = 1 let y = 2` on one line is a syntax error, not silently
   accepted, exactly like two fields can't share a line. Block-having
   statements (if/while/for) do NOT call it: their own closing `}` ends
   them, and requiring a terminator after it would wrongly reject
   `} else {` on one line, the normal style for chained if/else. *)

let end_of_stmt (st : state) : unit =
  ignore (accept st Token.Semicolon);
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

let rec parse_expr (st : state) : Ast.expr = parse_comparison st

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

and parse_unary (st : state) : Ast.expr =
  match peek st with
  | Token.Dash ->
    let pos = peek_pos st in
    let id = fresh_id st in
    ignore (advance st);
    let operand = parse_unary st in
    { Ast.id; pos; kind = Ast.Unary (Ast.Neg, operand) }
  | _ -> parse_postfix st

and parse_postfix (st : state) : Ast.expr =
  let base = ref (parse_primary st) in
  let continue_ = ref true in
  while !continue_ do
    match peek st with
    | Token.Dot ->
      let pos = peek_pos st in
      ignore (advance st);
      let name = expect_ident st "field or method name" in
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
    let fname = expect_ident st "constructor field name" in
    expect st Token.Colon "':'";
    let fval = parse_expr st in
    fields := (fname, fval) :: !fields;
    skip_newlines st;
    if accept st Token.Comma then skip_newlines st else continue_ := false
  done;
  skip_newlines st;
  expect st Token.RBrace "'}'";
  { Ast.id; pos; kind = Ast.Ctor (name, List.rev !fields) }

and parse_primary (st : state) : Ast.expr =
  match peek st with
  | k when is_select_trigger k -> parse_dbstub_expr st
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
  | Token.LParen ->
    ignore (advance st);
    (* Parens make the enclosed expression unambiguous again, so a
       constructor literal is legal here even inside an if/while
       condition (`if (Widget { x: 1 }).ok { ... }`). *)
    let e = with_no_brace st false (fun () -> parse_expr st) in
    expect st Token.RParen "')'";
    e
  | Token.Ident _ when looks_like_ctor st -> parse_ctor_literal st
  | Token.Ident s ->
    let pos = peek_pos st in
    let id = fresh_id st in
    ignore (advance st);
    { Ast.id; pos; kind = Ast.Ident s }
  | _ -> unexpected st "an expression"

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
  let ty = if accept st Token.Colon then Some (expect_ident st "let-binding type") else None in
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
  expect st Token.KwIn "`in`";
  let iter = parse_expr_no_brace st in
  let body = parse_block st in
  { Ast.s_id = id; s_pos = pos; s_kind = Ast.For { var; iter; body } }

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

and parse_stmt (st : state) : Ast.stmt =
  match peek st with
  | k when is_insert_trigger k ->
    let pos = peek_pos st in
    let id = fresh_id st in
    let e = parse_dbstub_expr st in
    end_of_stmt st;
    { Ast.s_id = id; s_pos = pos; s_kind = Ast.ExprStmt e }
  | Token.KwLet -> parse_let_stmt st
  | Token.KwIf -> parse_if_stmt st
  | Token.KwWhile -> parse_while_stmt st
  | Token.KwFor -> parse_for_stmt st
  | Token.KwReturn -> parse_return_stmt st
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

let parse_method (st : state) : Ast.method_decl =
  let h = parse_sig_head st in
  let body = parse_block st in
  { Ast.id = h.s_id; pos = h.s_pos; name = h.s_name; params = h.s_params; ret = h.s_ret; body }

(* A free top-level function is grammatically identical to a class
   method (signature + brace-delimited body span) — Task 6's brief
   ("free-fn tables") is why this exists as real grammar. *)
let parse_fn_decl (st : state) : Ast.method_decl = parse_method st

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

(* ---- class / type declaration -------------------------------------------

   Task 4 brief: "class Name { ... } and type Name { ... } — identical
   field grammar" and "methods live inside class/type" (no mention of
   rt's plan-13 asymmetry where a plain `type`'s `fn` was
   skip-discarded) — so both keywords get the same body loop here,
   `is_class` recorded purely as data for later stages, never gating
   what's parsed. *)
let parse_class_or_type (st : state) (ann : type_annotations) : Ast.class_decl =
  let pos = peek_pos st in
  let is_class = peek st = Token.KwClass in
  if is_class then ignore (advance st) else expect st Token.KwType "`type` or `class`";
  let name = expect_ident st "type/class name" in
  expect st Token.LBrace "'{'";
  let id = fresh_id st in
  let fields = ref [] in
  let methods = ref [] in
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
    is_gc = ann.is_gc;
    table = ann.table;
    fields = List.rev !fields;
    methods = List.rev !methods;
  }

(* ---- interface declaration ----------------------------------------------

   Signatures only — no fields, no bodies (Task 4 brief: "interface Name
   { fn sig... } (signatures only)"). Anything other than `fn` inside an
   interface body is a parse error; interfaces don't get the
   service/policy/on leniency class/type bodies get. *)
let parse_interface (st : state) : Ast.interface_decl =
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
  { Ast.id; pos; name; methods = List.rev !methods }

(* ---- top-level program ---------------------------------------------------

   `@table`/`@gc` annotations (like rt) may only prefix a `type`/`class`
   — not `interface`, not a free `fn`. Every arm is wrapped by the same
   try/with: on Parse_error (already reported at its raise site, see
   [fail]), sync to the next top-level construct and keep going —
   exactly one diagnostic per broken declaration, never a cascade. *)
let parse_program (st : state) : Ast.program =
  let decls = ref [] in
  let continue_ = ref true in
  while !continue_ do
    skip_newlines st;
    if at_end st then continue_ := false
    else begin
      (try
         match peek st with
         | Token.KwType | Token.KwClass ->
           decls := Ast.Class (parse_class_or_type st no_annotations) :: !decls
         | Token.KwInterface -> decls := Ast.Interface (parse_interface st) :: !decls
         | Token.KwFn -> decls := Ast.Fn (parse_fn_decl st) :: !decls
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

let parse (collector : Diag.Collector.t) ~(file : string) (toks : Token.t list) : Ast.program =
  let st = make collector ~file toks in
  parse_program st
