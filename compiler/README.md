# compiler/ — the OCaml `woc` compiler

Lexer → parser → typechecker → ownership pass → bytecode emitter for `.wo`. OCaml stdlib only (no Menhir, no ppx); dune is the build runner. Sibling of the C `wovm` bytecode VM (`runtime/`) — the two halves of the OOP track's spec (`docs/superpowers/specs/2026-08-01-oop-compiler-vm-design.md`) meet at plan 3 (`.wob` emission).

**Stage: Task 1 scaffold.** `src/` is currently an empty library and `bin/main.ml` is a CLI stub that only wires the exit-code contract below — no lexing, parsing, or checking happens yet. Tasks 2–8 (`compiler/plan/2026-08-01-woc-compiler-front.md`) fill in diagnostics, lexer, parser, typechecker, and the ownership pass in that order.

## Requirements

OCaml 4.14.1, dune 3.14.0 — Ubuntu 24.04 apt packages (`sudo apt install ocaml dune`), the version floor. Confirm with `ocaml -version` / `dune --version`. No opam packages, no Menhir, no ppx — stdlib only.

## Build, test

```bash
cd compiler && dune build     # -> _build/default/bin/woc
cd compiler && dune runtest   # golden suite (empty until Task 3 adds test/dune)

just woc-build   # same, from the repo root
just woc-test    # same, from the repo root
```

`woc` with no arguments (or the wrong number of arguments) prints usage to stderr and exits 2; given one path argument it exits 0 if the path exists, 2 if it doesn't. Exit-code contract, established now and enforced fully once diagnostics land in Task 2: **0** clean compile, **1** diagnostics reported, **2** usage/IO failure.

## Layout

- `src/` — one module per stage, added as each task lands: diag, token, lexer, ast, parser, types, owner, dump
- `bin/` — the `woc` executable (check / emit / build modes land in later tasks)
- `test/` — golden runner; `test/golden/` holds fixtures per stage (Task 3 onward)
- `plan/` — compiler-track docs: [`architecture.md`](plan/architecture.md) (pipeline, module contracts, reference-study map) + plans 2, 3, 8

Governing docs: spec `docs/superpowers/specs/2026-08-01-oop-compiler-vm-design.md`; plans 2, 3, 8 in `compiler/plan/`. Format contract: `docs/plan/oop-vm/00-wob-format.md`.
