# 08 — Project structure: the writeonce monorepo

Canonical map of the repository: what every root directory is and who writes to
it. Companion to [`CLAUDE.md`](../CLAUDE.md) (working rules), the status board
([`00-status.md`](stories/00-status.md)), and the story arc
([`stories/language-runtime-database/00-story.md`](stories/language-runtime-database/00-story.md)).

writeonce is **one compiled language, one runtime, one embedded database, one
binary**: OCaml `woc` compiles `.wo` source to a `.wob` image, the C `wovm`
runs it, and `woc <dir>` bakes the two into a single standalone executable. The
prior Rust `wo` runtime was removed 2026-08-18 (see
[`plan/discarded.md`](plan/discarded.md)); the tree below is only the current
project.

## The one-page map

```
writeonce-all/
├── compiler/     OCaml `woc` — lexer→parser→types→gcinfer→owner→emit; produces the compiler binary
├── runtime/      C `wovm` — the register VM that runs .wob images (src/); retired io_uring reference (wo-rt.c, bench/)
├── database/     C embedded engine — class-shaped tables, secondary indexes, typed WAL + recovery
├── tests/        corpus/ — conformance fixtures: run / compile-fail / trap / gc (+ five reserved, still empty)
├── scripts/      the corpus runner, packaging, linkcheck, and one acceptance script per sample
├── bench/        baseline.json (the db-bench gate's thresholds) + results/ + compare/ (Go+SQLite peer)
├── docs/         ALL documentation: numbered docs, stories/, plan/, examples/, guides/, superpowers/
├── dist/         `just dist` output: writeonce-<ver>-linux-amd64.tar.gz + .sha256
├── .github/      workflows/release.yml — builds, verifies and publishes on a `v*` tag push
├── .claude/      agents/ — project subagent definitions (see docs/guides/database-developer-subagent.md)
├── .dev/         gitignored per-developer links + reference study trees (v1 crates, colibri, llama-cpp)
├── justfile      task runner: woc-/wovm-build, the *-test gates, oop-accept, dist, install-accept
├── VERSION       single-sourced toolchain version (stamped into woc/wovm; asserted by `just dist`)
└── README.md     the getting-started front door (also the writeonce.de landing content)
```

`target/` and `.vscode/` are local build and editor state, not part of the
project layout.

## Root directories in detail

### `compiler/` — the OCaml `woc` compiler

```
compiler/
├── dune-project
├── README.md            orientation: pipeline map, build/test commands
├── src/                 one module per stage: diag, token, lexer, ast, parser,
│                        types, gcinfer, owner, emit, disasm, dump
├── bin/main.ml          the woc executable — check / build-from-manifest / --emit /
│                        build / version / --update-deps / the --dump-* modes / -D
└── test/                golden runner + golden/ fixtures per stage (tokens, ast,
                         owner, owner-err, bc) + fixtures/driver/ CLI-smoke cases
```

The compiler-track plan docs live under
[`plan/compiler/`](plan/compiler/architecture.md) in this `docs/` tree, not
inside `compiler/` — the repo rule below applies to the compiler like everything
else.

Doctrine: OCaml stdlib only — no Menhir, no ppx, no opam libraries; handwritten
lexer and recursive-descent parser. Build: `just woc-build`; gate:
`just woc-test`. Architecture map:
[`plan/compiler/architecture.md`](plan/compiler/architecture.md).

### `runtime/` — the C `wovm` VM

The register interpreter that loads and runs `.wob` images, plus the arena,
inferred GC, and the systems stdlib (`fs`/`time`/`env`/`net`/`proc`/`json`).
`runtime/src/main.c` also reads an image embedded in its own binary
(`/proc/self/exe`), which is how `woc build` produces a standalone executable.
`wo-rt.c` + `bench/` are the phase A–F io_uring event-loop reference that fed
the design (see [`plan/exploration/c-runtime/00-plan.md`](plan/exploration/c-runtime/00-plan.md)).

Doctrine: C11, libc only, direct syscalls; computed-goto dispatch with
`-DWO_ISO_C` fallback; ASan/UBSan gates. Build: `just wovm-build`; gate:
`just wovm-test` (both dispatch flavors + `cli_smoke`).

### `database/` — the embedded engine

Statically linked into every `wovm` (and every test binary) — one binary, no
separate database process. Class-shaped row slabs, an open-addressing id hash,
secondary-index multimaps with `@unique` enforcement, foreign-key restrict, and
a typed write-ahead log (`len|crc|payload|mark`, ack-after-fsync, torn-tail
drop, boot replay). Every class is a table; durability is opt-in via `WO_DATA`.
See `database/src/CODE-LOGIC.md`. The query surface that drives it lives in the
compiler (`emit.ml`), lowered to engine builtins — no SQL text in the image.

### `tests/`, `scripts/`

- `tests/corpus/` — the conformance spine. Four directories carry fixtures:
  `run/` (60), `compile-fail/` (46), `trap/` (5), `gc/` (2). Five more —
  `actor/`, `db/`, `lang/`, `sys/`, `sample-logwatcher/` — are reserved slots
  from the original plan and still **empty**; see
  [`tests/corpus/README.md`](../tests/corpus/README.md) for which plan each was
  to be filled by. Exact-outcome matching: byte-equal stdout, exact `WO-E###`,
  exact trap code. Driven by `scripts/oop-e2e.sh` (`just oop-e2e`).
- `scripts/` — the corpus runner (`oop-e2e.sh`) and
  `single-binary-smoke.sh`; packaging (`mkdist.sh`, `install-accept.sh`); the
  docs gate (`linkcheck.py`); the benchmark campaign (`db-bench.py`); and one
  acceptance script per sample — `employee-accept.sh`, `log-watcher-accept.sh`,
  `web-app-accept.sh`, `site-accept.sh`, `fibers-accept.sh`,
  `db-actor-accept.sh`, `deps-accept.sh`.

### `docs/` — all documentation

```
docs/
├── 00-*.md, 01-problem.md, 08-*.md principles / code-review / dependency-graph /
│                                   link-audit / doc-audit / problem / structure
├── stories/                        00-status.md (the board) + board-views.md +
│                                   the canonical iteration arc (language-runtime-database/,
│                                   FLAT — status lives in each story's frontmatter)
├── active-slice-*.md               the live slice's one marker doc, deleted when it lands
├── guides/                         runbooks: releasing, language-surface, subagents
├── examples/                       13 sample projects, 8 of them wired to a `just` recipe
├── plan/                           compiler/ plans, oop-vm/ contracts, exploration/ studies,
│                                   perf-targets.md, discarded.md + learnings.md registers
└── superpowers/                    specs/ (approved designs) + plans/ (implementation plans)
```

Repo rule: documentation belongs here; code directories keep one orientation
README each.

### `.dev/` — developer-local + reference

Gitignored per-machine tooling state plus `reference/` study trees: the v1
`wo-*` crates workspace, `colibri`/`llama-cpp` vendored studies, and
`linux/`/`go/` source symlinks. Read-only; nothing here is built by the main
gates.

## Build & test flow

`just woc-build` + `just wovm-build` produce the two binaries; `just oop-accept`
runs the full milestone gate (compile-time budget, conformance corpus under
ASan, single-binary smoke, both unit gates). Sample acceptance: `just employee`
(database), `just log-watcher` (systems stdlib), `just web-app` and `just site`
(the framework consumed through `[deps]`), `just fibers` and `just db-actor`
(the concurrency arc), `just deps-accept` (the package manager),
`just db-bench` / `db-bench-quick` (the benchmark campaign, gated against
`bench/baseline.json`). Docs gate: `just linkcheck`. Packaging: `just dist` →
`writeonce-<ver>-linux-amd64.tar.gz`, proven by `just install-accept`;
publishing is `.github/workflows/release.yml` on a `v*` tag
(see [`guides/releasing.md`](guides/releasing.md)).

## Naming conventions

- Binaries: `woc` (OCaml compiler), `wovm` (C VM); a `woc build` / `woc <dir>`
  output is named by the project's `wo.toml`.
- Story iterations: `<NN>-<topic>.md`, flat in
  `docs/stories/language-runtime-database/`. **No directory encodes status**
  (directive 2026-08-26) — each story's `status:` frontmatter key is the only
  place state is recorded, so a status change is a one-line edit and never
  moves a file or breaks a link.
- Plan/spec files: `YYYY-MM-DD-<topic>.md` under `docs/superpowers/{specs,plans}/`;
  compiler plans under `docs/plan/compiler/`; normative contracts under
  `docs/plan/oop-vm/`.
- A project's dependencies (iteration 15): `wo.toml [deps]` declares
  exact-rev git deps; they fetch to `.wo-deps/<name>/` (gitignored) and pin
  in `wo.lock` (committed).
- Sample projects live under `docs/examples/<name>/` with their own `wo.toml`
  and module `justfile`.
