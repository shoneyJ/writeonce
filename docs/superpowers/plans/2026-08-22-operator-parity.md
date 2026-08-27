# Iteration 36 — operator parity (`not`, bitwise, hex literals, compound assigns): implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task.
> Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** the language gains boolean `not`, the five Int bitwise
operators (`&` `|` `^` `<<` `>>`), hex/binary/underscore integer
literals, and working compound assigns (`+=` `-=` `*=` `/=` `%=`) —
full stack, `.wob` v6.

**Architecture:** front-to-back in dependency order — tokens/lexer,
then AST/parser, then the type checker, then emit + VM opcodes, then
the corpus/gates closeout. Every semantic decision is already settled
in the story ("Settled decisions", 2026-08-22): `>>` arithmetic,
out-of-range shift count traps (literal counts rejected at compile
time), `not` at the unary level beside minus.

**Tech stack:** OCaml compiler (`compiler/src/`), C runtime/VM
(`runtime/src/`), dune + just gates.

**Spec:** the story IS the spec —
`docs/stories/language-runtime-database/36-operator-parity.md`
(story-19 deviation precedent: decisions recorded normatively there,
reasoning-under-the-code lands in CODE-LOGIC.md as part of this plan).

## Global constraints

- Plans in this repo carry no code blocks — each step names the exact
  file/anchor and describes the change; the executor writes the code.
- Every pre-existing fixture must lex, parse, and run byte-identically
  (the iteration-19 discipline). Any golden that changes is a defect in
  the change, not a bless-and-move-on.
- Precedence copies Go (`.dev/reference/go/src/go/token/token.go:266`):
  `&` `<<` `>>` join the multiplicative rung, `|` `^` join the additive
  rung — no new rungs, both above comparison.
- Bitwise is Int-only. No Float, Bool, Text, Bytes operands — the
  existing operand-type diagnostics reject them; `numeric_world`
  (`types.ml:193`) is not touched.
- Run gates via just recipes only (`just woc-test`, `just wovm-test`,
  `just oop-accept`, `just web-app`, `just deps-accept`).
- Branch off master first (never commit on master); bullet-style commit
  messages, ≤25 lines, no push.

---

## Task 1 — lexer and tokens: the new surface exists as tokens

**Files:** modify `compiler/src/token.ml` (kind list ~134-150),
`compiler/src/lexer.ml` (keyword table ~131-147, operator switch
~500-570, digit scanner ~386-430), `compiler/src/dump.ml` (kind labels
~103).

**Interfaces produced:** tokens `Amp`, `Caret`, `Shl`, `Shr`,
`StarEq`, `SlashEq`, `PercentEq`, keyword `KwNot`; hex (`0x`), binary
(`0b`), and underscore-separated integer literals arriving as ordinary
`Token.Int`.

- [ ] Add the seven operator tokens and `KwNot` to `token.ml`'s kind
  list, each with the one-line comment style the file already uses;
  add `not` to the lexer keyword table beside `and`/`or`
  (`lexer.ml:144-145`) — grep the corpus and samples first to confirm
  `not` is used nowhere as an identifier (the discipline every keyword
  above it followed).
- [ ] Extend the operator switch: new `&` and `^` cases (single-char,
  today they fall to the error branch); `*`, `/`, `%` gain a peek for
  `=` (the `+`/`+=` two-char pattern at `lexer.ml:543-549` is the
  template); `<` gains a peek for a second `<` (order: `=` first so
  `<=` stays `LtEq`, then `<`), `>` symmetrically. No `<<=`/`>>=` —
  bitwise compound assigns are out of scope per the story.
- [ ] Extend the digit scanner: after an initial `0`, a peek for `x`
  or `b` switches to hex/binary accumulation (case-insensitive hex
  digits; at least one digit required, else the existing malformed-
  number path); `_` is skipped between digits in every integer form
  (leading/trailing/doubled `_` falls out as an error naturally).
  Accumulation wraps in i64 like the VM's own arithmetic (the ADD
  doctrine, `wob.h:194`). The bare-decimal and float paths stay
  byte-identical — the iteration-19 comment at `lexer.ml:387` explains
  exactly which branch keeps them safe.
- [ ] Add dump labels for the eight new kinds beside PLUSEQ/MINUSEQ
  (`dump.ml:103`).
- [ ] Verify: `just woc-test` — every existing golden unchanged (new
  tokens are unreachable from old source). Commit.

## Task 2 — AST and parser: the grammar accepts what the tokens spell

**Files:** modify `compiler/src/ast.ml` (unop at :149, binop at :158),
`compiler/src/parser.ml` (ladder doc ~594, additive/multiplicative
~860-905, unary at :908, assign statement at :1489, an id-freshening
clone helper beside `subst_expr` ~2068).

**Interfaces produced:** `unop` gains `Not`; `binop` gains `BAnd`,
`BOr`, `BXor`, `Shl`, `Shr`; the assign statement accepts all five
compound forms and desugars them at parse time.

- [ ] Extend `ast.ml`: `Not` in `unop`, the five bitwise constructors
  in `binop`, with a doc comment recording the Go-rung decision and
  pointing at the story's settled-decisions section.
- [ ] Parser rungs: `parse_multiplicative` also accepts `Amp`/`Shl`/
  `Shr`, the additive function also accepts `Pipe`/`Caret`, mapping to
  the new constructors. Update the ladder doc comment (~594) to show
  the new members of each rung and the two grouping consequences
  (`x & mask == 0` groups the AND first; shifts bind tighter than
  `+`). The declaration-position use of `Pipe` (union types) parses in
  a different grammar and is untouched — confirm by the goldens.
- [ ] `parse_unary` (:908) gains `KwNot` beside `Dash`, producing
  `Unary (Not, operand)` — the Lua placement the story settled:
  `not a == b` groups `(not a) == b`.
- [ ] Compound assigns at the assign site (:1489): after a place
  expression, `PlusEq`/`MinusEq`/`StarEq`/`SlashEq`/`PercentEq` build
  the same `Ast.Assign` whose value is a `Binary` of the matching
  arithmetic op over a CLONE of the target and the parsed right-hand
  side. The clone must freshen every node id (`fresh_id`) — write the
  small recursive helper beside `subst_expr`, which is the precedent
  for rebuilding expressions. Semantics are therefore exactly the
  written-out form (`x += e` ≡ `x = x + e`), including an index
  expression evaluating twice — that equivalence is the documented
  contract, pinned by a fixture in Task 5.
- [ ] Verify: `just woc-test` unchanged; hand-compile a scratch file
  with each new form and confirm the AST dump groups per the ladder
  doc. Commit.

## Task 3 — type checker: Int-only bitwise, Bool-only not, literal shift counts

**Files:** modify `compiler/src/types.ml` (the `Binary` operand
tables — the And/Or Bool-wiring near :868/:1196 and the arithmetic
table that owns `Add` are the anchors), `compiler/src/diag.ml` if the
new code needs registering.

**Interfaces produced:** `BAnd`/`BOr`/`BXor`/`Shl`/`Shr` check as
Int × Int → Int; `Not` checks as Bool → Bool; a literal shift count
outside 0..63 is a compile error.

- [ ] Wire the five bitwise ops into the same checker table `Add`
  lives in, Int-only on both sides (no Float twin — nothing like
  FADD exists for them, and `numeric_world` stays untouched). The
  wrong-operand diagnostic is the existing operator-type family the
  corpus fixture `compile-fail/lang-and-non-bool-operand` demonstrates
  for and/or — same shape, Int spelled where Bool was.
- [ ] Wire `Not` as Bool → Bool through the same path `And`/`Or`
  operands use.
- [ ] Literal shift counts: when a `Shl`/`Shr` right operand is an
  integer literal outside 0..63, emit the iteration's one NEW
  diagnostic at the operand's position — "shift count N is out of
  range 0..63". Take the next free code in the WO-E2 family (grep
  diag.ml/types.ml for the current highest; WO-E205 is the last known
  at `types.ml:1509`) and register it wherever that family is listed.
- [ ] Verify: `just woc-test`; scratch files confirm each rejection
  fires at the right position and each accepted form types as Int/Bool.
  Commit.

## Task 4 — emit and runtime: five opcodes, one trap kind, `.wob` v6

**Files:** modify `runtime/src/wob.h` (opcode enum after `WOP_FLE = 41`,
trap enum after `WO_T_FK = 11` at :186, `WOB_VERSION` at :16),
`runtime/src/vm.c` (dispatch table ~1058 and the arithmetic label
block), `runtime/src/loader.c` (the per-opcode validation switch,
~534), `compiler/src/emit.ml` (the `Binary` lowering that maps `Add`
to `WOP_ADD`, and the `Unary` site), `compiler/src/disasm.ml` (opcode
names), plus the runtime's own disassembler if `runtime/src` carries
one (grep for where FADD got its name).

**Interfaces produced:** `WOP_BAND = 42`, `WOP_BOR = 43`,
`WOP_BXOR = 44`, `WOP_SHL = 45`, `WOP_SHR = 46`; trap kind
`WO_T_SHIFT = 12`; `WOB_VERSION` 5 → 6.

- [ ] Add the five opcodes to `wob.h` with the house comment style: A
  B C register forms on i64; SHL/SHR document the trap contract (count
  outside 0..63 traps WO_T_SHIFT) and that SHR is arithmetic
  (sign-extending) per the story. Bump `WOB_VERSION` to 6 and extend
  its comment with a one-line v6 entry (the v5 comment is the
  template). Add `WO_T_SHIFT` to the trap enum with its message wired
  wherever the other trap kinds name theirs.
- [ ] VM: five new labels in the computed-goto table (~1058) and
  cases in the ISO-switch flavor (both dispatch flavors are gated —
  the wovm-test comment in the justfile says why). AND/OR/XOR are
  single-expression cases beside ADD; SHL/SHR range-check the count
  register first and trap WO_T_SHIFT, then shift (SHR on the signed
  value — C's signed right shift is arithmetic on every platform the
  runtime supports, but write it via the explicit sign-preserving
  idiom the codebase prefers if one exists; check how DIV guards
  INT64_MIN for the local style).
- [ ] Loader: the validation switch (~534) accepts the five new
  opcodes with three-register operand checking, same arm shape as ADD.
- [ ] Emit: extend the `Binary` lowering table (find where `Add`
  becomes `WOP_ADD` — `Ast.` names may be opened, grep for `Concat`'s
  lowering) with the five new mappings. Lower `Unary Not` with NO new
  opcode: Bool is 0/1, so `not x` is the existing WOP_EQ against a
  zero constant (the And/Or comment at `ast.ml:151` records the same
  no-new-opcode doctrine for the short-circuit pair). Compound assigns
  need nothing here — they died in the parser.
- [ ] Disasm: names for the five opcodes in `compiler/src/disasm.ml`
  and the runtime twin if it exists.
- [ ] Verify: `just woc-build && just wovm-build`, then `just
  woc-test` and `just wovm-test` (the ASan+UBSan gate, both dispatch
  flavors). A scratch program exercising every operator, a
  variable-count in-range shift, and a caught out-of-range shift (try/
  catch over the trap) prints the expected values. Commit.

## Task 5 — corpus, gates, docs: pin everything, close out

**Files:** create fixtures under `tests/corpus/run/`,
`tests/corpus/compile-fail/`, `tests/corpus/trap/`; modify
`docs/plan/oop-vm/00-wob-format.md` (v6 section),
`compiler/src/CODE-LOGIC.md` and `runtime/src/CODE-LOGIC.md`
(reasoning-under-the-code), the story file (move refine → done, both
frontmatter and folder in the same change), `docs/stories/00-status.md`
(standup entry).

- [ ] Run fixtures (the `run/arithmetic` fixture is the shape
  template): one covering all five bitwise operators including the two
  grouping pins (`x & mask == 0`, a shift mixed with `+`); one for
  hex/binary/underscore literals proving value-identity with decimal
  twins; one for `not` (plain, chained with and/or, and the
  `not a == b` grouping pin); one for compound assigns on a let, a
  field, and an index place, including the desugar-equivalence pin (an
  index expression with a visible side effect running twice, exactly
  as the written-out form would).
- [ ] The consumer proof: a fixture computing HMAC's ipad/opad XOR
  step in pure `.wo` — `byte_at` over a key Text, `^` with the 0x36
  and 0x5c pad constants — matching the RFC 2104 test-vector
  intermediate. This is the story's acceptance criterion that
  iteration 34 is demonstrably unblocked.
- [ ] Compile-fail fixtures: `not` on an Int operand; a bitwise
  operator on Bool and on Float; a literal shift count of 64 and of a
  negative literal (the new diagnostic, both sides of the range).
- [ ] Trap fixture: a variable-count shift receiving 64 at run time
  traps WO_T_SHIFT — uncaught surface first, then a try/catch arm
  reading the error code, whichever shape `tests/corpus/trap/`'s
  existing fixtures use.
- [ ] Dead-token defect closure: confirm the pre-change WO-E101 on
  `x += 1` is gone by the compound-assign run fixture existing at all;
  note the closure in the story's landing blockquote.
- [ ] Gates, all of them: `just woc-test`, `just wovm-test`,
  `just oop-accept`, `just web-app`, `just deps-accept` — every count
  at or above its story-recorded level, zero failures.
- [ ] Docs closeout: v6 section in `00-wob-format.md` (five opcodes,
  trap kind, version gate — the v5 section is the template);
  CODE-LOGIC.md on both sides for the decisions that live in code
  (arithmetic SHR, trap-not-mask, `not` as EQ-zero, parse-time
  compound-assign desugar and its double-eval contract); story file
  set to `status: done` with a landing blockquote in the
  story-15/16 voice; standup entry in `00-status.md` answering the six
  questions (reference project: `.dev/reference/go`).
- [ ] Commit.

## Self-review notes

- Story coverage: goals 1-5 map to Tasks 1-4; every acceptance
  criterion has a Task-5 fixture or gate; the settled decisions are
  restated at their implementation sites so no executor re-litigates
  them.
- Type consistency: token names (Amp/Caret/Shl/Shr/StarEq/SlashEq/
  PercentEq/KwNot), AST names (Not, BAnd/BOr/BXor/Shl/Shr), opcode
  names/ids (WOP_BAND..WOP_SHR = 42..46), and the trap kind
  (WO_T_SHIFT = 12) are spelled identically in every task that touches
  them.
- Known allowance: Task 3's diagnostic code is "next free in the
  WO-E2 family" rather than a fixed number — the family's registry is
  the source of truth and hard-coding here would rot.
