---
iteration: "36"
status: in-progress
---

# Iteration 36 — operator parity: `not`, bitwise, hex literals, compound assigns

> Format: fiberloom `product/story-iteration-template`. Part of
> [Story — one language, one runtime, one database, one binary](00-story.md).
>
> **Inserted 2026-08-22, forks settled the same day** (decisions below,
> story-19 discipline). Driver: a gap survey of the operator surface
> against the Go reference checkout (`.dev/reference/go` —
> `src/go/token/token.go`, spec §Arithmetic operators) found four holes
> that every mainstream language covers. The survey also found a latent
> defect: `+=`/`-=` are lexed (`lexer.ml:541/548`, tokens at
> `token.ml:149-150`) but no parser rule consumes them — dead tokens
> since the haxe-parity control-surface task, zero uses in the corpus,
> so `x += 1` today dies as a generic WO-E101 instead of working or
> being honestly absent.
>
> The driving consumer is iteration 34 (crypto builtins): HMAC's
> ipad/opad step is a byte-wise XOR, and the doctrine question recorded
> there — C-side builtin vs pure-`.wo` — is unanswerable while the
> language cannot spell XOR at all. Secondary consumer: `http/auth.wo`'s
> pure-`.wo` base64 does with division/modulo what shifts and masks say
> directly.
>
> **Plan:** [`2026-08-22-operator-parity.md`](../../superpowers/plans/2026-08-22-operator-parity.md)
> (5 tasks: lexer/tokens; AST/parser incl. compound-assign desugar;
> type checker incl. literal shift-count rejection; emit + VM opcodes
> `.wob` v6; corpus/gates/docs closeout).
>
> **CODE LANDED 2026-08-22** (branch `operator-parity`), full stack in
> one day: tokens/lexer (incl. hex `0x` / binary `0b` / `_` separators),
> Go-rung parsing, WO-E201 Int-only/Bool-only checking, WO-E223 literal
> shift rejection, opcodes `WOP_BAND..WOP_SHR` 42-46 + `WO_T_SHIFT` as
> `.wob` v6. The dead-token defect is closed: `x += 1` parses (all five
> compound assigns, parse-time sugar — the desugar-equivalence contract
> incl. double index-eval is in `compiler/src/CODE-LOGIC.md`). Gates:
> `woc-test` 543/0, `wovm-test` ASan both flavors, `oop-accept` ALL MET,
> `deps-accept` 8/0, `web-app` 26/0 — all unchanged. **Deviation by
> developer directive: NO test fixtures were written** — acceptance is
> MANUAL via the new sample `docs/examples/operators/` (`woc .`, run,
> read the ok/FAIL lines; `trap` mode proves WO_T_SHIFT). The corpus
> pins this plan called for (grouping, arithmetic `>>`, ipad/opad,
> trap) live in that sample instead. Status stays in-progress until the
> developer's manual pass; the AC below reads as written pre-deviation.

## Why this iteration exists

The expression grammar (`parser.ml` ~594, `token.ml:134-150`) stops at:
arithmetic `+ - * / %`, concat `..`, comparison `== != < <= > >=`,
logical `and`/`or`, unary minus. Four common gaps, in priority order:

1. **No boolean negation.** No `not`, no `!`, no unary NOT node in the
   AST. The only spelling is `x == false` — every caller pays, every
   day. Highest value for smallest surface.
2. **No bitwise operators.** No `&`, `|`, `^`, `<<`, `>>` in any form.
   Blocks iteration 34's HMAC honestly; base64/multipart boundary code
   emulates masks with arithmetic.
3. **No hex/binary integer literals.** The lexer scans decimal digit
   runs only (`lexer.ml:386` on). Bitwise code without `0xFF` masks is
   write-only.
4. **Compound assigns half-promised.** `+=`/`-=` lex but never parse
   (the latent defect above); `*=` `/=` `%=` do not exist at all.

Deliberately NOT gaps (doctrine, not omission): `&&`/`||`/`!` symbol
forms (words won — KwAnd/KwOr precedent), ternary `?:` (switch is
already an expression, `parser.ml:1181`), `++`/`--`, function values,
inheritance-family keywords (WO-E105 rejects them by name).

## Goals

- **`not` keyword**, unary boolean, sitting with unary minus in the
  grammar (settled below: Lua placement, binds tighter than comparison).
  Bool-only operand, diagnosed like every other type error.
- **Five bitwise operators on Int only**: `&` AND, `|` OR, `^` XOR,
  `<<` left shift, `>>` right shift. Precedence copies Go's C-trap fix
  (`token.go:266-280`): `&`/`<<`/`>>` at the multiplicative level,
  `|`/`^` at the additive level — both ABOVE comparison, so
  `x & mask == 0` groups the AND first. Complement is spelled
  `-1 ^ x` (single signed 64-bit Int makes Go's mask rule collapse to
  exactly this); no `~`, no unary `^`, no `&^`.
- **Hex and binary literals**: `0x` and `0b` prefixes on Int, plus `_`
  digit separators in all integer forms. Decimal lexing stays
  byte-identical for every existing fixture (the iteration-19
  discipline).
- **Compound assigns wired**: `+=` `-=` parse into the existing
  assignment statement (resurrecting the dead tokens), `*=` `/=` `%=`
  join them. Statement-level sugar over the existing place logic —
  no new AST evaluation semantics.
- **Runtime**: new i64 register opcodes beside `WOP_ADD..WOP_NEG`
  (`runtime/src/wob.h:194`) for AND/OR/XOR/SHL/SHR (+NOT if the emitter
  wants it); `.wob` version bump, loader validation, disasm coverage —
  the v5 (Float/Bytes) change is the template.

## Settled decisions (2026-08-22)

1. **`>>` is arithmetic (sign-extending).** Go's own choice for signed
   integers, and writeonce has exactly one Int, signed 64-bit. For the
   driving consumers the fork is moot anyway — crypto/base64 shift
   byte-range values whose sign bit is never set, where arithmetic and
   logical are bit-identical. No logical-shift operator ships; a caller
   who wants zero-fill masks first.
2. **Out-of-range shift count traps, like DIV0.** Valid counts are
   0..63; a negative or ≥64 count at run time is a named trap on the
   same machinery as `WOP_DIV`'s DIV0 (`wob.h:197`) — the established
   Int honesty precedent (story 19 kept the trap deliberately). NOT
   Go's saturate-to-0/-1 (spec surface serving generic-width code
   writeonce doesn't have) and NOT hardware masking (x86's count%64
   makes `x << 64 == x`, the classic silent wat). Real code shifts by
   literals: when the count is a literal, the compiler rejects it at
   emit time and the trap never runs.
3. **`not` sits at the unary level, beside minus (Lua placement).**
   `not a == b` parses `(not a) == b`. The grammar's ordering is
   already anchored to Lua by name (`parser.ml` ~601 "This ordering
   matches Lua's"), and joining `parse_unary` adds zero new precedence
   rungs. Python's looser placement reads closer to English, but it
   buys a new rung to prevent a misparse that Bool-only typing already
   converts into a compile error in every mixed-type case; the one
   silent case (Bool compared to Bool through `not`) gets a pinned
   fixture so the choice stays visible. `if not done` and `while not
   empty()` — the actual daily uses — read identically under both.

## Acceptance Criteria

- **Given** the corpus, **when** fixtures land for `not` (plain,
  chained with and/or, non-Bool operand diagnosed), each bitwise
  operator, precedence pins (`x & mask == 0` and one shift-vs-additive
  case), hex/binary/underscore literals (value-identical to decimal
  twins), out-of-range shift (literal count rejected at compile time,
  variable count trapping at run time), the `not a == b` grouping pin,
  and all five compound assigns on let/field/index places, **then**
  corpus passes with zero regressions on every pre-existing fixture.
- **Given** iteration 34's ipad/opad step written in pure `.wo` with
  `^` over `byte_at` values, **when** it runs against an RFC 2104 test
  vector's intermediate, **then** the bytes match — the consumer that
  motivated the iteration is demonstrably unblocked.
- **Given** the existing gates (`just corpus`-equivalent, `just
  web-app`, `just deps-accept`, `test_wal`), **when** the iteration
  lands, **then** all pass unchanged — the `.wob` bump breaks no
  replay/loader path.
- **Given** `x += 1` in a fixture today-vs-after, **when** compiled,
  **then** the before is the recorded WO-E101 and the after executes —
  the dead-token defect is provably closed, not papered over.

## Out Of Scope

Unary complement operator (spell `-1 ^ x`), Go's `&^` AND-NOT, bitwise
compound assigns (`&=` family), `++`/`--`, ternary, symbol forms
`&&`/`||`/`!`, bitwise on Float/Bytes/Bool, overflow-checked arithmetic
changes, octal literals.

## Proposed Solution

Lexer: extend the operator switch (the `+`/`+=` two-char pattern at
`lexer.ml:541-548` is the template) for the five operators and the
three new compound assigns; extend the digit scanner with `0x`/`0b`
prefix branches and `_` skipping, leaving the bare-decimal path
untouched. Parser: two new precedence rungs threaded into the existing
recursive-descent chain exactly where Go's table says; `not` joins
`parse_unary`; compound assigns join the statement that already
matches `Token.Eq` (`parser.ml:510`). Types: all five operators and
`not` are `Int -> Int -> Int` / `Bool -> Bool` in the same checker
table that owns `+`. Emit/runtime: new opcodes in `wob.h`'s i64 block,
`WOB_VERSION` bump, interpreter cases beside the existing wrapping
arithmetic, disasm names. The forks are settled above, so no separate
spec is owed (the story-19 deviation precedent: format decisions
recorded normatively where they land, reasoning in CODE-LOGIC.md); the
plan follows once this story is approved on the board.
