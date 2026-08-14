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

type str_part =
  | SText of string (* literal text, escapes already applied *)
  | SExpr of string (* raw, unlexed source of one `${...}`'s body *)

type kind =
  (* literals *)
  | Ident of string
  | Int of int
  | Str of string
  (* haxe-parity Task 2: a string literal containing at least one
     `${expr}` interpolation. Alternating text/expr segments, in source
     order; SExpr carries the *raw, unlexed* source text between the
     `${` and its matching `}` (nested braces/strings skipped verbatim
     by the lexer's own scan) -- the parser re-tokenizes/re-parses it as
     a real expression, which is where "desugars at parse time to
     concatenation" actually happens (ast.ml/parser.ml). A plain string
     with no `${` never produces this -- it still lexes as a bare Str,
     byte-identical to every pre-existing fixture. *)
  | InterpStr of str_part list
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
  (* haxe-parity Task 1 (modules): `use <path>` top-level import and the
     `pub` visibility marker on class/interface/fn declarations. Real
     keywords, not positionally-recognized idents like insert/select or
     ref/multi/map -- neither name is used as an identifier anywhere in
     the existing corpus/fixtures, so there is no rt-parity or
     field-name collision to dodge (see lexer.ml's module doc for why
     those other names stayed idents). *)
  | KwUse
  | KwPub
  (* haxe-parity Task 2 (small control surface): break/continue/do-while,
     const values, and/or booleans, and inline-fn rejection (the haxe
     verdict table's own row: "const compile-time values; inline
     *functions* rejected"). All real keywords -- none collides with an
     existing corpus identifier (grepped before adding, same discipline
     Task 1 used for use/pub). *)
  | KwBreak
  | KwContinue
  | KwDo
  | KwConst
  | KwAnd
  | KwOr
  | KwInline
  (* haxe-parity Task 3: `switch`/`case`/`default` — real keywords (none
     collides with an existing corpus/sample identifier, grepped first,
     same discipline Tasks 1/2 used for use/pub/break/etc.). *)
  | KwSwitch
  | KwCase
  | KwDefault
  (* haxe-parity Task 5: `try expr catch (e) arm` — real keywords, and
     neither appears as an identifier anywhere in the corpus or the
     driving workload (grepped, the same discipline every keyword above
     followed). *)
  | KwTry
  | KwCatch
  (* haxe-parity Task 6: the `?T` absent value. A keyword, not an
     identifier — `nil` appears in the corpus and the driving workload
     only ever as this literal. *)
  | KwNil
  (* haxe-parity: `expr as Type` — the checked-decode cast. Only meaningful
     over `json.decode(text)`, whose result has no type until one is named. *)
  | KwAs
  (* haxe-parity Task 4: `typedef Name = { ... }` structural records. A
     real keyword (grepped the corpus/sample first, same discipline as
     every keyword above — `typedef` appears only as this declaration's
     own leading word, never as an identifier). Union declarations reuse
     the existing KwType (`type Name = A | B` vs. the struct form
     `type Name { ... }` — disambiguated by the token after the name). *)
  | KwTypedef
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
