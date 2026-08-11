# writeonce OOP — OCaml compiler + C VM core (milestone 1 design)

**Date:** 2026-08-01
**Status:** approved design, pre-implementation
**Scope:** first sub-project of the OOP-writeonce track — the `woc` compiler and `wovm` VM core

## Motivation

writeonce today is a declarative language executed by the Rust runtime (`crates/rt`, Stage 2 shipped). This track evolves it into an object-oriented language with:

- an **OCaml compiler** (`woc`) — fast compiles, no LLVM,
- a **C runtime VM** (`wovm`) — libc-only, evolving out of the existing `wo-rt-c` C reference,
- **memory-safe object instances**: by default an object behaves like a Rust borrowed value (single owner, checked borrows),
- a **per-class `@gc` override** for reference semantics, collected without stop-the-world pauses,
- the same end product: one binary that is the database, the web API, and the UI, running multithreaded.

## Decisions locked during brainstorming

| Question | Decision |
| --- | --- |
| Fate of Rust runtime | **Evolve `wo-rt-c` into the C runtime.** OCaml compiler targets it. `crates/rt` stays active until parity, then retires to `.dev/reference/` like v1 did. |
| OOP shape | **Keep plan 13 doctrine: no inheritance, no override, no virtual class hierarchies — ever.** OOP = `class` (state + methods) + structural **interfaces** (Go-style) + composition (`ref`/`multi`). |
| Borrow enforcement | **Hybrid.** Compiler proves most sites statically and emits nothing; VM enforces residual sites with borrow-word checks at runtime. |
| GC opt-out granularity | **Per-class annotation** `@gc` — all instances of that class are GC-managed and freely aliased. |
| Execution model | **Register bytecode interpreter first** (computed-goto dispatch). JIT possible later, not now. AOT-to-C rejected (kills hot reload, slow builds). |
| Concurrency model | **Shard-actor with ownership transfer.** Thread-per-core shards, one heap per shard, cross-shard = message send = ownership move. GC is per-shard, so no global pause exists by construction. (Implementation is sub-project 2; milestone 1 reserves header space.) |
| First sub-project | **Compiler + VM core** — proves the novel risk (hybrid borrow VM) before any HTTP/DB integration. |
| Approach | **A — Mutable value semantics + register VM** (see below). Rejected: B "Lua-shaped minimal" (defers the core risk, Menhir dep), C "Rust-lite static regions" (research-grade complexity, recreates Rust ergonomics pain). |

### Approach A in one paragraph

Borrows are **second-class** (Hylo/Val's mutable-value-semantics model): a borrow cannot escape its scope — it cannot be stored in a field or returned. This eliminates full lifetime inference; the compiler needs only per-function flow analysis. Long-lived cross-object links go through `ref T` ids (as writeonce DB rows already do) or `@gc` references. This keeps compiles fast, keeps most code at zero runtime cost, and matches shard-actor ownership transfer exactly.

## Section 1 — Scope and placement

**Milestone 1 delivers:** `woc` (OCaml compiler) + `wovm` (C VM core). Input: pricing-demo-shaped `.wo` classes with methods. Output: `.wob` bytecode module; VM loads it, runs method calls, enforces the memory model. Single shard. **No HTTP, no DB engine, no UI, no scheduler** — those are later sub-projects.

**Placement — monorepo, root-level directories (no new code under `prototypes/`):**

- `compiler/` — OCaml `woc`: lexer, parser, typechecker, ownership flow pass, bytecode emitter.
- `runtime/` — C `wovm`: seeded by moving the existing `prototypes/wo-rt-c` code in; evolves per its A–F plan.
- Documentation stays under `docs/` (repo rule): this spec in `docs/superpowers/specs/`, phase plans in `docs/plan/`.
- `prototypes/` receives nothing new; existing `wo-db` stays as the query-layer reference.

**Dependency doctrine:** OCaml side = stdlib only, handwritten lexer and recursive-descent parser (no Menhir; dune as the build tool only). C side = libc only, same as `wo-rt-c`.

**Later sub-projects (named now, spec'd separately):**

2. Shard-actor runtime + per-shard heaps on the `wo-rt-c` A–F foundation (`spawn`, message send, ownership transfer).
3. DB engine binding — objects ↔ tables, SQL-layer statements execute (replaces `DB_STUB`).
4. HTTP/service layer — `service rest` blocks route to VM methods; trap surface maps to HTTP responses.
5. UI (`##ui` SSR + live patches).

The Rust runtime retires only after parity.

## Section 2 — Architecture

```
 .wo files
    │
    ▼
 compiler/ (OCaml, stdlib only)
    lexer.ml   ── tokens (newline-significant, same rules as crates/rt)
    parser.ml  ── AST (handwritten recursive descent)
    types.ml   ── typecheck: classes, structural interfaces, scalars
    owner.ml   ── flow pass: MVS borrow rules per fn, escape check,
                  marks runtime-check ops ONLY where static proof fails
    emit.ml    ── register bytecode
    │
    ▼
 app.wob (bytecode module: constant pool, class table, interface vtables,
          method code, line table)
    │
    ▼
 runtime/ (C, libc only)
    loader.c ── mmap .wob, validate once, link class table
    vm.c     ── register interpreter, computed-goto dispatch
    obj.c    ── object model: 16-byte header, per-shard arena allocator
    borrow.c ── runtime borrow acquire/release for residual sites
    gc.c     ── RC on @gc classes + Bacon–Rajan deferred cycle scan
                (per-shard, incremental, budgeted per tick — no global pause)
```

**Interface dispatch:** structural, Go-style. The compiler checks satisfaction and builds a per-(class, interface) vtable at compile time; the VM indexes it. No runtime reflection.

**Single-binary story:** dev mode is `wovm app.wob`; release mode `woc build` copies the `wovm` executable and appends the `.wob` plus an offset trailer — one self-contained deployable, the same promise `wo build` makes today.

## Section 3 — Language surface (milestone 1)

Grammar stays plan-13 compatible — `class` = fields + `fn`, no inheritance. New pieces: `interface`, `@gc`, parameter conventions.

```wo
interface Priced {
  fn current_price() -> Int
}

@table(name: "products")
class Product {                    -- default: owned, borrow-checked
  id:     Id
  sku:    Text @unique
  name:   Text
  prices: multi Price

  fn current_price() -> Int {      -- satisfies Priced structurally
    return latest(self.prices).amount;
  }

  fn rename(name: Text) {          -- self exclusive here (mutates)
    self.name = name;
  }
}

@gc
class PriceCache {                 -- reference semantics, freely aliased
  entries: map<Text, Int>
}
```

**Ownership rules the developer sees (mutable value semantics):**

1. A non-`@gc` object is an owned value. One owner. Assignment and return are moves.
2. Function parameter default = immutable borrow. `mut x: T` = exclusive borrow. `take x: T` = ownership moves in.
3. Borrows never escape: cannot be stored in a field, cannot be returned. Compile error.
4. Fields hold owned values, `ref T` ids (existing DB-style links), or `@gc` references.
5. `@gc` class instances alias freely: no borrow rules, reference-counted, cycles collected incrementally.
6. Method `self` is an immutable borrow if the body only reads, exclusive if it writes — the compiler infers this; no annotation.

**Executes in milestone 1:** class/interface declarations, constructors, field access, method and interface calls, control flow (`if`/`for`/`while`/`return`), arithmetic/text operations, `let`.

**Container types:** `multi T` (ordered collection) and `map<K, V>` are runtime-provided native object classes, not user-definable generics — the VM implements them in C, and they are accessed through builtins (`latest`, `count`, index/insert operations). Milestone 1 ships only these two.

**Parses but traps:** SQL-layer statements (`insert`, `select`), `service`/`policy`/`on` blocks — the emitter produces `DB_STUB`; the VM raises "engine not linked". The grammar stays whole; execution lands in sub-project 3.

**Deferred surface:** `spawn` / message send (sub-project 2). The header layout reserves a shard id now so no relayout is needed later.

## Section 4 — Memory model

**Object header (16 bytes):**

```c
struct wo_hdr {
    uint32_t class_id;   // index into loaded class table
    uint16_t shard_id;   // owner shard; always 0 in M1, reserved for sub-project 2
    uint8_t  flags;      // bit0 GC_MANAGED, bit1 IN_CYCLE_BUF
    uint8_t  _pad;
    uint32_t borrow;     // 0 = free, N = shared readers, 0xFFFFFFFF = exclusive
    uint32_t rc;         // strong count, @gc only; unused for owned
};                       // object fields follow inline
```

**Owned objects (default):** deterministic lifetime. The compiler emits `DROP` at owner scope end — destructor runs, memory is freed. Allocation from a per-shard arena with size-class free lists. No GC involvement, ever.

**Borrow enforcement split:**

- `owner.ml` proves most sites statically (locals, linear flow, no runtime-indexed aliasing) — zero ops emitted, zero runtime cost.
- Residual sites get `BORROW_S` / `BORROW_X` / `RELEASE` on the borrow word. Canonical residual case: two `mut` borrows through runtime indices (`items[i]`, `items[j]` where `i == j` is unprovable). A violation is a VM trap that unwinds to the method boundary as a structured error (Section 6).

**`@gc` objects:** RC increment/decrement on alias creation/drop (compiler-emitted, elided for provably balanced pairs). `rc == 0` frees immediately. Cycle risk exists only when a `@gc` object holds `@gc`-typed fields — those go to a per-shard possible-cycle buffer on decrement (Bacon–Rajan trial deletion), scanned **incrementally with a fixed per-tick budget** on the shard's own event loop. Per-shard heap, per-shard buffer: no cross-shard tracing, no global pause; worst case is a bounded slice of one shard's tick.

**Mixing rule:** an owned object may hold `@gc` references (rc participates). A `@gc` object may hold owned values (it owns them; they drop when the holder is freed). The borrow word applies only to owned objects; `@gc` aliasing is unrestricted by design.

## Section 5 — Bytecode and VM

> Normative format reference (pinned by plan 1): [`docs/plan/oop-vm/00-wob-format.md`](../../plan/oop-vm/00-wob-format.md) · machine-readable twin: [`runtime/src/wob.h`](../../../runtime/src/wob.h)

**Registers:** untyped 64-bit slots. The language is statically typed — the compiler knows every slot's type, so no tagging and no NaN-boxing. Scalars inline (`Int`/`Timestamp` = i64, `Bool`), heap values as pointers (the header supplies the class at runtime for interface dispatch and traps).

**`.wob` module format:** magic + version, then sections — constant pool (texts, numerics), class table (field layout, size, `@gc` bit, drop plan), interface table, per-(class, interface) vtables, method code (arg count, register count, bytecode), line table (for error reporting). The loader `mmap`s the file, bounds-validates every index once, and links class ids.

**Instruction set (~40 ops):**

| Group | Ops |
| --- | --- |
| data | `LOADK`, `MOVE` |
| arith/text | `ADD SUB MUL DIV NEG`, `CONCAT`, comparisons |
| control | `JMP`, `JZ`, `CALL`, `ICALL` (vtable), `RET` |
| objects | `NEW`, `GETF`, `SETF`, `DROP` |
| borrows | `BORROW_S`, `BORROW_X`, `RELEASE` (residual sites only) |
| gc | `RC_INC`, `RC_DEC` (elided when balance is provable) |
| runtime | `BUILTIN` (`now`, `latest`, `count`, `words`, …), `DB_STUB`, `TRAP` |

**Dispatch:** computed goto (`&&label` table) with a `switch` fallback under `-DWO_ISO_C` — the same portability pattern `wo-rt-c` uses.

**Calling convention:** contiguous frame stack; the callee gets a fresh register window, `self` in r0, arguments in r1..rN (moved or borrowed per signature). Fixed-depth stack; overflow is a trap.

## Section 6 — Error handling

**Compile time (`woc`):**

- Diagnostics carry `file:line:col`, a source excerpt, and a stable code (`WO-E###`). The parser recovers at declaration/statement sync points and reports many errors per run.
- Ownership errors name both sites: "`p` moved at pricing.wo:14, used at pricing.wo:17"; "borrow of `self.prices` escapes `current_price`". These messages are the product — MVS only beats Rust ergonomics if the errors are plain.

**Runtime (`wovm`) — traps:** borrow violation, division by zero, stack overflow, arena OOM, `DB_STUB`, bad interface dispatch (unreachable after loader validation; kept as defense).

- A trap unwinds to the method-call boundary. Each frame has a compiler-emitted **drop map** — unwinding runs `DROP` for live owned values, so traps never leak.
- Traps surface as a structured error `{code, method, line, message}` via the line table. In milestone 1 the harness prints it and exits nonzero. Sub-project 4 maps the same structure to HTTP responses — one trap surface forever.
- No undefined-behavior path: the loader pre-validates all static indices (registers, fields, vtable slots); the interpreter trusts them afterward. Residual dynamic checks (borrow word, bounds on runtime-indexed access) always trap, never corrupt.
- No panics/aborts except assertion failures under a debug build.

## Section 7 — Testing

**Compiler (`compiler/`, OCaml stdlib-only harness — a tiny assert runner under `dune runtest`; no ounit/alcotest):**

- Unit tests: lexer tokens, parser AST shapes, typechecker verdicts, owner-pass decisions (elided vs residual per site).
- Golden files: each fixture `.wo` has an expected `--dump-ast`, `--dump-bc` (disassembly), or expected diagnostics (`WO-E###` + line). The dump flags exist for this.

**VM (`runtime/`):**

- C unit tests per module: arena/free lists, borrow-word transitions, RC + cycle scan (budget respected, cycles freed, deterministic order), interpreter ops.
- The suite runs under ASan and Valgrind via a `just` recipe — drop-map correctness means zero leaks on both success and trap paths.

**Conformance corpus (drives both — the spine):** a directory of small `.wo` programs, three kinds —

1. runs, with expected stdout;
2. must fail compilation, with an expected error code — the ownership-rules suite (move-after-use, borrow escape, double `mut`);
3. must trap at runtime, with an expected trap code (aliased `mut` via runtime index, `DB_STUB`).

The pricing-demo classes seed kind 1. End-to-end: `woc` compiles, `wovm` runs, the harness diffs output.

**Parity check (later, cheap):** the corpus subset that overlaps 13b features also runs on the `crates/rt` method executor — same output required until the Rust runtime retires.

Recipes: `just woc-test`, `just wovm-test`, `just oop-e2e`.

## Success criteria

Milestone 1 is done when:

1. `woc docs/examples/pricing` (logic subset) compiles to `.wob` in under 100 ms on a developer laptop.
2. `wovm` runs the pricing classes' methods with correct output.
3. The ownership corpus passes: every must-fail program fails with the expected `WO-E###`; every must-trap program traps with the expected code; ASan/Valgrind report zero leaks and zero errors across the suite.
4. `@gc` cycle test: a cyclic `@gc` graph is collected within budgeted ticks with no pause longer than the configured slice.
5. `woc build` produces a single self-contained binary that runs with no arguments.

## Milestone 1 acceptance — 2026-08-11

Gate: `just oop-accept` (plan 3, Task 8), run for real against this working tree. Full output archived in `.superpowers/sdd/2026-08-01-wob-emit-e2e-single-binary/task-8-report.md`. Per-criterion result:

- [x] **1. Compile time.** `woc --emit` over the pricing-demo logic subset (`tests/corpus/{run/pricing-containers,run/pricing-current-price,run/pricing-discounted,run/pricing-text,trap/pricing-set-price-db-stub}/fixture.wo` — the five fixtures Task 3 derived from `docs/examples/pricing/`; the demo's original `.wo` files use surface milestone 1 doesn't have, so these are milestone 1's "logic subset" in fact, not the directory named in the spec's prose), measured over 20 runs: min 10.8 ms, avg 13.1–13.8 ms, max 16.9–17.6 ms. Worst observed run is under a fifth of the 100 ms budget. **MET.**
- [x] **2. Pricing output.** All four `run/pricing-*` fixtures compile and run under `wovm`, stdout byte-exact against `fixture.out`, under both the release binary and `runtime/build/wovm_asan`. **MET.**
- [ ] **3. Ownership corpus + ASan/Valgrind zero leaks.** The error-code half is fully met: all 7 `compile-fail/` fixtures fail with exactly their named `WO-E###`, all 5 `trap/` fixtures trap with exactly their named code. The ASan half is **not met**: running the full corpus (`run/`, `compile-fail/`, `trap/`, `gc/`, single-binary smoke — 26 checks) against `runtime/build/wovm_asan` (`make -C runtime wovm-asan`, Task 5's target), `gc/held-cycle` fails — LeakSanitizer reports a definite leak (1184 bytes / 3 allocations, `wo_multi_push`/`wo_obj_new` via `main.c:160`'s `wo_vm_call`) instead of exit 0. This is not a false positive to suppress: `runtime/src/main.c`'s entry-method return value (`uint64_t ret`, line 158) is stored and never used again, so the "permanent external hold" the fixture's own comment claims is not actually realized in the C driver — nothing keeps that pointer live for a precise scanner to find. The other 25 checks, including the other two `gc/` fixtures, are ASan-clean. **NOT MET** — tracked in `docs/00-status.md`'s "Known gaps carried out of iteration 4" and the SDD ledger; a `runtime/src/main.c` fix, out of scope for the task that found it.
- [x] **4. `@gc` cycle collection.** `gc/abandoned-cycle` (unreachable 2-cycle, collected step 1, `freed=2`) and `gc/budget-steps` (budget-sliced collection, `freed=4`) both pass byte-exact stdout plus exact `WO_GC_TRACE` step/freed counts, clean under ASan. `gc/held-cycle` demonstrates the complementary correctness property — an externally-held cycle is correctly *not* collected (`freed=0`, matching `fixture.trace`) — which is the GC decision criterion 4 asks about; its ASan failure is a criterion-3 (leak-detector) concern, not a collection-correctness one. **MET.**
- [x] **5. Single-binary.** `scripts/single-binary-smoke.sh` against the release `runtime/wovm`: `woc build` produces an executable; copied to a directory outside the repo and run with no arguments, stdout is byte-exact; a corrupted trailer fails clearly on exit 2 (never a crash or hang). All 3 checks pass. **MET.**

**4 of 5 criteria met; criterion 3 is not**, specifically its ASan-zero-leaks clause. `just oop-accept` fails loudly at that stage and does not proceed to the remaining stages (single-binary smoke, both unit gates) in the same run by design — those were verified to pass independently (see the Task 8 report) but are gated behind fixing this finding in a real `oop-accept` run.

### Update — 2026-08-11: criterion 3 closed, all five criteria MET

Root cause was already pinned above and did not change on inspection: `gc/held-cycle`'s "permanent external hold" was `runtime/src/main.c`'s entry-return value never being released — a genuine refcount leak, not a false positive. Adding release-on-return to `main.c` was rejected as the fix: the `.wob` method table carries no return-type/kind metadata, so the driver has no way to tell a pointer return from a scalar one, and adding that metadata is a format change out of scope for this closure.

Fixed at the source instead: this milestone's own spec (Section 4/Success criteria; the systems-track spec, `2026-08-01-systems-track-design.md:70`) makes the program entry's return value the process exit code, so an entry declaring a class return type was never legal — it just went unchecked. `compiler/src/emit.ml` now raises `WO-E405` for a free-fn entry (`main`, zero args) whose declared return type is anything but `Int`; a `main` with no return annotation is unaffected. Catalog entry in `01-error-catalog.md`; a `compiler/test/runner.ml` case (`entry return type: ...`, 4 checks) pins both the positive (fires on `-> Widget`) and negative (silent on no annotation) cases so it cannot regress silently.

With the entry contract enforced at compile time, `gc/held-cycle`'s premise — an externally-held cycle surviving a *post-exit* pump — is no longer expressible: nothing can hold a root past the point `main` returns, by construction. The fixture is retired (`oop-vm/02-corpus.md`, "Retired" note has the full reasoning); the scenario it meant to demonstrate remains covered, just one layer down, by `test_externally_held_cycle_survives_then_dies` in `runtime/test/test_cycle.c`, which holds its root the honest way (a real C local, not a leaked VM return value). This also means **criterion 4's evidence above is now partly stale**: `gc/held-cycle` no longer exists to "demonstrate the complementary correctness property" it's credited with — criterion 4 remains MET on the strength of `gc/abandoned-cycle` and `gc/budget-steps` alone (both untouched, both still ASan-clean), with the externally-held-cycle property now proven at the runtime-test layer instead of the corpus layer. Story iteration 7b (tracing GC) is scheduled to give the corpus a proper in-flight externally-held fixture once there is a root that doesn't route through `main`'s return.

Fresh `just oop-accept` run against this working tree, full output archived alongside this note in `.superpowers/sdd/2026-08-01-wob-emit-e2e-single-binary/m1-criterion3-closure-report.md`:

- [x] **1. Compile time.** 20 runs over the same 5-fixture pricing subset: min 6.464 ms, avg 6.825 ms, max 7.590 ms — under 8% of the 100 ms budget. **MET.**
- [x] **2. Pricing output.** Unchanged from the Task 8 report; still byte-exact under both `wovm` and `wovm_asan`. **MET.**
- [x] **3. Ownership corpus + ASan/Valgrind zero leaks.** Full corpus (`run/`×8, `compile-fail/`×7, `trap/`×5, `gc/`×2, single-binary-smoke×3 = 25 checks, one fewer than the Task 8 report's 26 because `gc/held-cycle` is retired) against `runtime/build/wovm_asan`: `oop-e2e: 25 checks, 0 failures`. Zero LeakSanitizer reports. **MET.**
- [x] **4. `@gc` cycle collection.** `gc/abandoned-cycle` (`freed=2`) and `gc/budget-steps` (`freed=4`) both pass byte-exact stdout and exact `WO_GC_TRACE` counts, clean under ASan — see the note above on `gc/held-cycle`'s retirement and where its property now lives. **MET.**
- [x] **5. Single-binary.** `scripts/single-binary-smoke.sh` against the release `runtime/wovm`: 3/3 checks pass. **MET.**

Both unit gates also ran green in the same `oop-accept` invocation: runtime (`make -C runtime test` + `test-iso` + `cli_smoke.sh`, 13 suites × 2 dispatch flavors, all ASan/UBSan-clean) and compiler (`dune runtest --root compiler`: 14 + 399 checks, up from 14 + 395 — the 4 new `WO-E405` cases). `oop-accept` printed `oop-accept: ALL CRITERIA MET`.

**5 of 5 criteria met.** Milestone 1's acceptance gate is fully green.
