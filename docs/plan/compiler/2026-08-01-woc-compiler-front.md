# woc Compiler Front (OCaml) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
>
> **Style rule (user convention):** this plan states concept, reason, and required behavior in words. The executor writes the actual code at implementation time; nothing here is copy-paste source.

**Goal:** Build the OCaml compiler front — lexer, parser, typechecker, and the mutable-value-semantics ownership pass — so milestone-1 `.wo` programs typecheck, ownership-check, and produce the analysis tables the plan-3 emitter consumes, with the diagnostic quality the spec calls "the product".

**Architecture:** Plan 2 of 3 for the approved spec `docs/superpowers/specs/2026-08-01-oop-compiler-vm-design.md`. New root-level `compiler/` directory: a dune project with one library (token, lexer, ast, parser, types, owner, diag, dump modules) and one executable (`woc`). Handwritten lexer and recursive-descent parser mirroring the conventions of the Rust runtime's front end (`crates/rt/src/{token,lexer,ast,parser}.rs`) — same newline significance, same keyword gotchas. No bytecode yet: plan 3 owns emission; this plan's contract with plan 3 is the typed AST plus per-site ownership decision tables, exposed through stable dump formats.

**Tech Stack:** OCaml (≥ 4.14) + dune, both from apt — NOT currently installed on the dev box; Task 1 installs them. OCaml stdlib only: no opam packages, no Menhir, no ppx. Golden-file testing under `dune runtest` with a tiny hand-rolled assert/diff runner.

## Global Constraints

- **OCaml stdlib only** — no Menhir, no ppx, no opam libraries; dune is the build runner only (spec dependency doctrine).
- **Handwritten lexer + recursive-descent parser** (spec approach A).
- **Newline-significant lexing** — newline tokens are real and parsers use them, exactly like the Rust runtime (CLAUDE.md gotcha).
- **Keyword discipline** — `self`, `me`, `subscribe`, `receive`, lowercase `insert`/`select` stay identifiers; the parser recognizes them positionally (CLAUDE.md gotcha; breaking this breaks existing `.wo` samples).
- **Diagnostics carry stable `WO-E###` codes**, file:line:col, a source excerpt, and — for ownership errors — both conflicting sites (spec section 6: "these messages are the product").
- **Multi-error reporting** — the parser recovers at declaration/statement sync points; first-error-stop is a defect.
- **Compile-speed budget:** the pricing-demo logic subset must lex+parse+check in well under 100 ms (spec success criterion 1 measures the full pipeline; the front end must leave headroom).
- **Git: the executing agent NEVER runs `git commit`** — append the given draft to `.dev/commit.md`; the user commits.
- **Docs rule:** documentation under `docs/`; `compiler/README.md` stays an orientation README.

---

## File Structure

```
compiler/
  dune-project
  README.md                orientation: pipeline map, build/test commands
  src/
    dune                   library stanza
    diag.ml                diagnostic type, WO-E code registry, rendering, exit codes (Task 2)
    token.ml lexer.ml      token variants + newline-significant lexer (Task 3)
    ast.ml parser.ml       AST + declaration/statement/expression parsers (Tasks 4–5)
    types.ml               symbols, field-kind derivation, structural interfaces, expression typing (Task 6)
    owner.ml               MVS flow analysis, ownership errors, residual/drop/rc tables (Task 7)
    dump.ml                stable text dumps of tokens/AST/typed info/owner decisions (grows Tasks 3–7)
  bin/
    dune main.ml           the woc executable: CLI, file discovery, pipeline driver (Task 1, polished Task 8)
  test/
    dune runner.ml         golden runner: compile fixture with a dump flag, diff expected (Task 3 onward)
    golden/                fixture .wo files + .expected files, one directory per stage
docs/plan/oop-vm/01-error-catalog.md   every WO-E code with meaning (Task 8)
justfile                   woc-build / woc-test recipes (Task 1)
```

## Contract with plan 3 (what "done" hands over)

The emitter consumes: (1) the typed AST; (2) a per-class field-kind table using the six `.wob` kinds of `docs/plan/oop-vm/00-wob-format.md`; (3) the structural-satisfaction set (which class satisfies which interface via which methods); (4) the owner pass's per-site tables — moves, scope-end drop sets, rc inc/dec sites with elision marks, and residual-borrow sites. All four have stable dump renderings, so plan 3 can pin them with goldens before emitting a single opcode.

---

### Task 1: Toolchain + project scaffold

**Files:** create `compiler/dune-project`, `compiler/src/dune`, `compiler/bin/{dune,main.ml}`, `compiler/README.md`; modify `justfile`.

**Concept & reason:** OCaml and dune are not on the dev box — install both from apt (Ubuntu 24.04 ships OCaml 4.14-era packages; that is the version floor). Scaffold the dune project: an empty library and a `woc` executable that prints usage and exits 2 — establishing the exit-code contract early (0 = clean, 1 = diagnostics reported, 2 = usage/IO failure). Root justfile gains `woc-build` and `woc-test`. The README states the pipeline map and the one-command build/test story, mirroring how `runtime/README.md` orients its half.

- [ ] Install: `sudo apt install ocaml dune` (or confirm present); record versions in the README's requirements line.
- [ ] Scaffold project; `dune build` produces the `woc` binary; running it with no args prints usage, exit 2.
- [ ] Add just recipes; verify both work from repo root.
- [ ] Record commit draft: `feat(compiler): OCaml/dune scaffold — woc executable stub with exit-code contract (0 clean/1 diagnostics/2 usage), just woc-build/woc-test, README orientation.`

### Task 2: Diagnostics module

**Files:** create `compiler/src/diag.ml`; test via the runner (unit assertions).

**Concept & reason:** every later stage reports through one channel, so it comes first. A diagnostic is: stable code (`WO-E###`), severity, file, line, column, message, and zero or more *related sites* (file/line/col + short label) — the ownership pass needs two-site errors ("moved here … used here"). Rendering shows the source excerpt with a caret under the column, the way rustc/OCaml do; related sites render beneath the primary. A collector accumulates diagnostics in source order across recovery, deduplicates identical (code, site) pairs, and decides the process exit code. Code ranges are reserved per stage now — lexing E0xx, parsing E1xx, types E2xx, ownership E3xx — so the Task-8 catalog is an enumeration, not an archaeology dig.

- [ ] Failing unit tests: rendering shape (code, position, caret excerpt, related site), collector ordering and dedup, exit-code decision.
- [ ] Implement; green under `dune runtest`.
- [ ] Record commit draft: `feat(compiler): diag module — WO-E coded diagnostics with excerpts, related sites (two-site ownership errors), ordered dedup collector, exit-code decision.`

### Task 3: Tokens + lexer

**Files:** create `compiler/src/token.ml`, `compiler/src/lexer.ml`, `compiler/src/dump.ml` (token dump), `compiler/test/runner.ml` + golden fixtures.

**Concept & reason:** the lexer defines what the language literally is, and it must agree with the Rust runtime's lexer on every convention or the two front ends will diverge on the same samples. Behaviors: position-tracked tokens; **newline tokens are emitted, never filtered**; `--` line comments; integer and string literals; annotation introducer `@`; the milestone-1 keyword set (declaration and statement words like type, class, interface, fn, let, mut, take, return, if, else, while, for, in, plus uppercase SQL-layer INSERT/SELECT) — and, critically, the *non-keywords*: `self`, `me`, `subscribe`, `receive`, lowercase `insert`/`select` lex as plain identifiers (CLAUDE.md gotcha — adding them to the keyword map breaks `expose subscribe` lists and method bodies). Unknown characters produce a lexing diagnostic and skip, so one bad byte doesn't kill the file. The golden framework arrives here: fixture `.wo` in, `--dump-tokens` out, diffed against an `.expected` file; a bless mode (environment variable) rewrites expectations intentionally.

- [ ] Failing goldens: a representative fixture covering comments, newlines, literals, annotations; a gotcha fixture proving the non-keyword identifiers.
- [ ] Implement lexer + dump + runner; goldens green.
- [ ] Record commit draft: `feat(compiler): newline-significant lexer mirroring rt conventions (self/me/subscribe/insert stay idents), --dump-tokens, golden test framework with bless mode.`

### Task 4: AST + declaration parser

**Files:** create `compiler/src/ast.ml`, `compiler/src/parser.ml` (declarations); extend `dump.ml` (`--dump-ast`); golden + must-fail fixtures.

**Concept & reason:** recursive descent over declarations: `interface` (method signatures only), `class` and `type` (identical field grammar — scalars, defaults including the explicit now() form, `ref T`, `multi T`, `map<K,V>`, field annotations like unique), type-level annotations (`@table` with its known keys, `@gc` bare, unknown annotation *names* skip silently — rt convention), and `fn` methods with the three parameter conventions (default borrow, `mut`, `take`). Two behaviors are load-bearing and get their own fixtures: **skip-on-block** — `service`/`policy`/`on <event>` blocks parse-and-discard with a brace-depth counter, because object literals inside trigger actions contain `}` that must not close the type (the exact bug rt's counter exists for); and **recovery** — a broken declaration syncs to the next top-level keyword and parsing continues, so one bad class yields one diagnostic, not a cascade. AST nodes carry positions and unique ids (the owner pass and emitter key their side tables on node ids).

- [ ] Failing goldens: pricing-demo-shaped class/interface fixtures dump correctly; skip-on-block fixture with a nested object literal; a two-error fixture reports both.
- [ ] Implement; green.
- [ ] Record commit draft: `feat(compiler): declaration parser — class/type/interface/fn signatures, @gc/@table annotations, brace-depth skip-on-block (service/policy/on), decl-level recovery, --dump-ast goldens.`

### Task 5: Statement + expression parser

**Files:** extend `parser.ml`, `ast.ml`, `dump.ml`; golden + must-fail fixtures.

**Concept & reason:** method bodies. Statements: `let` (with optional type ascription), assignment, `if`/`else`, `while`, `for … in`, `return`, expression statements. Expressions with a precedence ladder: arithmetic, comparison, text concatenation, unary minus, calls (free, method, and interface-typed method calls share one call node), field access chains, indexing (the residual-borrow trigger later), and **constructor literals** — `ClassName { field: expr, … }`, recognized by the two-token shape identifier-then-brace in expression position (the same shape trick rt uses for select expressions; this decision is normative here: milestone-1 construction syntax is the brace literal, no `new` keyword). SQL-layer statements (`insert …`, `select …`) parse into a single opaque DbStub node capturing their token span — the grammar stays whole, plan 3 emits the DB_STUB trap for them (spec's parse-but-trap story). Statement-level recovery syncs at newlines/semicolons.

- [ ] Failing goldens: a body fixture exercising every statement and precedence level; a constructor-literal fixture; an insert/select fixture dumping DbStub nodes; a recovery fixture with two body errors.
- [ ] Implement; green.
- [ ] Record commit draft: `feat(compiler): statement/expression parser — precedence ladder, constructor literals by ident-brace shape, indexing, insert/select as DbStub spans, statement recovery.`

### Task 6: Typechecker

**Files:** create `compiler/src/types.ml`; extend `dump.ml` (typed-info dump); golden + must-fail (WO-E2xx) fixtures.

**Concept & reason:** three products. (1) **Symbols and field kinds:** a two-pass walk (declare all, then check bodies) builds class/interface/free-fn tables and derives each field's `.wob` kind — Int/Bool/Timestamp/Id scalars → SCALAR, Text-like → TEXT, class-typed field → OWNED, `@gc`-class-typed → GCREF, `multi` → MULTI, `map` → MAP, and `ref T` → SCALAR (it is an id link, per spec section 3 rule 4). (2) **Structural interface satisfaction:** a class satisfies an interface exactly when it has a method matching every signature (name, arity, parameter types, return type); the satisfaction set — with the concrete method chosen per slot — is recorded for the emitter's vtables, and calling an interface method on a non-satisfying class is an error naming the missing/mismatched signature. (3) **Expression typing:** every expression node gets a type; checks cover operator operand types, call arity/types, field existence, constructor completeness (every field initialized or defaulted), method receiver rules, and builtin signatures (now, latest, count, words, print, print_int and the container operations) matching the VM's builtin table in the format doc. `self` types as the enclosing class; whether a method *mutates* (writes any field of self, directly or via a mut call) is computed here and recorded — the spec says self mutability is inferred, and the owner pass consumes the flag.

- [ ] Failing fixtures: typed-dump goldens for the pricing shapes; must-fail suite — type mismatch, unknown field, bad arity, unsatisfied interface (message names the missing method), incomplete constructor — each asserting its WO-E2xx code.
- [ ] Implement; green.
- [ ] Record commit draft: `feat(compiler): typechecker — two-pass symbols, .wob field-kind derivation (ref=scalar id, @gc=gcref), structural interface satisfaction sets for vtables, expression typing incl. builtins and constructor completeness, inferred self-mutability.`

### Task 7: Ownership pass (mutable value semantics)

**Files:** create `compiler/src/owner.ml`; extend `dump.ml` (`--dump-owner`); ownership must-fail suite (WO-E3xx) + decision goldens.

**Concept & reason:** the spec's novel core, and the reason this plan exists. A per-function forward dataflow walk (loops analyzed to a fixpoint by re-running the body once against joined states — conservative and simple) tracks each owned local through states: live, moved, borrowed. The MVS rules it enforces, each with a two-site diagnostic: use-after-move (assignment, return, `take` argument passing, and constructor field initialization are the move sites); move-while-borrowed; conflicting `mut` borrows of the same place where the analysis can prove aliasing; and the escape rule — borrows cannot be stored or returned (field kinds already forbid storage; returning a borrowed parameter's alias is the case caught here). `@gc`-typed values are exempt from all of it — aliasing them is legal by design.

Beyond errors, the pass computes the four tables plan 3's emitter consumes, keyed by AST node id: **move sites** (which MOVE is a real transfer); **scope-end drop sets** (which owned locals die at each scope exit — the source of DROP placement and of drop-map masks at every call/trap-capable site); **rc sites** (gcref alias creation/destruction, marked elided when creation and destruction are provably balanced in the same scope); and **residual sites** (places static proof fails — the canonical case: two `mut` element accesses through runtime indices in one region — where the emitter must emit runtime borrow ops). The `--dump-owner` rendering lists all four tables in source order; goldens pin them so emitter changes can never silently shift semantics.

- [ ] Failing must-fail suite: move-after-use, move-while-borrowed, double-mut on a provable alias, borrow escape via return — each with its WO-E3xx code and both sites in the message.
- [ ] Failing decision goldens: a fixture per table — moves, drops (including early-return paths), rc elision (balanced) vs kept (escaping alias), residual marking on indexed double-mut.
- [ ] Implement; all green. Then the perf smoke: time `woc` (check-only) on the pricing logic subset — comfortably inside the 100 ms budget.
- [ ] Record commit draft: `feat(compiler): MVS ownership pass — flow analysis with loop fixpoint, two-site errors (use-after-move, move-while-borrowed, double-mut, borrow escape), @gc exemption, and the four emitter tables (moves, scope-end drops, rc elision, residual borrow sites) with --dump-owner goldens.`

### Task 8: Driver polish + error catalog

**Files:** modify `compiler/bin/main.ml`; create `docs/plan/oop-vm/01-error-catalog.md`; modify `CLAUDE.md` (commands section), `compiler/README.md`.

**Concept & reason:** make `woc` behave like the toolchain the spec promises. Directory input discovers every `.wo` file under the path (same discovery contract as `wo run`); multiple files compile as one program (symbols span files); diagnostics print in (file, line) order regardless of discovery order; the dump flags operate per stage and compose with check-only mode. The error catalog documents every WO-E code shipped in Tasks 2–7 with a one-line meaning and an example message — the doc the conformance corpus (plan 3) and future users cite. CLAUDE.md's commands section gains the two just recipes and the one-line pipeline description.

- [ ] Failing tests: directory discovery + cross-file symbol resolution fixture; diagnostic ordering fixture.
- [ ] Implement; write the catalog (enumerating the registry — no code left uncataloged); sync docs.
- [ ] Full gate: `just woc-test` green; perf smoke re-run.
- [ ] Record commit draft: `feat(compiler): woc driver — directory discovery, cross-file programs, ordered diagnostics; docs/plan/oop-vm/01-error-catalog.md (full WO-E registry); CLAUDE.md commands sync.`

---

## Plan self-review notes

- **Spec coverage (plan-2 slice):** handwritten OCaml front end, stdlib-only, newline/keyword conventions, structural interfaces, field-kind derivation, MVS with second-class borrows, two-site ownership diagnostics, residual-site marking, drop/rc computation, multi-error recovery, sub-100 ms budget — all mapped. Deliberately deferred: bytecode emission, .wob output, conformance corpus, single binary (plan 3); `spawn`/messaging (sub-project 2).
- **Order rationale:** diagnostics before lexer (everything reports through it); parser split decl/body so skip-on-block lands before expression complexity; typechecker before owner (kinds and self-mutability feed the flow analysis); driver last when all stages exist.
- **Known accepted simplifications, documented in their tasks:** loop fixpoint by single re-run join; provable-aliasing only (residual sites cover the rest — that is the spec's hybrid design, not a gap); constructor syntax fixed as brace literal.

## Execution note

Requires `sudo apt install ocaml dune` (Task 1) — the only new toolchain on the box. Plan 1 (`runtime/`, wovm) need not be built for any task here; the two plans meet in plan 3.
