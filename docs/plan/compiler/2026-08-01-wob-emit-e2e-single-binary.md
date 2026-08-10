# Bytecode Emit + End-to-End Corpus + Single Binary Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
>
> **Style rule (user convention):** this plan states concept, reason, and required behavior in words. The executor writes the actual code at implementation time; nothing here is copy-paste source.

**Goal:** Close milestone 1: `woc` emits `.wob` bytecode that `wovm` executes, a three-kind conformance corpus proves the whole spec's semantics end to end, and `woc build` produces the single self-contained binary — then the spec's five success criteria are checked off as the acceptance gate.

**Architecture:** Plan 3 of 3 for `docs/superpowers/specs/2026-08-01-oop-compiler-vm-design.md`. Depends on plan 1 (`runtime/` wovm — loader, interpreter, memory model) and plan 2 (`compiler/` front — typed AST plus the four owner tables). This plan adds the emitter module to `compiler/`, the conformance corpus and its runner at the repo root (`tests/corpus/`), and the packaging path. The corpus is the spine: every language semantic lands as a fixture with an expected outcome, and the same harness carries forward to sub-projects 2–5.

**Tech Stack:** OCaml stdlib (emitter), C (small wovm additions: gc pump, self-exec trailer), bash + just (harness), ASan gate from plan 1.

## Global Constraints

- All plan-1 and plan-2 global constraints carry over verbatim (libc-only C, stdlib-only OCaml, no commits — drafts to `.dev/commit.md`, docs under `docs/`).
- **The format doc governs:** every emitted byte follows `docs/plan/oop-vm/00-wob-format.md`; any needed format change is a stop-and-ask, not a local invention.
- **Every emitted module must pass the plan-1 loader's validation** — a `woc`-produced image rejected by `wovm` is always an emitter bug (round-trip rule).
- **Register budget:** methods needing more than 64 registers are a compile-time diagnostic (WO-E4xx range for emitter limits), never a truncation.
- **Corpus outcomes are exact:** expected stdout byte-for-byte, expected `WO-E###` code, or expected trap code — no substring-ish matching.
- **Spec success criteria are the acceptance gate** (spec "Success criteria" 1–5); the final task runs all five.

---

## File Structure

```
compiler/src/
  emit.ml               lowering + register allocation + .wob serialization (Task 1)
  disasm.ml             .wob disassembler backing --dump-bc goldens (Task 1)
tests/corpus/
  run/                  fixture.wo + fixture.out (expected stdout)        (Tasks 2–3)
  compile-fail/         fixture.wo + fixture.code (expected WO-E###)      (Task 4)
  trap/                 fixture.wo + fixture.trap (expected trap code)    (Task 4)
  gc/                   cycle fixtures with budget expectations           (Task 5)
scripts/oop-e2e.sh      corpus runner (invoked by just oop-e2e)           (Task 2)
runtime/src/main.c      gc pump + self-exec trailer detection             (Tasks 5–6)
compiler/bin/main.ml    emit mode, woc build packaging                    (Tasks 1, 6)
docs/plan/oop-vm/       02-corpus.md (how to add fixtures)                (Task 2)
justfile                oop-e2e, oop-accept recipes                       (Tasks 2, 8)
```

---

### Task 1: The emitter

**Files:** create `compiler/src/emit.ml`, `compiler/src/disasm.ml`; modify `compiler/bin/main.ml` (emit mode, `--dump-bc`); golden fixtures; a round-trip check against the plan-1 loader.

**Concept & reason:** lower the typed, owner-annotated AST into `.wob` per the format doc. The pieces, each stated as a requirement:

- **Register allocation:** a simple scope-stack allocator — parameters first (the window convention fixes their slots), locals on declaration, expression temporaries from a high-water pool, freed on statement end. Over-budget methods (>64) diagnose, never truncate.
- **Calls:** the window convention — arguments placed at consecutive registers, callee index for direct calls, global slot id for interface calls (from the plan-2 satisfaction sets, which also serialize as the vtable section).
- **Ownership lowering** consumes the four plan-2 tables literally: real transfers become plain moves (the VM treats MOVE as the move); scope-end drop sets place DROP ops including on early-return paths; rc sites emit RC_INC/RC_DEC except where marked elided; residual sites — and only residual sites — emit borrow/release ops around the region. Zero-cost-when-provable is the spec's core promise: a golden fixture must show a fully-proven method emitting no borrow or rc ops at all.
- **Drop maps and line tables:** at every call- or trap-capable pc, the live owned/gc registers serialize as the drop-table masks; source lines serialize per the format. This is what makes plan-1's "traps never leak" hold for compiled code.
- **Constants and classes:** deduplicated constant pool; class table with the derived field kinds; `DB_STUB` for DbStub nodes; builtins lowered to the BUILTIN ids of the format doc.
- **Terminator rule:** every method's code ends in a terminator (the loader rejects otherwise — the emitter appends the implicit void return where control can fall off).
- **Disassembler:** renders a `.wob` back to readable mnemonics for `--dump-bc` goldens — pinned dumps are how emitter regressions surface before the corpus even runs.

- [ ] Failing goldens: disassembly of an arithmetic method, a method with owned locals (visible DROPs + drop-table rendering), an elision fixture (no borrow/rc ops), a residual fixture (borrow ops present), an interface fixture (vtable section rendered).
- [ ] Implement; goldens green. New `WO-E4xx` emitter-limit codes (register over-budget) are appended to `docs/plan/oop-vm/01-error-catalog.md` in the same change — the catalog stays complete.
- [ ] Round-trip gate: every corpus-bound fixture emitted so far loads clean in `wovm` (build plan-1's runtime if not built).
- [ ] Record commit draft: `feat(compiler): .wob emitter — scope-stack register allocation with 64-cap diagnostic, window calls + vtable serialization, ownership lowering from owner tables (moves/drops/rc-elision/residual-only borrow ops), drop maps + line tables, dedup const pool, DB_STUB, implicit terminators; disasm.ml for --dump-bc goldens; loader round-trip gate.`

### Task 2: Conformance harness

**Files:** create `scripts/oop-e2e.sh`, `tests/corpus/run/` seeds, `docs/plan/oop-vm/02-corpus.md`; modify `justfile`.

**Concept & reason:** the spec's testing spine, mechanized. The runner walks the three corpus kinds and enforces exact outcomes: `run/` fixtures compile with `woc`, execute with `wovm`, and their stdout must equal the `.out` file byte-for-byte; `compile-fail/` fixtures must fail compilation with exactly the `WO-E###` named in their `.code` file; `trap/` fixtures must exit 1 with the trap code named in their `.trap` file parsed from wovm's fixed stderr line. Any other outcome — wrong code, unexpected success, loader rejection — is a failure naming the fixture. The runner prints a one-line-per-fixture summary and a final tally; `just oop-e2e` wires it. The corpus doc explains how to add a fixture of each kind (the contribution path for every later sub-project). Seeds: a hello (print/print_int), arithmetic + control flow, a method-call fixture, and an interface-dispatch fixture.

- [ ] Failing: runner exists, seeds in place, runs against the Task-1 emitter — seed fixtures green or their failures fixed.
- [ ] Record commit draft: `feat(tests): conformance harness — three-kind corpus (run/compile-fail/trap) with exact-outcome matching, just oop-e2e, corpus contribution doc; seed fixtures (hello, arithmetic, methods, interface dispatch).`

### Task 3: Pricing-demo corpus

**Files:** add `tests/corpus/run/` and `tests/corpus/trap/` fixtures derived from `docs/examples/pricing/`.

**Concept & reason:** the spec names the pricing demo's logic subset as the milestone-1 workload — it becomes executable truth here. Fixtures: the pure `discounted` computation; `current_price` through a `multi` with `latest`; container round-trips (multi push/count, map set/get over SKU keys); text handling (`words`, concat); and `set_price` — whose `insert` lowers to DB_STUB — as a trap fixture expecting the DB code (the spec's parse-but-trap story, proven end to end). Where the original demo files use surface not in milestone 1, the fixture carries the minimal adaptation with a comment naming what was trimmed — the corpus never silently diverges from the sample it mirrors.

- [ ] Add fixtures; corpus green; ASan-built wovm run of the whole corpus stays clean.
- [ ] Record commit draft: `test(corpus): pricing-demo logic subset — discounted, current_price via multi/latest, container + text builtins, set_price DB_STUB trap fixture.`

### Task 4: Ownership + trap corpora

**Files:** add `tests/corpus/compile-fail/` and `tests/corpus/trap/` fixtures.

**Concept & reason:** spec success criterion 3, verbatim: every must-fail program fails with its expected code, every must-trap program traps with its expected code, ASan reports nothing. Compile-fail seeds mirror the plan-2 ownership suite as end-user programs (move-after-use, borrow escape via return, double `mut` on a provable alias, plus a type error and an unsatisfied interface for the E2xx range). Trap seeds exercise the runtime's residual checks through compiled code: aliased `mut` through runtime indices (the canonical residual — traps BORROW), missing map key (KEY), division by zero (DIV0, and the error line must match the fixture's marked source line, proving line tables survive emission). The distinction this task pins: provable violations fail at compile time, unprovable ones trap at runtime — the hybrid boundary made testable.

- [ ] Add fixtures; corpus green under the ASan gate; the div0 fixture asserts the reported line.
- [ ] Record commit draft: `test(corpus): ownership compile-fail suite (E3xx as user programs) + runtime trap suite (residual mut-alias BORROW, map KEY, DIV0 with line assertion) — the hybrid compile/runtime boundary pinned.`

### Task 5: @gc cycle collection end to end

**Files:** add `tests/corpus/gc/` fixtures; modify `runtime/src/main.c` (gc pump).

**Concept & reason:** spec success criterion 4 needs an observable collector in a real program. wovm gains a minimal gc pump: after the entry method returns, it drives collection steps until the candidate buffer empties, with the per-step budget from a `WO_GC_BUDGET` environment variable (defaulting sensibly); a `WO_GC_TRACE` variable makes each step print freed/visited counts to stderr — the observable the fixtures assert. Fixtures: a `@gc` cycle built and abandoned in `.wo` (collected — ASan proves the frees); an externally-held cycle (survives); and a budget fixture (trace shows multiple bounded steps rather than one unbounded sweep — "no pause longer than the configured slice" made visible). This pump is deliberately minimal: the real scheduler-integrated pacing belongs to sub-project 2; the interface (budgeted step calls) is already the plan-1 collector's.

- [ ] Failing fixtures; implement the pump; green + ASan-clean.
- [ ] Record commit draft: `feat(runtime)+test(corpus): wovm gc pump (WO_GC_BUDGET steps after entry, WO_GC_TRACE observability); gc fixtures — abandoned cycle collected, held cycle survives, budget slicing visible.`

### Task 6: Single binary — `woc build`

**Files:** modify `compiler/bin/main.ml` (build mode), `runtime/src/main.c` (self-exec detection); smoke additions to the harness.

**Concept & reason:** spec success criterion 5 and the language's one-binary promise. `woc build <dir> -o app`: compile, then copy the `wovm` executable and append the `.wob` image plus a fixed-size trailer (magic + payload offset/length). wovm startup order becomes: read its own executable (via /proc/self/exe), check for the trailer — if present, load the embedded image and ignore argv; otherwise require the `.wob` path argument as today. Locating `wovm` to copy: an explicit `--runtime` flag wins, else a repo-relative default; a missing runtime binary is a clear error telling the user to build it. Smoke: build the hello fixture into a single file, move it to a temp directory (proving self-containment), run it with no arguments, diff output; corrupt the trailer and confirm the clear failure mode.

- [ ] Failing smoke; implement both halves; green.
- [ ] Record commit draft: `feat: woc build single binary — wovm copy + appended .wob + trailer, self-exec detection via /proc/self/exe with argv fallback; relocation smoke + corrupt-trailer failure mode.`

### Task 7: Parity harness against the Rust runtime

**Files:** create `scripts/oop-parity.sh`; a small overlap manifest in `tests/corpus/`.

**Concept & reason:** the spec's cheap insurance — where milestone-1 semantics overlap the shipped 13b method executor in `crates/rt`, both stacks must agree until the Rust runtime retires. The harness takes the manifest of overlap fixtures (pure method logic: arithmetic, text, control flow — no containers or interfaces, which 13b lacks), runs each through the new stack directly, and through the Rust runtime by starting `wo run` against a fixture-derived project and invoking the method over its existing RPC route, then compares results. Non-overlapping features are out of manifest by construction, not skipped at runtime. This stays a separate opt-in recipe (`just oop-parity`) — it needs a cargo build and a port, too heavy for the per-change gate.

- [ ] Implement harness + manifest with the overlap fixtures; run once green; document the manifest criteria in the corpus doc.
- [ ] Record commit draft: `test: parity harness — overlap manifest run on both stacks (wovm direct vs crates/rt 13b RPC), just oop-parity opt-in recipe.`

### Task 8: Acceptance gate + docs closeout

**Files:** modify `justfile` (`oop-accept`), `CLAUDE.md`, `compiler/README.md`, `runtime/README.md`, `docs/plan/00-kanban.md`; the spec gets its criteria checked.

**Concept & reason:** run milestone 1's definition of done as one command. `just oop-accept` executes, in order: compile-time measurement of the pricing subset (must be < 100 ms — criterion 1); the full conformance corpus under ASan including gc fixtures (criteria 2–4); the single-binary smoke (criterion 5); plus plan-1's `wovm-test` and plan-2's `woc-test` full gates. Docs closeout: CLAUDE.md gains the three-directory story (compiler/, runtime/, corpus) and the recipes; both READMEs cross-link; the kanban records the milestone. Anything failing here is a defect in an earlier task — this task adds no functionality, only the gate and the paper trail.

- [ ] Wire the recipe; run it; fix nothing here — route failures back to their tasks.
- [ ] Sync docs; check the five criteria off in a dated note appended to the spec.
- [ ] Record commit draft: `chore: oop-accept acceptance gate (5 spec criteria + both unit gates in one recipe); docs closeout — CLAUDE.md three-directory story, README cross-links, kanban milestone note, spec criteria checked.`

---

## Plan self-review notes

- **Spec coverage:** every success criterion has a task (1→Task 8 measurement, 2→Tasks 2–3, 3→Task 4, 4→Task 5, 5→Task 6); ownership-lowering zero-cost promise pinned by Task 1 goldens; parse-but-trap proven in Task 3; hybrid boundary pinned in Task 4; parity per spec's "later, cheap" in Task 7.
- **Dependency honesty:** Tasks 1–8 need plans 1 and 2 complete. Task 5 and 6 modify `runtime/src/main.c` — small, contained additions to plan-1 code, called out rather than hidden.
- **Known accepted simplifications, documented in their tasks:** gc pump is post-exit stepping (scheduler pacing is sub-project 2); parity manifest excludes features 13b lacks by construction; single-binary trailer is append-based (no ELF section games).

## Execution note

Execution order across plans: plan 1 (C runtime) and plan 2 (OCaml front) are independent of each other; plan 3 requires both. Nothing in this plan runs today — documents only, per the user's instruction.
