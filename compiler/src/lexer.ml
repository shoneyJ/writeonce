(* lexer.ml — tokenizer for `.wo` source (milestone-1 OOP subset).

   Ported from crates/rt/src/lexer.rs; behavior kept identical wherever
   it defines what the language literally is:

   - newline tokens are emitted, never filtered, and consecutive
     newlines collapse to a single Newline token — exactly rt's
     `out.last() == Some(Newline) => don't push another` rule;
   - `--` starts a line comment that runs to (not including) the next
     newline;
   - single- or double-quoted strings support the same backslash
     escapes as rt: n, t, backslash, or either quote character, each
     backslash-prefixed; anything else verbatim. `\r` and `\0` were added
     2026-08-14 (a program writing HTTP needs CRLF, and rt never had to);
   - integer literals are plain runs of ASCII digits;
   - identifiers may contain internal dashes, exactly like rt's
     read_ident_chars (crates/rt/src/lexer.rs) — so `foo-bar` lexes as
     one identifier, not `foo`, Dash, `bar`. This is a faithful port of
     an rt quirk, not a milestone-1 design choice: Task 5's expression
     parser must therefore require whitespace around a binary minus
     that immediately follows an identifier (`a - b`, not `a-b`) to
     avoid the ambiguity, exactly as rt's schema layer already does.
     Flagged here for whoever picks up Task 5.

   Divergence from rt, deliberate: rt's `tokenize` returns
   `anyhow::Result` and bails (stops the whole file) on the first bad
   byte or malformed punctuation shape (a bare `!` not followed by
   `=`, a `$` with no name, ...). This front end's diagnostics contract
   is multi-error (compiler/src/diag.ml — every later stage accumulates
   into one Collector rather than stopping at the first problem), so
   every one of those cases becomes: report one WO-E001 unknown-
   character diagnostic at the offending position, skip exactly that
   one byte, and keep lexing. One bad byte must never stop the whole
   file (Task 3 brief's Unknown character decision). *)

let unknown_char_code = Diag.lexing_prefix ^ "01" (* WO-E001 *)

(* rt's equivalent site (crates/rt/src/lexer.rs:106) bails outright: a
   backslash as the very last byte of the file, with no character left
   to escape, loses information silently otherwise (the intended escaped
   character is simply gone, not skip-and-continue like the unknown-
   character case). Ported as its own code rather than reusing WO-E001
   because the shape is different (a truncated escape, not a byte the
   lexer doesn't recognize at all) and diag.ml's convention is one code
   per distinct lexing situation.

   Deliberately NOT applied to a plain unterminated string: a string
   that runs off the end of the file with no dangling backslash (say,
   a quote-open let-binding with nothing after it and no closing quote)
   — rt does not bail there either, its scanning loop just stops when
   peek returns None and emits whatever was collected as the Str token.
   Reporting nothing in that case is intentional rt parity, not an
   oversight; pinned by the plain-unterminated-string-reports-nothing
   assertion in compiler/test/runner.ml. *)
let unterminated_escape_code = Diag.lexing_prefix ^ "02" (* WO-E002 *)
let directive_code = Diag.lexing_prefix ^ "03" (* WO-E003: #if/#else/#end misuse *)

(* haxe-parity Task 8: build flags. `woc -D name` fills this before any
   tokenize call; undefined flags are false. A module-level ref because the
   compiler is a single-shot process — tests that care set it explicitly
   and reset to empty. *)
module StringSet = Set.Make (String)

let defines : StringSet.t ref = ref StringSet.empty

type lexer = {
  src : string;
  len : int;
  mutable pos : int;
  mutable line : int;
  mutable col : int;
}

let make src = { src; len = String.length src; pos = 0; line = 1; col = 1 }
let peek lx = if lx.pos < lx.len then Some lx.src.[lx.pos] else None

let peek_at lx n =
  let i = lx.pos + n in
  if i < lx.len then Some lx.src.[i] else None

let advance lx =
  match peek lx with
  | None -> None
  | Some c ->
    lx.pos <- lx.pos + 1;
    if c = '\n' then begin
      lx.line <- lx.line + 1;
      lx.col <- 1
    end
    else lx.col <- lx.col + 1;
    Some c

let is_digit c = c >= '0' && c <= '9'
let is_alpha c = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
let is_ident_start c = is_alpha c || c = '_'
let is_ident_cont c = is_alpha c || is_digit c || c = '_' || c = '-'

(* Consumes a run of identifier characters starting at the lexer's
   current position (caller has already confirmed is_ident_start on
   the character at that position) and returns the collected text.
   Mirrors rt's read_ident_chars, dash-continuation included (see
   module doc). *)
let read_ident_chars lx =
  let start = lx.pos in
  let continue_ = ref true in
  while !continue_ do
    match peek lx with
    | Some c when is_ident_cont c -> ignore (advance lx)
    | _ -> continue_ := false
  done;
  String.sub lx.src start (lx.pos - start)

(* The milestone-1 keyword set, exact per the Task 3 brief: type class
   interface fn let mut take return if else while for in true false,
   plus uppercase-only INSERT/SELECT. Deliberately absent: self, me,
   subscribe, receive, and lowercase insert/select — those fall
   through to the `_ -> None` case below and lex as plain Ident,
   matching rt and the CLAUDE.md gotcha this task exists to preserve.
   `use`/`pub` (haxe-parity Task 1, modules) added on top of that set. *)
let keyword_kind = function
  | "type" -> Some Token.KwType
  | "class" -> Some Token.KwClass
  | "interface" -> Some Token.KwInterface
  | "fn" -> Some Token.KwFn
  | "let" -> Some Token.KwLet
  | "mut" -> Some Token.KwMut
  | "take" -> Some Token.KwTake
  | "return" -> Some Token.KwReturn
  | "if" -> Some Token.KwIf
  | "else" -> Some Token.KwElse
  | "while" -> Some Token.KwWhile
  | "for" -> Some Token.KwFor
  | "in" -> Some Token.KwIn
  | "true" -> Some Token.KwTrue
  | "false" -> Some Token.KwFalse
  | "use" -> Some Token.KwUse
  | "spawn" -> Some Token.KwSpawn
  | "using" -> Some Token.KwUsing
  | "pub" -> Some Token.KwPub
  | "break" -> Some Token.KwBreak
  | "continue" -> Some Token.KwContinue
  | "do" -> Some Token.KwDo
  | "const" -> Some Token.KwConst
  | "and" -> Some Token.KwAnd
  | "or" -> Some Token.KwOr
  | "inline" -> Some Token.KwInline
  | "switch" -> Some Token.KwSwitch
  | "case" -> Some Token.KwCase
  | "default" -> Some Token.KwDefault
  | "typedef" -> Some Token.KwTypedef
  | "try" -> Some Token.KwTry
  | "catch" -> Some Token.KwCatch
  | "nil" -> Some Token.KwNil
  | "as" -> Some Token.KwAs
  | "INSERT" -> Some Token.KwInsert
  | "SELECT" -> Some Token.KwSelect
  | _ -> None

(* haxe-parity Task 2: scans the raw source of one `${...}` interpolation
   body, starting right after the `{` (caller already consumed `$` and
   `{`). Returns that raw, unlexed text -- the parser re-tokenizes it as a
   full expression (parser.ml's own desugar-to-Concat step; this is a
   mechanical extraction only, no semantics). Tracks brace depth so a
   nested `{}` (a constructor literal inside an interpolation,
   `${Point{x:1}.x}`) doesn't end the scan early, and skips a nested
   string literal verbatim (honoring its own backslash escapes) so a
   quote or brace *inside* that nested string can't confuse either
   count. Runs off the end of the file the same silent way an
   unterminated outer string does -- the caller's own EOF handling picks
   up right after. *)
let read_interp_expr lx =
  let buf = Buffer.create 16 in
  let depth = ref 0 in
  let continue_ = ref true in
  while !continue_ do
    match peek lx with
    | None -> continue_ := false
    | Some '}' when !depth = 0 ->
      ignore (advance lx);
      continue_ := false
    | Some ('{' as c) ->
      incr depth;
      Buffer.add_char buf c;
      ignore (advance lx)
    | Some ('}' as c) ->
      decr depth;
      Buffer.add_char buf c;
      ignore (advance lx)
    | Some (('"' | '\'') as q) ->
      Buffer.add_char buf q;
      ignore (advance lx);
      let scanning = ref true in
      while !scanning do
        match peek lx with
        | None -> scanning := false
        | Some c when c = q ->
          Buffer.add_char buf c;
          ignore (advance lx);
          scanning := false
        | Some '\\' -> (
          Buffer.add_char buf '\\';
          ignore (advance lx);
          match peek lx with
          | Some c ->
            Buffer.add_char buf c;
            ignore (advance lx)
          | None -> scanning := false)
        | Some c ->
          Buffer.add_char buf c;
          ignore (advance lx)
      done
    | Some c ->
      Buffer.add_char buf c;
      ignore (advance lx)
  done;
  Buffer.contents buf

(* haxe-parity Task 8: the #if filter, run over the in-order token list at
   the end of tokenize. A `#if <flag>` section is kept when the flag is
   defined AND every enclosing section is kept; `#else` flips the section;
   `#end` closes it. Nesting allowed; flag NAMES only (no expression
   language — the spec's limit); undefined flags are false. Misuse is
   WO-E003: a #if without a flag name, a second #else, a stray #else/#end,
   or a #if left open at end of file. Eof always survives so the parser
   still terminates after a reported error. *)
let preprocess (collector : Diag.Collector.t) ~(file : string)
    (toks : Token.t list) : Token.t list =
  let err line col msg =
    Diag.Collector.add collector
      (Diag.error ~code:directive_code ~file ~line ~col ~message:msg ())
  in
  (* frame: (emitting, seen_else, opening line, opening col) *)
  let stack : (bool * bool * int * int) list ref = ref [] in
  let emitting () = List.for_all (fun (e, _, _, _) -> e) !stack in
  let out = ref [] in
  let rec go = function
    | [] -> (
      match !stack with
      | (_, _, l, c) :: _ -> err l c "#if left open — missing #end"
      | [] -> ())
    | { Token.kind = Token.HashIf; line; col } :: rest -> (
      match rest with
      | { Token.kind = Token.Ident flag; _ } :: rest2 ->
        stack := (StringSet.mem flag !defines, false, line, col) :: !stack;
        go rest2
      | _ ->
        err line col "#if needs a flag name (`#if portable`)";
        stack := (false, false, line, col) :: !stack;
        go rest)
    | { Token.kind = Token.HashElse; line; col } :: rest ->
      (match !stack with
       | (e, false, l, c) :: tl -> stack := (not e, true, l, c) :: tl
       | (_, true, _, _) :: _ -> err line col "second #else in one #if section"
       | [] -> err line col "#else outside any #if");
      go rest
    | { Token.kind = Token.HashEnd; line; col } :: rest ->
      (match !stack with
       | _ :: tl -> stack := tl
       | [] -> err line col "#end outside any #if");
      go rest
    | ({ Token.kind = Token.Eof; _ } as t) :: rest ->
      out := t :: !out;
      go rest
    | t :: rest ->
      if emitting () then out := t :: !out;
      go rest
  in
  go toks;
  List.rev !out

let tokenize (collector : Diag.Collector.t) ~(file : string) (src : string) :
    Token.t list =
  let lx = make src in
  let out = ref [] in
  let emit kind line col = out := { Token.kind; line; col } :: !out in
  let last_is_newline () =
    match !out with
    | { Token.kind = Token.Newline; _ } :: _ -> true
    | _ -> false
  in
  let report_unknown line col c =
    Diag.Collector.add collector
      (Diag.error ~code:unknown_char_code ~file ~line ~col
         ~message:(Printf.sprintf "unknown character '%c'" c) ())
  in
  let report_unterminated_escape line col =
    Diag.Collector.add collector
      (Diag.error ~code:unterminated_escape_code ~file ~line ~col
         ~message:"unterminated string escape" ())
  in
  let running = ref true in
  while !running do
    match peek lx with
    | None -> running := false
    | Some c -> (
      let line = lx.line and col = lx.col in
      if c = '-' && peek_at lx 1 = Some '-' then begin
        (* line comment: -- ... EOL (EOL itself is left for the next
           iteration to turn into its own Newline token). *)
        let scanning = ref true in
        while !scanning do
          match peek lx with
          | Some '\n' | None -> scanning := false
          | Some _ -> ignore (advance lx)
        done
      end
      else if c = '\n' then begin
        ignore (advance lx);
        if not (last_is_newline ()) then emit Token.Newline line col
      end
      else if c = ' ' || c = '\t' || c = '\r' then ignore (advance lx)
      else if c = '"' || c = '\'' then begin
        let quote = c in
        ignore (advance lx);
        let buf = Buffer.create 16 in
        (* haxe-parity Task 2: segments accumulate here only when at
           least one `${...}` is actually found (flush_text below); a
           plain string never touches `parts` at all, so it emits the
           exact same `Token.Str` it always did -- see the `match !parts`
           dispatch after the loop. *)
        let parts = ref [] in
        let flush_text () =
          parts := Token.SText (Buffer.contents buf) :: !parts;
          Buffer.clear buf
        in
        let scanning = ref true in
        while !scanning do
          match peek lx with
          | None ->
            (* Plain unterminated string (ran off the end of the file
               with no closing quote and no dangling backslash): rt does
               not bail here either, it just stops and emits whatever was
               collected. No diagnostic, deliberately — see
               unterminated_escape_code's doc comment above. *)
            scanning := false
          | Some c when c = quote ->
            ignore (advance lx);
            scanning := false
          | Some '$' when peek_at lx 1 = Some '{' ->
            (* Unescaped `${` -- `\$` never reaches here, it is fully
               consumed by the backslash branch below, one dispatch
               earlier, so this is always a genuine interpolation start,
               never an escaped `$` that happens to be followed by `{`. *)
            flush_text ();
            ignore (advance lx);
            (* '$' *)
            ignore (advance lx);
            (* '{' *)
            parts := Token.SExpr (read_interp_expr lx) :: !parts
          | Some '\\' -> (
            (* Captured before advancing: this is the backslash's own
               position, so a dangling-escape diagnostic points at the
               `\` itself rather than wherever the scan happens to stop. *)
            let esc_line = lx.line and esc_col = lx.col in
            ignore (advance lx);
            match advance lx with
            | Some 'n' -> Buffer.add_char buf '\n'
            | Some 't' -> Buffer.add_char buf '\t'
            (* `\r` — added 2026-08-14: without it a program cannot write CRLF
               at all, and the driving workload's HTTP server needs it (its
               `index_of(buf, "\r\n\r\n")` was searching for a literal
               backslash-r, so it never found a header terminator). *)
            | Some 'r' -> Buffer.add_char buf '\r'
            | Some '0' -> Buffer.add_char buf '\000'
            | Some '\\' -> Buffer.add_char buf '\\'
            | Some '"' -> Buffer.add_char buf '"'
            | Some '\'' -> Buffer.add_char buf '\''
            (* `\$` -- not one of the escapes above, so it falls into
               this catch-all exactly like any other unrecognized
               backslash sequence, producing a literal `$` that the `$`
               dispatch above never sees (it already advanced past it). *)
            | Some other -> Buffer.add_char buf other
            | None ->
              report_unterminated_escape esc_line esc_col;
              scanning := false)
          | Some other ->
            ignore (advance lx);
            Buffer.add_char buf other
        done;
        flush_text ();
        (match List.rev !parts with
        | [] -> emit (Token.Str "") line col
        | [ Token.SText s ] -> emit (Token.Str s) line col
        | segs -> emit (Token.InterpStr segs) line col)
      end
      else if is_digit c then begin
        (* iteration 19: one scanner for both numeric worlds. The integer run
           is scanned into a buffer as well as accumulated, because a fraction
           or an exponent turns the whole thing into a Float and OCaml's
           float_of_string wants the original text.

           A digit run stays an Int unless it is followed by:
             - '.' AND a digit  -> `1.5`. The digit requirement is what keeps
               `0..10` a range (Dot Dot after Int 0) and leaves any future
               `1.method()` reachable; without it `0..10` would lex as
               Float 0. followed by `.10`.
             - 'e'/'E' with an optional sign AND a digit -> `2e10`. Checked
               before consuming, so `2eggs` is still Int 2 then Ident. *)
        (* `c` is PEEKED, not consumed — the loop below reads it. Adding it to
           the buffer here as well would count the first digit twice. *)
        let buf = Buffer.create 16 in
        let n = ref 0 in
        let scanning = ref true in
        while !scanning do
          match peek lx with
          | Some d when is_digit d ->
            n := (!n * 10) + (Char.code d - Char.code '0');
            Buffer.add_char buf d;
            ignore (advance lx)
          | _ -> scanning := false
        done;
        let is_float = ref false in
        (match (peek lx, peek_at lx 1) with
        | Some '.', Some d when is_digit d ->
          is_float := true;
          Buffer.add_char buf '.';
          ignore (advance lx);
          let frac = ref true in
          while !frac do
            match peek lx with
            | Some d when is_digit d ->
              Buffer.add_char buf d;
              ignore (advance lx)
            | _ -> frac := false
          done
        | _ -> ());
        (* exponent, on an integer run (`2e10`) or after a fraction (`1.5e-3`) *)
        (match (peek lx, peek_at lx 1, peek_at lx 2) with
        | Some ('e' | 'E'), Some d, _ when is_digit d -> is_float := true
        | Some ('e' | 'E'), Some ('+' | '-'), Some d when is_digit d -> is_float := true
        | _ -> ());
        if !is_float then begin
          (match peek lx with
          | Some (('e' | 'E') as e) ->
            Buffer.add_char buf e;
            ignore (advance lx);
            (match peek lx with
            | Some (('+' | '-') as s) ->
              Buffer.add_char buf s;
              ignore (advance lx)
            | _ -> ());
            let ex = ref true in
            while !ex do
              match peek lx with
              | Some d when is_digit d ->
                Buffer.add_char buf d;
                ignore (advance lx)
              | _ -> ex := false
            done
          | _ -> ());
          (* float_of_string cannot fail here: the buffer is a well-formed
             decimal by construction. Overflow is not an error either — it
             yields infinity, which is a legitimate Float per IEEE quiet
             semantics (`1e400` is `inf`, not a compile error). *)
          emit (Token.Float (float_of_string (Buffer.contents buf))) line col
        end
        else emit (Token.Int !n) line col
      end
      else if c = '#' then begin
        (* haxe-parity Task 8: `#if` / `#else` / `#end` build-flag
           directives. Names only — anything else after '#' is WO-E003. *)
        ignore (advance lx);
        let name =
          match peek lx with
          | Some d when is_ident_start d -> read_ident_chars lx
          | _ -> ""
        in
        match name with
        | "if" -> emit Token.HashIf line col
        | "else" -> emit Token.HashElse line col
        | "end" -> emit Token.HashEnd line col
        | other ->
          Diag.Collector.add collector
            (Diag.error ~code:directive_code ~file ~line ~col
               ~message:
                 (Printf.sprintf
                    "unknown directive `#%s` — the build-flag directives are #if <flag>, #else, #end"
                    other)
               ())
      end
      else if is_ident_start c then begin
        let name = read_ident_chars lx in
        let kind =
          match keyword_kind name with Some k -> k | None -> Token.Ident name
        in
        emit kind line col
      end
      else
        match c with
        | '{' ->
          ignore (advance lx);
          emit Token.LBrace line col
        | '}' ->
          ignore (advance lx);
          emit Token.RBrace line col
        | '(' ->
          ignore (advance lx);
          emit Token.LParen line col
        | ')' ->
          ignore (advance lx);
          emit Token.RParen line col
        | '[' ->
          ignore (advance lx);
          emit Token.LBracket line col
        | ']' ->
          ignore (advance lx);
          emit Token.RBracket line col
        | ',' ->
          ignore (advance lx);
          emit Token.Comma line col
        | ';' ->
          ignore (advance lx);
          emit Token.Semicolon line col
        | ':' ->
          ignore (advance lx);
          emit Token.Colon line col
        | '.' -> (
          ignore (advance lx);
          match peek lx with
          | Some '.' ->
            ignore (advance lx);
            emit Token.DotDot line col
          | _ -> emit Token.Dot line col)
        | '?' ->
          ignore (advance lx);
          emit Token.Question line col
        | '@' ->
          ignore (advance lx);
          emit Token.At line col
        | '|' ->
          ignore (advance lx);
          emit Token.Pipe line col
        | '-' -> (
          ignore (advance lx);
          match peek lx with
          | Some '>' ->
            ignore (advance lx);
            emit Token.Arrow line col
          | Some '=' ->
            ignore (advance lx);
            emit Token.MinusEq line col
          | _ -> emit Token.Dash line col)
        | '+' -> (
          ignore (advance lx);
          match peek lx with
          | Some '=' ->
            ignore (advance lx);
            emit Token.PlusEq line col
          | _ -> emit Token.Plus line col)
        | '*' ->
          ignore (advance lx);
          emit Token.Star line col
        | '/' ->
          ignore (advance lx);
          emit Token.Slash line col
        | '%' ->
          ignore (advance lx);
          emit Token.Percent line col
        | '=' -> (
          ignore (advance lx);
          match peek lx with
          | Some '=' ->
            ignore (advance lx);
            emit Token.EqEq line col
          | Some '>' ->
            ignore (advance lx);
            emit Token.FatArrow line col
          | _ -> emit Token.Eq line col)
        | '!' -> (
          ignore (advance lx);
          match peek lx with
          | Some '=' ->
            ignore (advance lx);
            emit Token.NotEq line col
          | _ -> report_unknown line col '!')
        | '<' -> (
          ignore (advance lx);
          match peek lx with
          | Some '=' ->
            ignore (advance lx);
            emit Token.LtEq line col
          | _ -> emit Token.Lt line col)
        | '>' -> (
          ignore (advance lx);
          match peek lx with
          | Some '=' ->
            ignore (advance lx);
            emit Token.GtEq line col
          | _ -> emit Token.Gt line col)
        | other ->
          ignore (advance lx);
          report_unknown line col other)
  done;
  emit Token.Eof lx.line lx.col;
  preprocess collector ~file (List.rev !out)
