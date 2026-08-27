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

(* iteration 37, the raw text literal (backtick-delimited, verbatim
   content, no backslash escapes). Two codes, because the two shapes
   are genuinely different situations:

   WO-E004 — a raw literal that runs off the end of the file. Unlike a
   plain "..." string (silent, rt parity, see above), this one IS
   reported: multi-line is the raw literal's normal case, so a missing
   closing backtick would otherwise swallow every remaining line of the
   file with nothing to show for it. Reported at the OPENING backtick,
   which is the only position that helps -- EOF tells the reader
   nothing about which literal never closed.

   WO-E005 — a raw newline inside a "..." or '...' string. This used to
   be accepted silently: the string scanner's catch-all appended the
   newline like any other byte, so a forgotten closing quote ate the
   rest of the file with no diagnostic at all. Nothing in the repo ever
   relied on it (zero of the .wo sources span a line inside quotes) and
   the backtick literal is now the spelling for multi-line text, so the
   accident becomes an error. The scan stops at the newline WITHOUT
   consuming it, so the Newline token is still emitted and the
   statement terminates -- one diagnostic, and the next line parses
   normally instead of being swallowed. The rt-parity silence for a
   plain unterminated string with no newline is untouched. *)
let unterminated_raw_code = Diag.lexing_prefix ^ "04" (* WO-E004 *)
let newline_in_string_code = Diag.lexing_prefix ^ "05" (* WO-E005 *)

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
  | "not" -> Some Token.KwNot
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

(* ---- the raw literal's margin rule (iteration 37) -------------------

   A render() body is written at its method's indentation, but that
   indentation is an artifact of the SOURCE, not of the markup -- nobody
   wants six leading spaces on every line of the served HTML. So the
   common margin is removed here, at lex time: the constant pool holds
   the dedented text, no downstream stage ever sees the source
   indentation, and the whole rule costs nothing at run time.

   The rule (Java's text blocks, which solved exactly this):
     - one newline immediately after the opening backtick is dropped,
       so the first markup line can start on its own line;
     - the smallest leading run of spaces/tabs across all non-blank
       lines is removed from every line (characters counted, tabs NOT
       expanded -- mixing them is the author's problem, and expanding
       would need a tab width the language does not have);
     - a whitespace-only final line (the usual case: the closing
       backtick sits on its own line) loses its whitespace but keeps
       its newline.
   A literal with no newline in it is left completely alone -- there is
   no margin to speak of, and silently eating the leading spaces of
   `  hi` would be a surprise, not a service.

   Holes do not disturb any of this. A line's indentation is by
   definition the run of whitespace at its start, and the only thing
   that can split a line across segments is a hole, which ends that run
   -- so an indentation run always lives whole inside one SText. The
   measuring pass replaces each hole with a single non-whitespace
   sentinel byte so that a line that is `    {{ x }}` correctly counts
   as indent 4 and as NON-blank. *)

let is_indent_char c = c = ' ' || c = '\t'

let segments_shadow (segs : Token.str_part list) : string =
  let b = Buffer.create 64 in
  List.iter
    (function
      | Token.SText s -> Buffer.add_string b s
      | Token.SExpr _ | Token.SEsc _ -> Buffer.add_char b '\001')
    segs;
  Buffer.contents b

let min_indent (shadow : string) : int =
  let m = ref max_int in
  List.iter
    (fun line ->
      let n = String.length line in
      let i = ref 0 in
      while !i < n && is_indent_char line.[!i] do
        incr i
      done;
      (* a blank (or whitespace-only) line never sets the margin *)
      if !i < n && !i < !m then m := !i)
    (String.split_on_char '\n' shadow);
  if !m = max_int then 0 else !m

let strip_margin (k : int) (segs : Token.str_part list) : Token.str_part list =
  if k = 0 then segs
  else begin
    let at_line_start = ref true in
    let one seg =
      match seg with
      | Token.SExpr _ | Token.SEsc _ ->
        at_line_start := false;
        seg
      | Token.SText s ->
        let n = String.length s in
        let b = Buffer.create n in
        let i = ref 0 in
        while !i < n do
          if !at_line_start then begin
            let dropped = ref 0 in
            while !dropped < k && !i < n && is_indent_char s.[!i] do
              incr dropped;
              incr i
            done;
            at_line_start := false
          end
          else begin
            let c = s.[!i] in
            Buffer.add_char b c;
            if c = '\n' then at_line_start := true;
            incr i
          end
        done;
        Token.SText (Buffer.contents b)
    in
    (* fold_left, not List.map: `one` carries state across segments and
       List.map's application order is unspecified. *)
    List.rev (List.fold_left (fun acc seg -> one seg :: acc) [] segs)
  end

let drop_trailing_margin (segs : Token.str_part list) : Token.str_part list =
  match List.rev segs with
  | Token.SText s :: rest_rev ->
    let n = String.length s in
    let i = ref n in
    while !i > 0 && is_indent_char s.[!i - 1] do
      decr i
    done;
    (* only a run that directly follows a newline is a closing line *)
    if !i < n && !i > 0 && s.[!i - 1] = '\n' then
      List.rev (Token.SText (String.sub s 0 !i) :: rest_rev)
    else segs
  | _ -> segs

let dedent (segs : Token.str_part list) : Token.str_part list =
  let shadow = segments_shadow segs in
  if not (String.contains shadow '\n') then segs
  else begin
    let segs =
      match segs with
      | Token.SText s :: rest when String.length s > 0 && s.[0] = '\n' ->
        Token.SText (String.sub s 1 (String.length s - 1)) :: rest
      | _ -> segs
    in
    let k = min_indent (segments_shadow segs) in
    drop_trailing_margin (strip_margin k segs)
  end

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
  (* A line ending in `..` continues on the next line — the ONE newline
     suppression in the language, so multi-line markup/text builds read
     as one expression (the shop template's ask; story 37 rides it). *)
  let last_is_dotdot () =
    match !out with
    | { Token.kind = Token.DotDot; _ } :: _ -> true
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
  let report_unterminated_raw line col =
    Diag.Collector.add collector
      (Diag.error ~code:unterminated_raw_code ~file ~line ~col
         ~message:"unterminated raw text literal" ())
  in
  let report_newline_in_string line col =
    Diag.Collector.add collector
      (Diag.error ~code:newline_in_string_code ~file ~line ~col
         ~message:
           "newline in string literal (use a `...` raw text literal for \
            multi-line text)" ())
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
        if not (last_is_newline ()) && not (last_is_dotdot ()) then
          emit Token.Newline line col
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
          | Some '\n' ->
            (* WO-E005. Deliberately NOT consumed: the outer loop turns
               it into the Newline token that terminates the statement,
               so recovery is one bad line rather than the rest of the
               file. *)
            report_newline_in_string lx.line lx.col;
            scanning := false
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
      else if c = '`' then begin
        (* iteration 37: the raw text literal. Everything up to the
           closing backtick is content -- newlines included, and with NO
           escape processing at all, which is the whole point: markup
           carries quotes and backslashes verbatim. A literal backtick
           (or a literal `{{`) is written by concatenating an ordinary
           "..." string with `..`; that door is one greppable operator,
           which beats inventing an escape character for the one form
           whose selling point is not having any.

           Two hole forms, and ONLY here -- inside "..." a `{{` is still
           two literal braces, so existing CSS/JS text is untouched:
             ${ expr }   raw, exactly like a "..." string's hole
             {{ expr }}  HTML-escaped (the parser wraps it in esc()) *)
        ignore (advance lx);
        let buf = Buffer.create 64 in
        let parts = ref [] in
        let flush_text () =
          parts := Token.SText (Buffer.contents buf) :: !parts;
          Buffer.clear buf
        in
        let scanning = ref true in
        while !scanning do
          match peek lx with
          | None ->
            report_unterminated_raw line col;
            scanning := false
          | Some '`' ->
            ignore (advance lx);
            scanning := false
          | Some '$' when peek_at lx 1 = Some '{' ->
            flush_text ();
            ignore (advance lx);
            ignore (advance lx);
            parts := Token.SExpr (read_interp_expr lx) :: !parts
          | Some '{' when peek_at lx 1 = Some '{' ->
            flush_text ();
            ignore (advance lx);
            ignore (advance lx);
            (* read_interp_expr stops at the first `}` at depth 0 and
               consumes it -- the second one closes this hole. Reusing it
               means brace depth and nested string literals are already
               handled, so `{{ Point{x:1}.x }}` scans correctly. *)
            let raw = read_interp_expr lx in
            (match peek lx with
            | Some '}' -> ignore (advance lx)
            | _ -> report_unterminated_raw line col);
            parts := Token.SEsc raw :: !parts
          | Some other ->
            ignore (advance lx);
            Buffer.add_char buf other
        done;
        flush_text ();
        (match dedent (List.rev !parts) with
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
        (* iteration 36: hex (`0x`) and binary (`0b`) Int literals, and `_`
           digit separators in every integer form. The prefix commits only
           when the character AFTER it is a real digit of that base, so `0x`
           followed by anything else stays Int 0 + Ident — a parse error at
           its own position, no new lexer diagnostic. Accumulation uses
           OCaml's native int (63-bit): a full-width 64-bit literal like
           0xFFFFFFFFFFFFFFFF is out of reach — all-ones is spelled -1. The
           float path below is untouched: neither prefix can reach it (a
           fraction/exponent needs the decimal branch), and `_` is consumed
           only between digits of an integer run. *)
        let is_hex_digit ch =
          is_digit ch || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F')
        in
        let hex_val ch =
          if is_digit ch then Char.code ch - Char.code '0'
          else if ch >= 'a' && ch <= 'f' then Char.code ch - Char.code 'a' + 10
          else Char.code ch - Char.code 'A' + 10
        in
        let scan_prefixed base is_base_digit digit_val =
          (* consumes the peeked '0' and the prefix char, then the run *)
          ignore (advance lx);
          ignore (advance lx);
          let n = ref 0 in
          let scanning = ref true in
          while !scanning do
            match peek lx with
            | Some d when is_base_digit d ->
              n := (!n * base) + digit_val d;
              ignore (advance lx)
            | Some '_' when (match peek_at lx 1 with
                             | Some d -> is_base_digit d
                             | None -> false) ->
              ignore (advance lx)
            | _ -> scanning := false
          done;
          emit (Token.Int !n) line col
        in
        match (c, peek_at lx 1, peek_at lx 2) with
        | '0', Some ('x' | 'X'), Some d when is_hex_digit d ->
          scan_prefixed 16 is_hex_digit hex_val
        | '0', Some ('b' | 'B'), Some ('0' | '1') ->
          scan_prefixed 2 (fun ch -> ch = '0' || ch = '1') (fun ch -> Char.code ch - Char.code '0')
        | _ ->
        let buf = Buffer.create 16 in
        let n = ref 0 in
        let scanning = ref true in
        while !scanning do
          match peek lx with
          | Some d when is_digit d ->
            n := (!n * 10) + (Char.code d - Char.code '0');
            Buffer.add_char buf d;
            ignore (advance lx)
          | Some '_' when (match peek_at lx 1 with
                           | Some d -> is_digit d
                           | None -> false) ->
            (* separator only BETWEEN digits: `1_` stops the run and the
               `_` lexes as its own ident, a parse error at its position *)
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
        | '*' -> (
          ignore (advance lx);
          match peek lx with
          | Some '=' ->
            ignore (advance lx);
            emit Token.StarEq line col
          | _ -> emit Token.Star line col)
        | '/' -> (
          ignore (advance lx);
          match peek lx with
          | Some '=' ->
            ignore (advance lx);
            emit Token.SlashEq line col
          | _ -> emit Token.Slash line col)
        | '%' -> (
          ignore (advance lx);
          match peek lx with
          | Some '=' ->
            ignore (advance lx);
            emit Token.PercentEq line col
          | _ -> emit Token.Percent line col)
        (* iteration 36: the bitwise operators. `&` and `^` were unknown
           characters before this; `|` (Pipe, above) is reused in expression
           position by the parser. No `&=`/`^=`/`<<=`/`>>=` — bitwise
           compound assigns are out of scope per the story. *)
        | '&' ->
          ignore (advance lx);
          emit Token.Amp line col
        | '^' ->
          ignore (advance lx);
          emit Token.Caret line col
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
          | Some '<' ->
            ignore (advance lx);
            emit Token.Shl line col
          | _ -> emit Token.Lt line col)
        | '>' -> (
          ignore (advance lx);
          match peek lx with
          | Some '=' ->
            ignore (advance lx);
            emit Token.GtEq line col
          | Some '>' ->
            ignore (advance lx);
            emit Token.Shr line col
          | _ -> emit Token.Gt line col)
        | other ->
          ignore (advance lx);
          report_unknown line col other)
  done;
  emit Token.Eof lx.line lx.col;
  preprocess collector ~file (List.rev !out)
