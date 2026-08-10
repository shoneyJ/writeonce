# `woc` compiler architecture

> The stable map of the OCaml compiler: pipeline, module contracts, data
> forms, invariants, and the marked reference paths that inform each stage.
> Task-level detail lives in the plans (see the index at the bottom); this
> document carries the shape that outlives them.

## 1. Purpose & doctrine

`woc` compiles `.wo` source to `.wob` register bytecode for the C VM
(`runtime/`, the `wovm` binary). Doctrine, locked by the spec:

- **OCaml stdlib only.** Handwritten lexer and recursive-descent parser — no
  Menhir, no ppx, no parser generators. dune is the build runner, nothing more.
- **Fast compiles are a feature.** No LLVM, no native backend, no
  monomorphization. Budget: the pricing demo compiles in under 100 ms.
- **Diagnostics are the product.** Mutable value semantics only beats Rust
  ergonomics if the errors are plain: stable `WO-E###` codes, two-site
  ownership messages, many errors per run, never abort.
- **The VM loader is the executable spec.** Every image `emit` produces must
  pass `wovm`'s loader validation; the golden end-to-end suite enforces this
  round-trip rule.

## 2. Pipeline

```
.wo files
   │  lexer      text → tokens          (newline-significant)
   │  parser     tokens → AST           (recursive descent, skip-on-block)
   │  types      AST → typed AST        (class/interface tables, field kinds)
   │  owner      typed AST → annotations (MVS flow analysis, four tables)
   │  emit       annotations → bytes    (registers, drop maps, vtables)
   ▼
app.wob ──► wovm (runtime/, plan 1) — loads, validates, executes
```

Side modules: `diag` (every stage reports into it), `dump`/`disasm`
(golden-test observation seams), `bin/woc` (driver).

## 3. Module contracts

One module per stage under `src/`; each block states what it does, what it
consumes/produces, and what it depends on.

**`diag`** — diagnostic records: stable code (`WO-E###`), `file:line:col`,
source excerpt, optional second site. A collector accumulates; nothing in the
compiler aborts on the first error. Depends on nothing.

**`token`** — token kinds and source positions. Depends on nothing.

**`lexer`** — source text → token stream. Newline-significant (`Newline`
tokens end policy/trigger lines — never filtered globally). Only SQL-layer
uppercase keywords are keywords; `self`, `insert`, `subscribe`, `receive`,
`me` stay plain identifiers (grammar parity with `crates/rt`). Consumes text;
produces tokens; depends on `token`, `diag`.

**`ast`** — untyped AST: declarations (class, interface, type, service,
policy, trigger), statements, expressions, each carrying a span. Depends on
`token` (positions).

**`parser`** — tokens → AST. Handwritten recursive descent. Brace-depth-aware
skip-on-block for constructs that parse but don't execute in milestone 1.
Error recovery at declaration/statement sync points so one bad line doesn't
eat the file. Consumes tokens; produces AST; depends on `lexer`, `ast`,
`diag`.

**`types`** — AST → typed AST. Builds the class table (field kinds derived:
SCALAR / OWNED / GCREF / TEXT / MULTI / MAP) and the interface table; checks
structural satisfaction (Go-style — a class satisfies an interface by method
shape, no declaration); infers `self` mutability per method (reads-only =
shared, writes = exclusive). Consumes AST; produces typed AST + tables;
depends on `ast`, `diag`.

**`owner`** — typed AST → ownership annotations. The MVS flow pass, per
function: borrows are second-class (cannot escape scope, cannot be stored or
returned). Produces four tables keyed to AST nodes: moves, drop points
(scope-end drops plus per-pc live-register masks for trap unwinding), rc
inc/dec pairs with provably-balanced elisions, and residual runtime-check
sites (the only places `emit` emits `BORROW_*` ops). Ownership errors name
both sites. Consumes typed AST; depends on `types`, `diag`.

**`emit`** — annotations → `.wob` image. Per-method register allocation
(≤ 64 registers, loader-enforced), instruction selection against the opcode
set, line tables, drop tables from `owner`'s masks, vtable rows from `types`'
satisfaction results. Consumes annotated AST; produces bytes per the format
contract; depends on `owner` and the format constants.

**`disasm`** — `.wob` → readable listing; drives `--dump-bc` golden tests.

**`dump`** — `--dump-ast` / `--dump-typed` printers; drives front-end golden
tests.

**`bin/woc`** — the driver. Three modes: `check` (front end only, exit code +
diagnostics), `emit` (write `.wob`), `build` (copy `wovm`, append the image +
offset trailer — the single self-contained binary).

## 4. Data forms

Six representations, five handoffs — each observable by a dump flag or a
golden fixture:

1. **Source text** (UTF-8 `.wo` files)
2. **Token stream** — kind, lexeme, position; `Newline` tokens significant
3. **AST** — untyped, spanned (`--dump-ast`)
4. **Typed AST** — every expression typed; class/interface tables attached
   (`--dump-typed`)
5. **Ownership-annotated AST** — the four `owner` tables keyed to nodes
6. **`.wob` image** — per the format contract (`--dump-bc` disassembles)

## 5. Architectural invariants

1. OCaml stdlib only; dune as runner; no generated parser, no ppx.
2. Diagnostics collect and continue; stable codes; never abort; ownership
   errors always name both sites.
3. Grammar parity with the Rust runtime's lexer/parser gotchas (newline
   significance, skip-on-block, keyword/ident split) until `crates/rt`
   retires — the shared conformance corpus enforces parity.
4. The loader is the contract: an emitted image the `wovm` loader rejects is
   an `emit` bug by definition, caught by the golden e2e suite.
5. No LLVM linkage, no native codegen — `.wob` only. The LLVM tree is study
   material (see §6), never a dependency.
6. Compile-speed budget: `woc` on the pricing demo < 100 ms on a developer
   laptop.

## 6. Reference-study map

Marked paths per stage. **porting source** = code this stage is ported from;
**contract** = artifact the stage must satisfy byte-for-byte; **study** =
architecture to learn from, never link against.

| Stage | Path | What to study | Role |
| --- | --- | --- | --- |
| lexer | `crates/rt/src/lexer.rs`, `crates/rt/src/token.rs` | newline tokens, keyword map, ident gotchas | **porting source** |
| lexer | `.dev/reference/go/src/go/scanner/` | handwritten stdlib scanner shape | study |
| lexer | `.dev/reference/llvm-project/clang/lib/Lex/` | keyword tables, performance tricks | study |
| parser | `crates/rt/src/parser.rs` | the `.wo` grammar, brace-depth skip-on-block | **porting source** |
| parser | `.dev/reference/go/src/go/parser/` | recursive descent, error-recovery sync points | study |
| parser | `.dev/reference/llvm-project/clang/lib/Parse/` | recovery at scale | study |
| ast / dump | `.dev/reference/go/src/go/ast/`, `crates/rt/src/ast.rs` | node + span design | study / porting source |
| diag | `.dev/reference/llvm-project/clang/include/clang/Basic/Diagnostic*.td` | stable error codes, severities, notes attached to errors | study |
| types | `.dev/reference/go/src/go/types/` | stdlib-only structural typechecker — the closest cousin to `woc types` | **primary study** |
| types | `.dev/reference/llvm-project/clang/lib/Sema/` | protocol-conformance checking (interface-satisfaction analogue) | study |
| owner | `runtime/src/borrow.c`, `runtime/src/gc.c`, spec §4 | the semantic target the four owner tables must satisfy | **contract** |
| owner | Hylo/Val mutable-value-semantics papers (external, not vendored) | second-class borrows theory | study |
| emit | `docs/plan/oop-vm/00-wob-format.md`, `runtime/src/wob.h` | the target format | **contract** |
| emit | `runtime/test/wob_build.c` | the second, independent encoder — the model for emit's section writing | **porting source** |
| emit | `runtime/src/loader.c` | the validation battery every image must pass | **contract** |
| pipeline | `.dev/reference/go/src/cmd/compile/README.md` | how a production compiler documents its pass pipeline | study |
| query layer (plan 5, deferred) | `prototypes/wo-db/`, `.dev/reference/postgresql/src/backend/parser/` | SQL/Cypher grammar semantics | deferred |

Deliberately excluded: `.dev/reference/colibri`, `.dev/reference/llama-cpp`,
`.dev/reference/linux` — runtime/kernel references, not compiler material.

## 7. Governing docs

- Spec: [`docs/superpowers/specs/2026-08-01-oop-compiler-vm-design.md`](../../superpowers/specs/2026-08-01-oop-compiler-vm-design.md)
- Plan 2 — compiler front: [`2026-08-01-woc-compiler-front.md`](./2026-08-01-woc-compiler-front.md)
- Plan 3 — emit + e2e + single binary: [`2026-08-01-wob-emit-e2e-single-binary.md`](./2026-08-01-wob-emit-e2e-single-binary.md)
- Plan 8 — Haxe-parity language surface: [`2026-08-01-haxe-parity-language.md`](./2026-08-01-haxe-parity-language.md)
- Format contract: [`docs/plan/oop-vm/00-wob-format.md`](../../plan/oop-vm/00-wob-format.md)
- VM counterpart (shipped): [`docs/superpowers/plans/2026-08-01-wob-format-and-vm-core.md`](../../superpowers/plans/2026-08-01-wob-format-and-vm-core.md)

> **Docs-location note:** compiler plan documents live here in
> `docs/plan/compiler/` — a recorded exception to the repo's "documentation under
> `docs/`" rule (see CLAUDE.md and `docs/08-project-structure.md`).
