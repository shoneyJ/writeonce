# compiler/ — the OCaml `woc` compiler

Lexer → parser → typechecker → ownership pass → bytecode emitter, for `.wo`. OCaml stdlib only (no Menhir, no ppx); dune is the build runner. Sibling of the C `wovm` bytecode VM ([`runtime/`](../runtime/README.md)) — the two halves of the OOP track's spec (`docs/superpowers/specs/2026-08-01-oop-compiler-vm-design.md`) meet at plan 3, where `woc`'s emitted `.wob` runs on `wovm`.

**Stage: plan 3 (`docs/plan/compiler/2026-08-01-wob-emit-e2e-single-binary.md`) complete, Tasks 1–6 + 8** (Task 7, a parity harness against the Rust runtime, was deferred by explicit decision — the two stacks now diverge by design). `.wo` source compiles to `.wob` bytecode (`--emit`) and to a single self-contained executable (`build`) that runs `wovm` with no arguments and no repo-relative dependency. Milestone 1's acceptance gate — compile-time budget, the full conformance corpus under ASan, the single-binary smoke, both unit suites — is `just oop-accept`. Plan 2 (lexer through ownership pass) shipped first and is unchanged.

## Requirements

OCaml 4.14.1, dune 3.14.0 — Ubuntu 24.04 apt packages (`sudo apt install ocaml dune`), the version floor. Confirm with `ocaml -version` / `dune --version`. No opam packages, no Menhir, no ppx — stdlib only.

## Build, test

```bash
cd compiler && dune build     # -> _build/default/bin/woc
cd compiler && dune runtest   # test_diag unit checks + runner golden/CLI-smoke suite

just woc-build   # same, from the repo root
just woc-test    # same, from the repo root
```

`WOC_BLESS=1 dune runtest` (from `compiler/`) rewrites golden `.expected` files to match current output — use it once, by hand, to seed or intentionally update a fixture.

## Running `woc`

```
woc <path>                       # compile (lex, parse, typecheck, ownership-check); nothing prints on success
woc --dump-tokens <path>         # stdout: one line per lexed token
woc --dump-ast <path>            # stdout: the declaration + body AST, indented
woc --dump-owner <path>          # stdout: the ownership pass's four tables (moves, drops, rc, residual)
woc --dump-bc <path>             # stdout: disassembled bytecode for every emitted method
woc --emit <path> -o <out.wob>   # compile through to a .wob bytecode module, runnable by wovm
woc build <dir> -o <app> [--runtime <path>]
                                  # compile + append the .wob image to a copy of wovm (default
                                  # runtime/wovm, or --runtime) into one self-contained <app>
```

`<path>` is a single `.wo` file or a directory. A directory is discovered recursively for every `.wo` file under it — same contract as `wo run` (`crates/rt/src/lib.rs::discover`): dot-prefixed entries and `target`/`data`/`node_modules` are skipped, results are sorted by path. Every discovered file compiles as one program (declarations in one file resolve for bodies in another, regardless of discovery order); diagnostics from every file and every stage print sorted by `(file, line, col)`. For multi-file `--dump-*` output, each file's dump is preceded by a `=== path ===` header line (`compiler/src/dump.ml`'s `file_header`) — a single-file run never prints one.

Diagnostics render as `file:line:col: severity CODE: message` plus a source excerpt with a caret; every shipped code is cataloged in `docs/plan/oop-vm/01-error-catalog.md`. Exit codes: **0** clean compile, **1** diagnostics reported, **2** usage/IO failure.

## Layout

- `src/` — one module per stage: `diag` (diagnostics, collector, exit-code decision), `token`/`lexer`, `ast`/`parser`, `types` (typechecker), `owner` (MVS ownership pass), `emit` (bytecode emitter, consumes `owner`'s four tables), `disasm` (bytecode disassembler, backs `--dump-bc`), `dump` (stable text dumps for all of the above)
- `bin/` — the `woc` executable: CLI parsing, file discovery, the multi-file/cross-file driver, `--emit`/`build` output
- `test/` — `runner.ml` (golden runner + CLI smoke) and `test_diag.ml` (diag.ml unit checks); `test/golden/<stage>/` holds one-file-per-fixture goldens (`tokens`, `ast`, `owner`, `owner-err`, `bc`); `test/fixtures/driver/` holds the multi-file CLI-smoke fixtures (directory discovery, cross-file symbols, diagnostic ordering) that don't fit the one-`.wo`-file-per-fixture golden shape

Governing docs (all under `docs/`, not here — this file stays an orientation README): spec `docs/superpowers/specs/2026-08-01-oop-compiler-vm-design.md`; plans `docs/plan/compiler/2026-08-01-woc-compiler-front.md` and `2026-08-01-wob-emit-e2e-single-binary.md` (+ `architecture.md`, `nullable-types-implementation.md`, `2026-08-01-haxe-parity-language.md` in the same directory). Format contract: `docs/plan/oop-vm/00-wob-format.md`. Error catalog: `docs/plan/oop-vm/01-error-catalog.md`. Conformance corpus contract (fixture layout `woc`'s golden output feeds into): `docs/plan/oop-vm/02-corpus.md`; source-language builtin surface `woc` accepts: `docs/plan/oop-vm/08-builtin-surface.md`. Runtime sibling: [`runtime/README.md`](../runtime/README.md).
