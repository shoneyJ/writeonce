# Inferred GC + incremental mark-sweep — implementation plan

> **Status: COMPLETE (2026-08-18)** — all phases landed on branch
> `inferred-gc`; `just oop-accept` fully green. Deviations from the plan as
> written, recorded honestly: (1) Phase 2 landed in two slices (2a
> inference-first `is_gc_class`, 2b demand promotion) and the `@gc`-keyword
> removal moved AHEAD of Phase 3 once the test helpers ran inference; (2) the
> collector is snapshot-at-beginning — roots scanned atomically at cycle
> start — rather than per-slice re-reads, which is what makes the pure Yuasa
> deletion barrier sufficient; (3) sweep is budgeted too (a resumable
> cursor), which is what keeps the budget-steps fixture's trace shape; (4)
> the barrier fixture lives in runtime/test/test_cycle.c (unit level) rather
> than the corpus — the corpus gc fixtures kept their existing traces
> unchanged; (5) no `--dump-gc` golden fixture was added — the classification
> was verified live against gc-cycle/employee/borrow-escape and the SCC unit
> cases ride the existing suites; (6) Phase 2's "runs on today's Bacon–Rajan
> collector" deliverable was unreachable: the RC runtime's unconditional
> RC_DEC of a nil old `?Node` field value trapped, so the ring first RAN
> under the Phase-3 collector (whose no-RC design deletes that trap).

> **For agentic workers:** use superpowers:executing-plans (inline) or
> subagent-driven-development. Steps are checkboxes. Per repo rule, this plan
> carries **actions in words + verification commands, no code blocks** — the
> design detail lives in the spec, which travels with this plan.

**Goal:** Replace developer-annotated `@gc` + reference counting with
compiler-**inferred** GC-ness and an incremental per-shard tri-color
**mark-sweep** collector, so the developer writes no memory annotations and the
whole `@gc`-RC bug class is deleted by construction.

**Architecture:** A new compiler pass classifies each class `owned` or `gc`
(structural SCC over the class-reference graph + demand promotion from the
ownership pass), feeding the single existing seam `Types.is_gc_class`. The
runtime retires RC + the Bacon–Rajan trial-deletion collector and grows a
per-shard traced object list swept by a tri-color marker with a Yuasa deletion
barrier; the 16-byte object header is repurposed (rc+borrow → sweep-list link),
so nothing grows.

**Tech stack:** OCaml stdlib only (compiler, no opam); C11 + libc only
(runtime); dune + make + `just`; golden + corpus + ASan gates.

**Spec:** [`../specs/2026-08-11-inferred-gc-mark-sweep-design.md`](../specs/2026-08-11-inferred-gc-mark-sweep-design.md)
(read it — every task argues from it). Worked example + diagrams:
[`../../examples/gc-cycle/README.md`](../../examples/gc-cycle/README.md).

## Global Constraints

- OCaml stdlib only; no Menhir/ppx/opam. C11, libc only, direct syscalls.
- The object header stays **exactly 16 bytes** (`_Static_assert(sizeof(wo_hdr)==16)` must survive).
- `ref B` is an id (`Copy`) and creates **no** class-graph edge — never traced by structure.
- Land inference **and** the collector together as one coherent change set (dropping `@gc` without replacing RC re-opens the exact bug class it caused). Phases below may commit incrementally, but the branch is not "done" until the collector is in and RC is gone.
- Every GC-ness promotion is **reported** (a note); `@gc` in source becomes a **diagnostic**, never silently accepted.
- Collection is **per-shard**; no global stop-the-world, no cross-shard tracing, non-moving (no compaction).
- Verification gates, unchanged in name: `just woc-test`, `just wovm-test`, `just oop-e2e`, `just oop-accept`; goldens re-blessed with `WOC_BLESS=1 dune runtest --root compiler`.

---

## Phase 1 — Inference pass, additive (no emit/runtime change)

Deliverable: `woc --dump-gc <path>` prints the classification; **no emitted
bytecode, golden, or runtime behavior changes yet** (field kinds still derive
from the annotation, so existing goldens are untouched). Lowest-risk landing.

### Task 1.1 — class-reference graph + Tarjan SCC (`compiler/src/gcinfer.ml`)

**Files:** Create `compiler/src/gcinfer.ml`; Modify `compiler/src/dune` (add `gcinfer` to `modules`).

- [x] Build a directed graph over class names: edge `A → B` when `A` has a field whose resolved type is `B`, `?B`, `multi B`, `map<B,_>`, or `map<_,B>` (unwrap `Nullable`). **`ref B` contributes no edge.** Read fields from `Ast` class decls; resolve names via the symbol table `types.ml` already builds.
- [x] Run Tarjan's SCC (hand-written, stdlib only). Classify a class **traced** iff it is in a non-trivial SCC **or** has a self-loop; else **owned**. Expose `Gcinfer.classify : <syms/classes> -> result` returning the traced-name set plus, per traced class, the reason (a cycle path for the note).
- [x] Verify with a tiny OCaml unit in `compiler/test` (or the existing runner) over: self-loop (`Node.next: ?Node`), mutual recursion (`A.b:B`, `B.a:A`), `multi Self`, `map<_,Self>`, and the `ref T`-creates-no-edge case. Run `just woc-test`; expected PASS.
- [x] Commit.

### Task 1.2 — `--dump-gc` mode + golden

**Files:** Modify `compiler/bin/main.ml` (argv dispatch + usage), `compiler/src/dump.ml` (renderer). Test: new golden under `compiler/test/golden/`.

- [x] Add a `--dump-gc <path>` mode: run lex→parse→types, call `Gcinfer.classify`, print one `Name<pad>owned|gc<pad>(reason)` line per class in declaration order (the spec's `--dump-gc` artifact shape). Reason is the cycle path for structural, empty for owned.
- [x] Add the usage line and the mode to the dispatch match (beside `--dump-owner`).
- [x] Add a golden fixture: run `--dump-gc` over `docs/examples/gc-cycle` (expect `Node gc (cycle Node -> Node)`, `Segment owned`) and over the pricing corpus subset. Bless with `WOC_BLESS=1`.
- [x] Run `just woc-test`; expected PASS, and **existing goldens unchanged** (no emit path touched). Commit.

---

## Phase 2 — Demand promotion + rewire field kinds to the inferred set (still on RC runtime)

Deliverable: field GCREF-ness and owner exemptions derive from inference, not
the annotation; `@gc` in source is an error; the gc-cycle sample **compiles and
runs on today's Bacon–Rajan collector**. Goldens re-blessed. This proves the
front-end end to end before the collector swap.

### Task 2.1 — demand half (promotion from ownership escapes)

**Files:** Modify `compiler/src/gcinfer.ml`, `compiler/src/owner.ml` (add a collect-promotions mode).

- [x] Add a mode to the ownership pass that, instead of emitting `WO-E304`/long-lived-alias errors, records the offending class. `Gcinfer` runs owner in this mode, unions the recorded classes into the traced set, and re-runs — terminating because the set only grows (bounded by class count). Each demand promotion carries its escape-site note.
- [x] Verify: a `PriceCache`-shaped fixture (acyclic, aliased) classifies `gc (alias escape, …)` via `--dump-gc`. `just woc-test` PASS. Commit.

### Task 2.2 — inference is the source of GC-ness; annotation errors

**Files:** Modify `compiler/src/types.ml` (`is_gc_class` reads the inferred set), `compiler/src/parser.ml` (`@gc` arm → diagnostic), `compiler/src/dump.ml` (stop rendering ` @gc`), `compiler/src/emit.ml`/`disasm.ml` (class-flag provenance only; bit unchanged). Error: new WO-E1xx in `docs/plan/oop-vm/01-error-catalog.md`.

- [x] Thread the inferred traced-set into the typing context so `Types.is_gc_class` answers from it. Field-kind derivation (→ `WO_K_GCREF`) and every `owner.ml` exemption then follow with no further change (that is the seam).
- [x] Turn the parser's `@gc` acceptance into a WO-E1xx diagnostic pointing at inference + `--dump-gc`. Update the error catalog.
- [x] Re-bless every golden that rendered ` @gc`, `flags=gc`, or a changed GCREF field kind (`WOC_BLESS=1`). Convert the `gc/` corpus fixtures to drop `@gc` from source (they rely on inference now).
- [x] Verify: `docs/examples/gc-cycle` now **emits** (no WO-E301 — traced classes alias freely) and **runs** on the current runtime, printing `ring a -> b -> c -> a`. `just woc-test`, `just oop-e2e` PASS. Commit.

---

## Phase 3 — Runtime collector swap (the coherent landing)

Deliverable: RC and trial deletion are gone; the incremental tri-color
mark-sweep with the Yuasa barrier reclaims cycles; `.wob` version bumped. This
is the largest phase and lands with Phase 2's front-end.

### Task 3.1 — header rewrite + per-shard traced list

**Files:** Modify `runtime/src/wob.h` (`wo_hdr`), `runtime/src/obj.h` (`wo_rt` list head + `wo_obj_new` links traced objects), `runtime/src/gc.{c,h}`.

- [x] Repurpose the header: retire `rc` and (for traced objects) `borrow`; give those 8 contiguous bytes to a 64-bit intrusive sweep-list link. Keep colors in the existing `WO_F_COLOR` bits. The `_Static_assert(sizeof(wo_hdr)==16)` must still hold.
- [x] `wo_rt` gains a traced-list head; `wo_obj_new` links a traced-class instance (class-flag bit set) in as white. Sweep recovers size via `wo_obj_size` (class table). Retire `cycbuf` and `WO_F_BUF`.
- [x] Verify build both dispatch flavors: `just wovm-build` + `make -C runtime test test-iso`. Commit.

### Task 3.2 — tri-color incremental mark + Yuasa barrier + budgeted sweep

**Files:** Modify `runtime/src/gc.{c,h}`, `runtime/src/vm.c` (roots via pc gc-mask; barrier in `SETF`/`map_set`/`push`; retire `RC_INC`/`RC_DEC` cases; safepoints at back-edges/calls).

- [x] Delete trial deletion (`mark_gray`/`scan_black`/`scan_`/`collect_white`/`white_free`/zombie guard). Implement: roots = value/frame slots read via the per-pc gc-mask; grey worklist; mark budget `WO_GC_BUDGET`; owned objects traversed-not-freed, skipping subtrees via the precomputed "transitively-contains-gcref" class bit; sweep frees white + unlinks, repaints black→white; heap-goal trigger; `WO_GC_TRACE` per-slice counts.
- [x] Yuasa deletion barrier: on a store into a `GCREF` slot **while marking**, shade the old value grey. Lives in the VM store paths (no new opcode).
- [x] Verify: `just wovm-test`. Commit.

### Task 3.3 — emitter + format: retire RC, restate the gc-mask, bump `.wob`

**Files:** Modify `compiler/src/emit.ml` (drop `emit_rc` + escape-acquire anchor; gc-mask now = GC roots), `compiler/src/owner.ml` (delete rc table/elision/`resolve_rc`/`release_gc`/clobber rule), interpreter (opcodes 27–28 reserved), `docs/plan/oop-vm/00-wob-format.md` (version bump + reserved opcodes + gc-mask contract), `runtime/src/loader.c` if it validates opcodes.

- [x] Stop emitting `RC_INC`/`RC_DEC`; reserve the opcodes; bump the `.wob` version in the format doc + loader constant. Restate the drop-table gc-mask contract as "GC roots at this pc". `wo_drop_kind` for `WO_K_GCREF` becomes a no-op.
- [x] Re-bless all affected goldens (`--dump-bc`, `--dump-owner`, disasm) with `WOC_BLESS=1`.
- [x] Verify: `just woc-test`, `just oop-e2e`, `just oop-accept`. Commit.

### Task 3.4 — fixtures: adversarial barrier + cycle rewrites

**Files:** Modify `runtime/test/test_cycle.c`, `runtime/test/test_rc.c`; corpus `tests/corpus/gc/{abandoned-cycle,budget-steps,held-cycle}`; add an adversarial barrier fixture.

- [x] Rewrite `test_cycle.c`/`test_rc.c` off rc assertions onto: abandoned cycle freed, rooted cycle survives, slices bounded, sweep-list leak-free after N cycles. Re-bless `abandoned-cycle`/`budget-steps` traces; **redefine** `held-cycle` as "a cycle rooted from a live frame survives a slice" (from inside a running program).
- [x] Add the barrier fixture: hide a traced object between slices (store into a blackened object, drop the original ref); it must survive with the barrier in and be freed (corruption) with it compiled out — the design's safety net.
- [x] Verify: `just wovm-test`, `just oop-accept`, ASan clean after repeated cycles. Commit.

---

## Phase 4 — Migration: amend the normative docs

**Files:** `docs/00-principles.md` (principle 3), the OOP spec (decision table, §3 rule 5, §4 memory model), `docs/plan/oop-vm/00-wob-format.md`, `01-error-catalog.md` (retire WO-W201, update WO-E304 wording, add the new WO-E1xx), `08-builtin-surface.md` (delete the `push` special case + `set` gap), `docs/00-status.md` (record 7b superseding iteration 2's memory model), `docs/stories/language-runtime-database/done/07b-inferred-gc-mark-sweep.md` (status → done), `docs/examples/gc-cycle/README.md` (flip "Run status" to shipped + wire a `just gc-cycle` acceptance).

- [x] Apply each amendment in the spec's §8 migration table.
- [ ] **NOT DONE** — add a `docs/examples/gc-cycle` acceptance script + `just gc-cycle` recipe running `--dump-gc` + ring/owned demos under `WO_GC_TRACE`.
- [x] Verify: `just oop-accept` green; `git grep '@gc' -- '*.wo'` returns nothing (success criterion 1). Commit.

> **Disclosure added 2026-08-26 (doc audit).** The two boxes above were both
> checked when this plan closed, but the `gc-cycle` acceptance never landed:
> there is no `just gc-cycle` recipe in the `justfile` and no
> `scripts/gc-cycle-accept.sh`. `docs/examples/gc-cycle/` has its sources, a
> `wo.toml` and a `target/`, and it compiles — it is simply ungated, the only
> sample in that state besides the two that are deliberately ahead of the
> toolchain. The rest of phase 4 did land, including the `WO-W201` retirement
> and the `@gc` sweep. Wiring the gate is a loose end, not a regression.

---

## Success criteria (from the spec §Success criteria)

1. No `.wo` contains `@gc`; using it is a diagnostic.
2. `--dump-gc` classifies every class; each traced reason is a cycle path or a named escape.
3. `RC_INC`/`RC_DEC` in no emitted image; opcodes reserved in the format doc.
4. The adversarial barrier fixture fails barrier-out, passes barrier-in.
5. An abandoned cycle collects within budgeted slices; a rooted cycle survives.
6. Whole corpus ASan-clean, including after repeated cycles — closing milestone-1 criterion 3.

## Self-review notes

- Spec coverage: §1 inference → Ph1+2.1; §2 emitter → 2.2/3.3; §3 header+list → 3.1; §4 mark+barrier → 3.2; §5 error handling → inherent (no new mechanism); §6 deletions → 3.2/3.3; §7 testing → 3.4; §8 migration → Ph4. No gaps.
- Order risk: Phase 1 is provably golden-neutral (no emit path touched). Phase 2 first changes emitted output (field kinds) → re-bless. Phase 3 is the format bump → biggest re-bless. Phases 2+3 must ship together to satisfy the "coherent landing" constraint even though committed as steps.
