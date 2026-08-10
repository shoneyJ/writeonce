(* dump.ml — stable text dumps of compiler-internal data.

   Used both by `woc --dump-*` flags (compiler/bin/main.ml) and the
   golden-file test runner (compiler/test/runner.ml) that diffs
   against compiler/test/golden/. These formats are load-bearing test
   contracts, not debug output: once a fixture's `.expected` file is
   checked in, renaming a kind's dumped label is a breaking change to
   every golden fixture that contains it. Grows one `dump_<stage>`
   function per task (Task 3 adds dump_tokens; Task 4 adds dump_ast;
   Task 6 dump_types; Task 7 dump_owner).

   Token dump format (one line per token):

     LINE:COL KIND
     LINE:COL KIND(payload)

   e.g. "3:1 KW_CLASS", "3:7 IDENT(Product)", "4:12 INT(42)",
   "4:20 STR(hello)", "5:1 NEWLINE". LINE and COL are 1-based. *)

(* One stable, upper-snake-case label per Token.kind constructor.
   Written as an exhaustive match with no wildcard, on purpose: adding
   a Token.kind case without adding it here is a compile error (a
   non-exhaustive-match warning promoted to an error by dune's default
   build profile), not a silently unlabelled dump line. *)
let kind_label (k : Token.kind) : string =
  match k with
  | Token.Ident s -> Printf.sprintf "IDENT(%s)" s
  | Token.Int n -> Printf.sprintf "INT(%d)" n
  | Token.Str s -> Printf.sprintf "STR(%s)" s
  | Token.KwType -> "KW_TYPE"
  | Token.KwClass -> "KW_CLASS"
  | Token.KwInterface -> "KW_INTERFACE"
  | Token.KwFn -> "KW_FN"
  | Token.KwLet -> "KW_LET"
  | Token.KwMut -> "KW_MUT"
  | Token.KwTake -> "KW_TAKE"
  | Token.KwReturn -> "KW_RETURN"
  | Token.KwIf -> "KW_IF"
  | Token.KwElse -> "KW_ELSE"
  | Token.KwWhile -> "KW_WHILE"
  | Token.KwFor -> "KW_FOR"
  | Token.KwIn -> "KW_IN"
  | Token.KwTrue -> "KW_TRUE"
  | Token.KwFalse -> "KW_FALSE"
  | Token.KwInsert -> "KW_INSERT"
  | Token.KwSelect -> "KW_SELECT"
  | Token.LBrace -> "LBRACE"
  | Token.RBrace -> "RBRACE"
  | Token.LParen -> "LPAREN"
  | Token.RParen -> "RPAREN"
  | Token.LBracket -> "LBRACKET"
  | Token.RBracket -> "RBRACKET"
  | Token.Comma -> "COMMA"
  | Token.Semicolon -> "SEMICOLON"
  | Token.Colon -> "COLON"
  | Token.Dot -> "DOT"
  | Token.DotDot -> "DOTDOT"
  | Token.Question -> "QUESTION"
  | Token.At -> "AT"
  | Token.Pipe -> "PIPE"
  | Token.Arrow -> "ARROW"
  | Token.FatArrow -> "FAT_ARROW"
  | Token.Dash -> "DASH"
  | Token.Plus -> "PLUS"
  | Token.Star -> "STAR"
  | Token.Slash -> "SLASH"
  | Token.Percent -> "PERCENT"
  | Token.Eq -> "EQ"
  | Token.EqEq -> "EQEQ"
  | Token.NotEq -> "NOTEQ"
  | Token.Lt -> "LT"
  | Token.LtEq -> "LTEQ"
  | Token.Gt -> "GT"
  | Token.GtEq -> "GTEQ"
  | Token.PlusEq -> "PLUSEQ"
  | Token.MinusEq -> "MINUSEQ"
  | Token.Newline -> "NEWLINE"
  | Token.Eof -> "EOF"

(* Multi-file dump layout (Task 8, bin/main.ml). Every dump_* function
   above still renders exactly one file's contribution — that contract
   doesn't change. When a --dump-* flag's <path> resolves to more than
   one discovered file, main.ml prints each file's dump_* output one
   after another in sorted discovery order, each preceded by this
   separator line naming the file. For a single file, main.ml never
   calls this, so every dump's stdout stays byte-identical to every
   pre-Task-8 golden fixture. *)
let file_header (path : string) : string = Printf.sprintf "=== %s ===\n" path

let dump_tokens (toks : Token.t list) : string =
  let lines =
    List.map
      (fun (t : Token.t) -> Printf.sprintf "%d:%d %s" t.line t.col (kind_label t.kind))
      toks
  in
  match lines with [] -> "" | _ -> String.concat "\n" lines ^ "\n"

(* dump_ast — stable indented-tree dump of the declaration AST (Task 4;
   Task 5 adds real statement/expression rendering under METHOD).

   One node per line: "LINE:COL KIND payload", children indented two
   spaces under their parent. Node ids are deliberately never printed
   (Task 4 brief: "ids would churn goldens" — they're an internal,
   monotonic-per-parse detail Tasks 6/7 key side tables on, not a
   stable rendering surface); positions are, since they're what makes
   the dump useful as a fixture at all.

   Statements get one line each (dump_stmt), matching how fields/
   methods already get one line each under their class — the natural
   "line" granularity for a body, not one dump line per sub-expression.
   Expressions render inline as a single unparsed string (expr_str),
   the same choice this file already made for field types/defaults/
   signatures (field_ty_str/default_str/sig_str): readable golden files
   that look like source, not an exploded parse tree. Expression nodes
   still carry their own id/pos in the AST (ast.ml) for Tasks 6/7's
   side tables; the dump just doesn't surface them, same as decl ids. *)

let pos_str (p : Ast.pos) : string = Printf.sprintf "%d:%d" p.line p.col

let conv_str : Ast.param_conv -> string = function
  | Ast.Borrow -> ""
  | Ast.Mut -> "mut "
  | Ast.Take -> "take "

let rec field_ty_str : Ast.field_ty -> string = function
  | Ast.Scalar s -> s
  | Ast.Ref s -> Printf.sprintf "ref %s" s
  | Ast.Multi s -> Printf.sprintf "multi %s" s
  | Ast.Map (k, v) -> Printf.sprintf "map<%s, %s>" k v
  | Ast.Nullable t -> "?" ^ field_ty_str t

let param_str (p : Ast.param) : string = Printf.sprintf "%s%s: %s" (conv_str p.conv) p.name (field_ty_str p.ty)
let params_str (params : Ast.param list) : string = String.concat ", " (List.map param_str params)

(* An opaque default's token span is rendered via kind_label, same as
   --dump-tokens, rather than a second ad hoc "unparse a token" writer —
   one canonical, exhaustive way to turn a Token.kind into stable text. *)
let default_str : Ast.default_expr -> string = function
  | Ast.DefaultNow -> " = now()"
  | Ast.DefaultOpaque toks ->
    " = " ^ String.concat " " (List.map (fun (t : Token.t) -> kind_label t.kind) toks)

let annotations_str (anns : string list) : string =
  String.concat "" (List.map (fun a -> " @" ^ a) anns)

let dump_field (f : Ast.field) : string =
  Printf.sprintf "%s FIELD %s: %s%s%s" (pos_str f.pos) f.name (field_ty_str f.ty)
    (match f.default with None -> "" | Some d -> default_str d)
    (annotations_str f.annotations)

let sig_str (name : string) (params : Ast.param list) (ret : Ast.field_ty option) : string =
  Printf.sprintf "%s(%s)%s" name (params_str params)
    (match ret with None -> "" | Some r -> " -> " ^ field_ty_str r)

let dump_method_sig (m : Ast.method_sig) : string =
  Printf.sprintf "%s METHOD %s" (pos_str m.pos) (sig_str m.name m.params m.ret)

let binop_str : Ast.binop -> string = function
  | Ast.Add -> "+"
  | Ast.Sub -> "-"
  | Ast.Mul -> "*"
  | Ast.Div -> "/"
  | Ast.Mod -> "%"
  | Ast.Concat -> ".."
  | Ast.Eq -> "=="
  | Ast.Ne -> "!="
  | Ast.Lt -> "<"
  | Ast.Le -> "<="
  | Ast.Gt -> ">"
  | Ast.Ge -> ">="

(* Raw token span shared by both DbStub renderings below: a statement-
   position DbStub (dump_stmt) and an expression-position one nested
   inside a LET/ASSIGN/etc. (expr_str). *)
let dbstub_tokens_str (toks : Token.t list) : string =
  String.concat " " (List.map (fun (t : Token.t) -> kind_label t.kind) toks)

(* expr_str — one unparsed line per expression, no positions (see this
   file's module doc for why: same "inline text, not an exploded tree"
   choice as field_ty_str/default_str). Recurses structurally; no
   precedence-driven parenthesization since every expr_str call site
   here only ever needs "readable enough to eyeball in a golden file",
   not a round-trippable unparser. *)
let rec expr_str (e : Ast.expr) : string =
  match e.Ast.kind with
  | Ast.IntLit n -> string_of_int n
  | Ast.StrLit s -> "\"" ^ s ^ "\""
  | Ast.BoolLit b -> if b then "true" else "false"
  | Ast.Ident s -> s
  | Ast.Field (base, name) -> expr_str base ^ "." ^ name
  | Ast.Index (base, idx) -> expr_str base ^ "[" ^ expr_str idx ^ "]"
  | Ast.Call (callee, args) ->
    Printf.sprintf "%s(%s)" (expr_str callee) (String.concat ", " (List.map expr_str args))
  | Ast.Unary (Ast.Neg, operand) -> "-" ^ expr_str operand
  | Ast.Binary (op, l, r) -> Printf.sprintf "%s %s %s" (expr_str l) (binop_str op) (expr_str r)
  | Ast.Ctor (name, fields) ->
    Printf.sprintf "%s { %s }" name
      (String.concat ", "
         (List.map (fun (fname, fval) -> Printf.sprintf "%s: %s" fname (expr_str fval)) fields))
  | Ast.DbStub toks -> Printf.sprintf "DB_STUB(%s)" (dbstub_tokens_str toks)

(* dump_stmt — one line per statement (LINE:COL KIND detail), matching
   dump_field/dump_method_sig's "one descriptive line" convention;
   block-having statements (IF/WHILE/FOR) get their body's statements
   as children, indented two spaces, same nesting rule as
   class/interface members. A bare DbStub expression-statement (the
   `insert`/`select` sublanguage — see ast.ml/parser.ml) is special-
   cased to a standalone "DB_STUB ..." line rather than "EXPR
   DB_STUB(...)", so it reads as its own concept, not a generic
   expression statement that happens to contain one. *)
let rec dump_stmt (s : Ast.stmt) : string list =
  let indent_block (body : Ast.stmt list) : string list =
    List.concat_map (fun st -> List.map (fun l -> "  " ^ l) (dump_stmt st)) body
  in
  match s.Ast.s_kind with
  | Ast.Let { name; ty; value } ->
    let ty_part = match ty with None -> "" | Some t -> ": " ^ t in
    [ Printf.sprintf "%s LET %s%s = %s" (pos_str s.Ast.s_pos) name ty_part (expr_str value) ]
  | Ast.Assign { target; value } ->
    [ Printf.sprintf "%s ASSIGN %s = %s" (pos_str s.Ast.s_pos) (expr_str target) (expr_str value) ]
  | Ast.If { cond; then_body; else_body } ->
    let header = Printf.sprintf "%s IF %s" (pos_str s.Ast.s_pos) (expr_str cond) in
    let else_lines =
      match else_body with
      | None -> []
      | Some (else_pos, body) -> Printf.sprintf "%s ELSE" (pos_str else_pos) :: indent_block body
    in
    (header :: indent_block then_body) @ else_lines
  | Ast.While { cond; body } ->
    Printf.sprintf "%s WHILE %s" (pos_str s.Ast.s_pos) (expr_str cond) :: indent_block body
  | Ast.For { var; iter; body } ->
    Printf.sprintf "%s FOR %s IN %s" (pos_str s.Ast.s_pos) var (expr_str iter) :: indent_block body
  | Ast.Return None -> [ Printf.sprintf "%s RETURN" (pos_str s.Ast.s_pos) ]
  | Ast.Return (Some e) -> [ Printf.sprintf "%s RETURN %s" (pos_str s.Ast.s_pos) (expr_str e) ]
  | Ast.ExprStmt { Ast.kind = Ast.DbStub toks; _ } ->
    [ Printf.sprintf "%s DB_STUB %s" (pos_str s.Ast.s_pos) (dbstub_tokens_str toks) ]
  | Ast.ExprStmt e -> [ Printf.sprintf "%s EXPR %s" (pos_str s.Ast.s_pos) (expr_str e) ]

(* [header; body statements...] — body lines are already indented two
   spaces; callers nesting this under a class add one more level of
   indent uniformly, same as before Task 5. *)
let dump_method (m : Ast.method_decl) : string list =
  let header = Printf.sprintf "%s METHOD %s" (pos_str m.pos) (sig_str m.name m.params m.ret) in
  let body_lines = List.concat_map (fun s -> List.map (fun l -> "  " ^ l) (dump_stmt s)) m.body in
  header :: body_lines

let annotations_header (is_gc : bool) (table : Ast.table_cfg option) : string =
  let gc_part = if is_gc then " @gc" else "" in
  let table_part =
    match table with
    | None -> ""
    | Some t ->
      let name_part =
        match t.table_name with None -> [] | Some n -> [ Printf.sprintf "name=%S" n ]
      in
      let index_parts =
        List.map (fun cols -> Printf.sprintf "index=[%s]" (String.concat ", " cols)) t.indexes
      in
      let parts = name_part @ index_parts in
      if parts = [] then " @table" else " @table(" ^ String.concat ", " parts ^ ")"
  in
  gc_part ^ table_part

let dump_class (c : Ast.class_decl) : string list =
  let kw = if c.is_class then "CLASS" else "TYPE" in
  let header =
    Printf.sprintf "%s %s %s%s" (pos_str c.pos) kw c.name (annotations_header c.is_gc c.table)
  in
  let field_lines = List.map (fun f -> "  " ^ dump_field f) c.fields in
  let method_lines = List.concat_map (fun m -> List.map (fun l -> "  " ^ l) (dump_method m)) c.methods in
  (header :: field_lines) @ method_lines

let dump_interface (i : Ast.interface_decl) : string list =
  let header = Printf.sprintf "%s INTERFACE %s" (pos_str i.pos) i.name in
  let method_lines = List.map (fun m -> "  " ^ dump_method_sig m) i.methods in
  header :: method_lines

let dump_decl : Ast.decl -> string list = function
  | Ast.Class c -> dump_class c
  | Ast.Interface i -> dump_interface i
  | Ast.Fn f -> dump_method f

let dump_ast (prog : Ast.program) : string =
  let lines = List.concat_map dump_decl prog.decls in
  match lines with [] -> "" | _ -> String.concat "\n" lines ^ "\n"

(* dump_owner — the ownership pass's four emitter tables (Task 7).

   Four fixed sections in a fixed order, each listing its entries in
   source order (LINE:COL first, same convention as every other dump
   here); a section with no entries still prints its header, so the
   format is stable and a golden diff shows an emptied table as a real
   change. Node ids are not printed — the tables carry them for plan 3
   (Owner.move_site.mv_node and friends), but ids churn goldens exactly
   the way dump_ast's module doc describes, so positions are the
   rendering surface.

     == MOVES ==      one line per real ownership transfer
                      "LINE:COL MOVE <place> <how>", where <how> is
                      LET / ASSIGN / RETURN / ARG(param) / CTOR(field).
                      Copy-classed and @gc-classed transfers are absent
                      by design (a register copy and an rc site
                      respectively, not a transfer).

     == DROPS ==      deterministic destruction, plus the frame's drop
                      map at trap-capable sites:
                        SCOPE <label> [..]  owned locals of a scope that
                                            are live where it ends
                        RETURN [..]         owned locals to drop before
                                            this return leaves the frame
                        OVERWRITE <place>   the value an assignment
                                            replaces (absent when the
                                            assignment's target and value
                                            are the same storage, e.g.
                                            `a = a`: dropping there would
                                            destroy what was just stored)
                        JOIN-DROP <label> [..]
                                            join normalization: locals the
                                            *other* branch of an `if` moved
                                            and this one did not, dropped at
                                            the named branch's end so both
                                            paths leave the merge in the one
                                            state the join records. Without
                                            these, a conditionally moved
                                            value would leak on the path
                                            that kept it
                        LIVE-MASK [..]      everything live at a call /
                                            DB_STUB, `:gc` tagging the
                                            entries that need a decrement
                                            rather than a DROP
                      Lists are in destruction order (innermost scope
                      first, reverse declaration order inside a scope).
                      A SCOPE entry is anchored at its *construct's* own
                      position (the `fn`/`if`/`else`/`while`/`for` token):
                      this AST carries no end positions at all (ast.ml's
                      single-point convention), so a SCOPE line can sort
                      ahead of the lines for sites inside that same scope.
                      Label plus construct position is what identifies the
                      block; source order is only here to keep the
                      rendering deterministic.

     == RC ==       "LINE:COL ACQUIRE|RELEASE <place> ELIDED|KEPT" for
                      @gc reference counting. ELIDED marks a pair the
                      emitter may skip because increment and decrement
                      are provably balanced inside one scope.

     == RESIDUAL ==   "LINE:COL RESIDUAL <op> <place> vs <op> <place>" —
                      the sites static proof could not settle, so the
                      emitter wraps them in runtime borrow ops
                      (runtime/src/borrow.c) and the VM traps on a real
                      violation. Each <op> is BORROW_X (an exclusive
                      access: a `mut` argument, a mutating receiver, or an
                      assignment target) or BORROW_S (a live shared
                      borrow); at least one side is always BORROW_X, since
                      two shared readers never conflict. A move is never a
                      residual side — a move names a whole local, and a
                      whole local either provably overlaps another place or
                      is provably disjoint from it. Places are rendered
                      *canonically*: an access written through a borrow
                      binding (`r`, from `let r = bag.items[i]`) prints as
                      the storage it names (`bag.items[i]`), so two
                      bindings into one container read as the aliasing pair
                      they are. The operand node ids in the table
                      (Owner.rs_a_node / rs_b_node) are the precise key.
                      Several entries may name the *same* canonical
                      operand: a reborrow chain produces more than one
                      genuine unprovable pair over one statement (an
                      exclusive access against the pairwise partner, and
                      again against a shared borrow still live through the
                      chain). The emitter MUST therefore coalesce guards
                      **per operand**, not emit one acquire/release pair per
                      table entry — doing the latter asks for both
                      wo_borrow_excl and wo_borrow_shared on the same
                      object and self-traps on legal code, the case where
                      the runtime indices differ after all. *)

let move_kind_str : Owner.move_kind -> string = function
  | Owner.MvLet -> "LET"
  | Owner.MvAssign -> "ASSIGN"
  | Owner.MvReturn -> "RETURN"
  | Owner.MvArg name -> Printf.sprintf "ARG(%s)" name
  | Owner.MvCtorField name -> Printf.sprintf "CTOR(%s)" name

let drop_item_str (i : Owner.drop_item) : string =
  match i.Owner.di_kind with
  | Owner.LOwned -> i.Owner.di_name
  | Owner.LGc -> i.Owner.di_name ^ ":gc"

let drop_items_str (items : Owner.drop_item list) : string =
  "[" ^ String.concat ", " (List.map drop_item_str items) ^ "]"

let acc_kind_str : Owner.acc_kind -> string = function
  | Owner.AShared -> "BORROW_S"
  | Owner.AExcl -> "BORROW_X"
  | Owner.AMove -> "MOVE"

let owner_pos_str (p : Ast.pos) : string = Printf.sprintf "%d:%d" p.Ast.line p.Ast.col

let dump_owner (t : Owner.tables) : string =
  let move_line (m : Owner.move_site) =
    Printf.sprintf "%s MOVE %s %s" (owner_pos_str m.Owner.mv_pos) m.Owner.mv_place
      (move_kind_str m.Owner.mv_kind)
  in
  let drop_line (d : Owner.drop_site) =
    let pos = owner_pos_str d.Owner.dr_pos in
    match d.Owner.dr_kind with
    | Owner.DScope label ->
      Printf.sprintf "%s SCOPE %s %s" pos label (drop_items_str d.Owner.dr_items)
    | Owner.DReturn -> Printf.sprintf "%s RETURN %s" pos (drop_items_str d.Owner.dr_items)
    | Owner.DOverwrite ->
      Printf.sprintf "%s OVERWRITE %s" pos
        (String.concat ", " (List.map (fun (i : Owner.drop_item) -> i.Owner.di_name) d.Owner.dr_items))
    | Owner.DBranchJoin label ->
      Printf.sprintf "%s JOIN-DROP %s %s" pos label (drop_items_str d.Owner.dr_items)
    | Owner.DLiveMask -> Printf.sprintf "%s LIVE-MASK %s" pos (drop_items_str d.Owner.dr_items)
  in
  let rc_line (r : Owner.rc_site) =
    Printf.sprintf "%s %s %s %s" (owner_pos_str r.Owner.rc_pos)
      (match r.Owner.rc_op with Owner.RcAcquire -> "ACQUIRE" | Owner.RcRelease -> "RELEASE")
      r.Owner.rc_place
      (if r.Owner.rc_elided then "ELIDED" else "KEPT")
  in
  let res_line (r : Owner.residual_site) =
    Printf.sprintf "%s RESIDUAL %s %s vs %s %s" (owner_pos_str r.Owner.rs_pos)
      (acc_kind_str r.Owner.rs_a_kind) r.Owner.rs_a (acc_kind_str r.Owner.rs_b_kind) r.Owner.rs_b
  in
  let section header lines = (header :: lines) in
  let lines =
    section "== MOVES ==" (List.map move_line t.Owner.moves)
    @ section "== DROPS ==" (List.map drop_line t.Owner.drops)
    @ section "== RC ==" (List.map rc_line t.Owner.rcs)
    @ section "== RESIDUAL ==" (List.map res_line t.Owner.residuals)
  in
  String.concat "\n" lines ^ "\n"
