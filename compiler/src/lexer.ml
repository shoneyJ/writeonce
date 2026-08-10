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
     backslash-prefixed; anything else verbatim;
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
   matching rt and the CLAUDE.md gotcha this task exists to preserve. *)
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
  | "INSERT" -> Some Token.KwInsert
  | "SELECT" -> Some Token.KwSelect
  | _ -> None

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
          | Some '\\' -> (
            (* Captured before advancing: this is the backslash's own
               position, so a dangling-escape diagnostic points at the
               `\` itself rather than wherever the scan happens to stop. *)
            let esc_line = lx.line and esc_col = lx.col in
            ignore (advance lx);
            match advance lx with
            | Some 'n' -> Buffer.add_char buf '\n'
            | Some 't' -> Buffer.add_char buf '\t'
            | Some '\\' -> Buffer.add_char buf '\\'
            | Some '"' -> Buffer.add_char buf '"'
            | Some '\'' -> Buffer.add_char buf '\''
            | Some other -> Buffer.add_char buf other
            | None ->
              report_unterminated_escape esc_line esc_col;
              scanning := false)
          | Some other ->
            ignore (advance lx);
            Buffer.add_char buf other
        done;
        emit (Token.Str (Buffer.contents buf)) line col
      end
      else if is_digit c then begin
        let n = ref 0 in
        let scanning = ref true in
        while !scanning do
          match peek lx with
          | Some d when is_digit d ->
            n := (!n * 10) + (Char.code d - Char.code '0');
            ignore (advance lx)
          | _ -> scanning := false
        done;
        emit (Token.Int !n) line col
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
  List.rev !out
