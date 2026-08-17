# 08 — Project structure: the writeonce monorepo

Canonical map of the repository: what every root directory is, who writes to it, and where it is headed as the OOP + systems tracks land. Companion to [`CLAUDE.md`](../CLAUDE.md) (working rules) and the two track specs ([`superpowers/specs/2026-08-01-oop-compiler-vm-design.md`](superpowers/specs/2026-08-01-oop-compiler-vm-design.md), [`superpowers/specs/2026-08-01-systems-track-design.md`](superpowers/specs/2026-08-01-systems-track-design.md)). Plan documents govern the *target* entries; this doc is the one place the whole shape is visible.

## The one-page map

```
writeonce-all/
├── compiler/            ⏳ OCaml `woc` — lexer→parser→types→owner→emit (plans 2, 8)
├── runtime/             ✅ C runtime — wo-rt.c event-loop reference + ⏳ src/ wovm VM (plans 1, 4–7, 9)
├── crates/              ✅ Rust runtime `rt` + 14 phase-scaffold crates — active until C-stack parity
├── tests/               ⏳ corpus/ — conformance fixtures: run / compile-fail / trap / gc / sys / actor / db
├── client/              ⏳ wo-live.js — the ~20 KB live-patch browser runtime (plan 7)
├── scripts/             ⏳ oop-e2e.sh, oop-parity.sh — corpus + parity harnesses (plan 3)
├── prototypes/          ✅ wo-db C++ query-engine reference; ⚠ wo-rt-c = stale duplicate of runtime/
├── .dev/reference/           ✅ v1 crates workspace, colibri, llama-cpp; linux/ + go/ symlinks (per-dev)
├── docs/                ✅ ALL documentation: numbered design docs, plan/, runtime/, examples/, superpowers/
├── justfile             recipes: rt-c-demo/bench today; woc-/wovm-/oop-accept as plans land
├── Cargo.toml           the Rust workspace root (crates/*; .dev/reference/crates excluded)
└── .dev/                gitignored per-developer links + commit.md draft (see .dev/README.md)
```

✅ exists today · ⏳ created by a named plan · ⚠ cleanup note (below).

## Root directories in detail

### `compiler/` — the OCaml `woc` compiler (target)

Created by plan 2 (`docs/plan/compiler/2026-08-01-woc-compiler-front.md`), grown by plans 3 and 8.

```
compiler/
├── dune-project
├── README.md            orientation: pipeline map, build/test commands
├── plan/                compiler-track docs: architecture.md + plans 2, 3, 8
│                        (recorded exception to the docs-under-docs/ rule)
├── src/                 one module per stage: diag, token, lexer, ast, parser,
│                        types, owner, emit, disasm, dump
├── bin/main.ml          the woc executable (check/emit/build modes)
└── test/                golden runner + golden/ fixtures per stage
```

Doctrine: OCaml stdlib only — no Menhir, no ppx, no opam libraries; handwritten lexer and recursive-descent parser. Architecture map with the marked reference-study paths: [`docs/plan/compiler/architecture.md`](plan/compiler/architecture.md).

### `runtime/` — the C runtime (canonical, partially landed)

The root-level home of the C track. Today it holds the shipped thread-per-core io_uring reference (`wo-rt.c`, phases A–F done — see [`plan/exploration/c-runtime/00-plan.md`](plan/exploration/c-runtime/00-plan.md)) plus its `bench/` and Makefile. The VM and every runtime module land beside it under `src/`:

```
runtime/
├── Makefile             wo-rt today; wovm + test targets as plan 1 lands
├── README.md            orientation only (docs rule: real docs live under docs/)
├── wo-rt.c              UNTOUCHED phase A–F event-loop reference
├── bench/               HTTP bench + goref/ Go comparison server
├── src/                 ⏳ wob.h, obj, borrow, cont, gc, loader, vm, builtin, main   (plan 1)
│                        ⏳ sched, mailbox, shard                                     (plan 4)
│                        ⏳ table, wal, db                                            (plan 5)
│                        ⏳ http, json, router, handlers                              (plan 6)
│                        ⏳ ws, sub, htmlx, assets                                    (plan 7)
│                        ⏳ sys_env, sys_fs, sys_proc, sys_net, sys_time              (plan 9)
└── test/                ⏳ t.h harness, wob_build assembler, test_*.c, cli_smoke.sh
```

Doctrine: C11, libc only, direct syscalls; computed-goto dispatch with `-DWO_ISO_C` fallback; ASan (+TSan from plan 4) gates.

### `crates/` — the Rust runtime (active, retiring on parity)

Unchanged from CLAUDE.md's description: `rt` is the monolithic Stage-2 runtime producing the `wo` binary; the 14 unprefixed siblings (`ql`, `value`, `engine`, `txn`, `db`, `wal`, `sub`, `http`, `gen`, `policy`, `logic`, `service`, `ui`, `app`) are phase scaffolds. **Retirement path:** stays authoritative until the C stack passes the parity harness (plan 3 task 7, plan 6 blog smoke); then moves under `.dev/reference/` the way v1 did. Until that day, nothing here is refactored to accommodate the C track.

### `tests/`, `scripts/`, `client/` — the proof layer (target)

- `tests/corpus/` — the conformance spine (plan 3): `run/`, `compile-fail/`, `trap/`, then `gc/` (plan 3), `db/` (plan 5), `actor/` (plan 4), `sys/` (plan 9), `sample-logwatcher/` (plan 10), `lang/` (plan 8). Exact-outcome matching: byte-equal stdout, exact `WO-E###`, exact trap code.
- `scripts/` — `oop-e2e.sh` (corpus runner), `oop-parity.sh` (Rust-overlap manifest runner).
- `client/` — `wo-live.js`, the hand-written no-framework live-patch runtime (plan 7).

### `prototypes/` — reference implementations (frozen)

- `wo-db/` — the ~2k-line C++ query-layer prototype (SQL + Cypher + document paths). Stays: it is the semantic reference plan 5 cites. `make test` must keep passing.
- `wo-rt-c/` — ⚠ **stale duplicate.** The content moved to root `runtime/` (both are currently git-tracked). Slated for deletion once the user confirms nothing references it; `justfile`/doc references already point at `runtime/`. Nothing new lands here — repo rule: prototypes receive nothing new.

### `.dev/reference/` — read-only study material

- `crates/` — the 13 v1 `wo-*` crates, a nested Cargo workspace (build with `cd .dev/reference/crates && cargo build`). Preserved per the wo-seg migration plan ([`runtime/database/07-wo-seg-migration.md`](runtime/database/07-wo-seg-migration.md)).
- `colibri/`, `llama-cpp/` — vendored study trees (zero-dep C inference engine; MoE runtime) — see [`.dev/reference/colibri/`](.dev/reference/colibri/).
- `linux/`, `go/` — per-developer symlinks to kernel and Go sources (gitignored; recreate per `CLAUDE.md`).
- `rest/` — `.rest` HTTP files driving manual smoke against a running runtime; plan 6's blog smoke scripts the same sequences.

### `docs/` — all documentation

```
docs/
├── 00-*,01,08-*.md      status / principles / problem / structure docs
├── runtime/             the 7-phase database design series + runtime concept refs
├── examples/            log-watcher/, employee/, employee-list/ samples
├── plan/                numbered engineering plans 00–16, linux/ cards, assembly/,
│   ├── exploration/     c-runtime/ (A–F, done), linux/, postgresql/, assembly/
│   └── oop-vm/          ⏳ the OOP-track contracts: 00-wob-format, 01-error-catalog,
│                        02-corpus, 03-shard-actor, 04-db-binding, 05-http-service,
│                        06-ui-live, 07-systems-stdlib
├── superpowers/
│   ├── specs/           the two approved track specs (2026-08-01)
│   └── plans/           implementation plans 1–10 (2026-08-01, prose-only)
└── cm.md               legacy notes
```

Repo rule restated: documentation belongs here; code directories keep one orientation README each.

### `.dev/` — developer-local state

Gitignored symlinks into per-machine AI-tooling state plus `commit.md`, the running commit-message draft (the user commits; agents only append drafts). See `.dev/README.md`.

## Lifecycle: how the shape evolves

| Stage | What changes at the root |
| --- | --- |
| Today | `runtime/` holds wo-rt.c; `crates/rt` serves Stage 2; plans are paper. |
| After plans 1–3 (milestone 1) | `compiler/` + `runtime/src/` + `tests/corpus/` + `scripts/` exist; `woc build` emits single binaries; `prototypes/wo-rt-c` deleted. |
| After plans 4–7 | `runtime/src/` carries shard/db/http/ui modules; `client/` exists; the C stack serves the blog sample end to end. |
| After plans 8–10 | systems stdlib in `runtime/src/sys_*`; `docs/examples/log-watcher/` proves program mode. |
| Parity | `crates/` moves to `.dev/reference/crates-v2/` (naming decided then); the `wo` toolchain name transfers to the C stack; Cargo.toml shrinks accordingly. |

## Build sequence

The order the project completes, with the two parallel windows made explicit:

1. **`runtime/` VM core** — plan 1 (`.wob` format + `wovm`: memory model, loader, interpreter). No dependencies; the format doc it pins is everyone's contract.
2. **`compiler/` front** — plan 2 (`woc`: lexer→parser→types→owner). Independent of plan 1 — *may run in parallel with it*; needs `apt install ocaml dune`.
3. **Emit + end-to-end** — plan 3 (bytecode emitter, conformance corpus, `woc build` single binary, acceptance gate). Needs 1 + 2. **Milestone 1 done here.**
4. Two tracks fork and *can proceed in parallel*:
   - **Server track (sequential within):** plan 4 shard-actor runtime → plan 5 DB engine binding → plan 6 HTTP service layer → plan 7 UI/.htmlx/LIVE.
   - **Systems track (sequential within):** plan 8 Haxe-parity language → plan 9 program mode + stdlib → plan 10 log-watcher sample. Only seam with the server track: the JSON codec shared between plans 6 and 9 (either lands it, noted in both).
5. **Parity + retirement** — parity harnesses green (plans 3/6), then `crates/` (Rust) retires to `.dev/reference/` and the `wo` name transfers to the C toolchain.

Rule of thumb: `runtime → compiler → emit → {shard → db → http → ui} ∥ {language → stdlib → sample} → parity`.

## Naming conventions (recap)

- New runtime crates: unprefixed (`ql`, `db`, …). V1 crates: `wo-` prefixed, in `.dev/reference/crates/`.
- Binaries: `wo` (Rust toolchain today), `woc` (OCaml compiler), `wovm` (C VM); at parity the `wo` name moves to the C toolchain.
- C prototype directory names keep their historical `wo-` prefixes (`wo-db`).
- Plan/spec files: `YYYY-MM-DD-<topic>.md` under `docs/superpowers/{specs,plans}/`; numbered engineering plans under `docs/plan/`.
