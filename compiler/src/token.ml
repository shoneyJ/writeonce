(* token.ml — token kinds for the woc lexer.

   Ported from the Rust runtime's lexer/token pair
   (crates/rt/src/token.rs, crates/rt/src/lexer.rs), trimmed to the
   milestone-1 OOP subset this compiler front targets (Task 3 of
   compiler/plan/2026-08-01-woc-compiler-front.md). Kept from rt:
   position-tracked tokens, the same literal forms (Ident/Int/Str),
   newline-as-a-real-token, and a punctuation/operator set mirroring
   rt's generic categories (braces, parens, brackets, comma, colon,
   dot, arrow, assignment, arithmetic, comparison).

   Deliberately dropped relative to rt's token.rs: the schema/query
   layer keyword zoo (ref, multi, via, policy, txn, BEGIN/COMMIT/...),
   the `$name` parameter token, and the `#name` / `##name` block-marker
   tokens — none of those belong to the milestone-1 OOP language this
   front end parses (interface/class/fn declarations and bodies), only
   to rt's schema DSL. INSERT/SELECT are kept as uppercase-only keyword
   stubs because Task 5 parses them into an opaque DbStub node;
   lowercase `insert`/`select` fall through to Ident, exactly like rt
   (CLAUDE.md gotcha — this is the whole reason Task 3 exists as a
   from-scratch lexer rather than a copy of rt's). *)

type kind =
  (* literals *)
  | Ident of string
  | Int of int
  | Str of string
  (* milestone-1 keywords *)
  | KwType
  | KwClass
  | KwInterface
  | KwFn
  | KwLet
  | KwMut
  | KwTake
  | KwReturn
  | KwIf
  | KwElse
  | KwWhile
  | KwFor
  | KwIn
  | KwTrue
  | KwFalse
  (* uppercase-only SQL-layer stubs (Task 5 parses these into a DbStub
     span); lowercase "insert"/"select" are plain Ident, never these. *)
  | KwInsert
  | KwSelect
  (* punctuation / operators, mirroring rt's generic set *)
  | LBrace
  | RBrace
  | LParen
  | RParen
  | LBracket
  | RBracket
  | Comma
  | Semicolon
  | Colon
  | Dot
  | DotDot (* .. *)
  | Question
  | At
  | Pipe
  | Arrow (* -> *)
  | FatArrow (* => *)
  | Dash
  | Plus
  | Star
  | Slash
  | Percent
  | Eq
  | EqEq
  | NotEq
  | Lt
  | LtEq
  | Gt
  | GtEq
  | PlusEq
  | MinusEq
  (* meta *)
  | Newline
  | Eof

(* line and col are both 1-based, matching rt's Token and diag.ml's
   site convention. *)
type t = { kind : kind; line : int; col : int }
