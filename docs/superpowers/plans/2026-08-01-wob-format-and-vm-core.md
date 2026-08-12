# .wob Format + wovm VM Core Implementation Plan

> **Status: ✅ done** (story iteration 2) — `.wob` v1 loader with full static validation, register interpreter (computed-goto + ISO-C fallback), arena with size-class free lists, borrow word, RC + budgeted Bacon–Rajan cycle collector, drop-map trap unwinding, containers, builtins, ICALL, CLI. 13 suites × 2 dispatch flavors + CLI smoke, ASan/UBSan clean. The format contract itself lives on in [`plan/oop-vm/00-wob-format.md`](../../plan/oop-vm/00-wob-format.md). Board: [00-status.md](../../00-status.md)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
>
> **Style rule (user convention):** this plan states concept, reason, and required behavior in words. The executor writes the actual code at implementation time; nothing here is copy-paste source.

**Goal:** Build the C register VM (`wovm`) and pin the `.wob` bytecode format so it runs hand-assembled bytecode with the full milestone-1 memory model: owned objects with a runtime borrow word, `@gc` reference counting with budgeted cycle collection, and trap unwinding that never leaks.

**Architecture:** Plan 1 of 3 for the approved spec `docs/superpowers/specs/2026-08-01-oop-compiler-vm-design.md`. The existing `prototypes/wo-rt-c` moves to root-level `runtime/`; VM modules land in `runtime/src/`, C unit tests in `runtime/test/`, driven by an in-memory `.wob` assembler helper so the VM is fully testable before any compiler exists. Plan 2 (OCaml `woc` compiler front) and plan 3 (bytecode emit + end-to-end conformance corpus + single binary) follow and consume the format pinned here. Plan 2 needs OCaml + dune installed (not present on the dev box today).

**Tech Stack:** C11, libc only. gcc 13 with AddressSanitizer + UBSan for the test suite (valgrind is not installed — ASan is the primary checker). `make` inside `runtime/`, `just` recipes at the repo root.

## Global Constraints

- **libc only** in `runtime/` — no external C libraries (spec dependency doctrine).
- **C11**, `-Wall -Wextra -Werror` in test builds.
- **Little-endian on-disk format**; x86-64/ARM64 Linux only in milestone 1.
- **Limits, loader-enforced:** 64 registers per method, 4096 value-stack slots, 256 frames.
- **No undefined behavior on any input:** the loader validates every static index once; residual dynamic checks (borrow word, field bounds, map keys) always trap, never corrupt (spec section 6).
- **Traps never leak:** unwinding applies drop maps; the whole suite must be ASan-clean on success and trap paths alike.
- **Dispatch:** computed goto under GNU C, `switch` fallback under a `WO_ISO_C` define (spec section 5); both flavors stay under test.
- **Git: the executing agent NEVER runs `git commit`** (user convention). "Record commit draft" means: append the given message to `.dev/commit.md` at the repo root and move on; the user commits at their own checkpoints. `git add`/`git mv`/`git status` are fine.
- **Docs rule:** documentation lives under `docs/`; `runtime/README.md` stays an orientation README.

---

## File Structure

```
runtime/                      (moved from prototypes/wo-rt-c — Task 1)
  Makefile                    existing + wovm/test targets (grows per task)
  README.md                   orientation note updated
  wo-rt.c                     UNTOUCHED — event-loop reference for sub-project 2
  bench/                      moved as-is
  src/
    wob.h                     format constants, limits, opcodes, shared structs (Task 2)
    obj.h obj.c               arena allocator, object headers, strings (Tasks 3–4)
    borrow.h borrow.c         borrow-word acquire/release (Task 5)
    cont.h cont.c             native containers: multi, map (Task 6)
    gc.h gc.c                 RC, drop plans, cycle collector (Tasks 7–8)
    loader.h loader.c         .wob loader + full validation (Task 10)
    vm.h vm.c                 register interpreter, frames, traps, unwinding (Tasks 11–13)
    builtin.h builtin.c       builtin table (Task 14)
    main.c                    wovm CLI (Task 16)
  test/
    t.h                       tiny assert harness (Task 2)
    wob_build.h wob_build.c   in-memory .wob assembler for tests (Task 9)
    test_*.c                  one binary per module/topic (Tasks 3–15)
    cli_smoke.sh              end-to-end CLI check (Task 16)
docs/plan/oop-vm/00-wob-format.md   normative format reference (Task 2)
justfile                      paths fixed (Task 1), wovm recipes (Task 16)
```

Every `test_*.c` compiles to its own binary and exits nonzero on failure; `make -C runtime test` builds and runs all of them as an ASan build.

---

## The `.wob` format v1 (normative — Task 2 copies this section into `docs/plan/oop-vm/00-wob-format.md`)

All integers little-endian; offsets are absolute file offsets.

**Header (44 bytes):** magic `"WOB1"`, version 1, then offset/count u32 pairs for the constant pool, class table, interface section, and method table, then a u32 entry-method index (all-ones = none).

**Constant pool** — sequential entries: one tag byte; tag 0 = i64 follows; tag 1 = text (u32 length + bytes, no NUL).

**Class table** — per class: name constant index, flags u32 (bit0 = instances are `@gc`), field count, then one kind byte per field padded to a 4-byte boundary. Field kinds: 0 SCALAR, 1 OWNED, 2 GCREF, 3 TEXT, 4 MULTI, 5 MAP. Runtime object layout: 16-byte header then one 8-byte slot per field, in declaration order.

**Interface section** — per interface: name constant index, method count. Global *slot ids* are assigned sequentially across interfaces in declaration order. Then a vtable row count and rows: class id, interface id, one method index per interface method.

**Method table** — per method: name constant index, class id (all-ones = free fn), arg count u8, register count u8, reserved u16, code length in bytes (multiple of 4), the u32 instructions, a line table (count + ascending pc→line pairs), and a drop table (count + ascending entries of pc, owned-register bitmask u64, gc-register bitmask u64). Drop-table lookup = last entry with pc ≤ current pc; no entry means nothing live.

**Instructions** — fixed 32-bit, Lua-style fields: opcode byte, A byte, then either B and C bytes or a 16-bit Bx (signed jumps encode as Bx − 32768).

| op | name | semantics (in words) |
| --- | --- | --- |
| 0 | NOP | nothing |
| 1 | LOADK A Bx | register A = constant Bx (int inline; text = pointer to interned const string) |
| 2 | MOVE A B | copy register; for owned values this IS the move — compiler guarantees the source is dead |
| 3–7 | ADD/SUB/MUL/DIV/NEG | i64 arithmetic, two's-complement wrapping (no signed-overflow UB); DIV traps on zero divisor and on INT64_MIN ÷ −1 |
| 8 | CONCAT A B C | new owned text from two texts |
| 9–12 | EQ/LT/LE/EQS | i64 compares and text-content equality, result 0/1 |
| 13–14 | JMP / JZ | relative jump (JZ when register A is zero) |
| 15 | CALL A Bx | call method Bx; callee's register window starts at caller base + A (register-window overlap, Lua-style); args sit at A, A+1, …; return value lands back in slot A |
| 16 | ICALL A Bx | interface call by global slot id Bx; receiver in A; vtable lookup by the receiver's class |
| 17–18 | RET A / RET0 | return value from register A (or zero), pop frame |
| 19 | NEW A Bx | new zeroed instance of class Bx |
| 20–21 | GETF / SETF | field read/write with runtime null/native/bounds checks (trap T_BOUNDS); overwriting a non-scalar field does NOT auto-drop the old value — the compiler emits the drop |
| 22 | DROP A | recursively drop the owned value in A per its class drop plan, null the register |
| 23–26 | BORROW_S/BORROW_X/RELEASE_S/RELEASE_X | borrow-word ops on the object in A; violation traps T_BORROW |
| 27–28 | RC_INC / RC_DEC | refcount ops on the `@gc` object in A |
| 29 | BUILTIN A B C | register A = builtin C applied to args starting at register B (fixed arity per builtin; `multi_new`/`map_new` carry kind immediates in B instead) |
| 30 | DB_STUB | trap T_DB "engine not linked" (spec: SQL-layer statements in milestone 1) |
| 31 | TRAP Bx | explicit trap with code Bx |

**Builtins:** now (ms), print (text), print_int, words (whitespace token count), multi_new/multi_push/multi_get/count/latest, map_new/map_set/map_get/map_has.

**Trap codes:** DIV0, BORROW, STACK, OOM, DB, BOUNDS, KEY, EXPLICIT.

---

### Task 1: Move `prototypes/wo-rt-c` → `runtime/`

**Files:** git-mv the directory; modify `justfile` (rt-c-demo, rt-c-bench paths), `runtime/README.md`, `CLAUDE.md` path references.

**Concept & reason:** the spec's monorepo decision — root-level `runtime/` is the real C runtime now, seeded by the wo-rt-c event-loop reference. Move first so every later task lands in the final location. Recipe names stay (muscle memory); only paths change. `wo-rt.c` itself is not touched.

- [x] Move with `git mv`, delete any stale built binary.
- [x] Update every `prototypes/wo-rt-c` reference in `justfile`, `CLAUDE.md`, and the README's self-description; add a one-line note in `runtime/README.md` explaining the move and the coming `src/` VM core.
- [x] Verify: `make -C runtime` still builds `wo-rt`; `just rt-c-demo` round-trips.
- [x] Record commit draft: `refactor: move prototypes/wo-rt-c to runtime/ (monorepo root dir per OOP spec); justfile/CLAUDE.md/README paths updated, wo-rt.c untouched.`

### Task 2: `wob.h` format header + test harness + format doc

**Files:** create `runtime/src/wob.h`, `runtime/test/t.h`, `docs/plan/oop-vm/00-wob-format.md`; modify `runtime/Makefile`.

**Concept & reason:** one header is the single source of truth for the compiler↔VM contract: magic/version, header field offsets, field kinds, object-header struct (16 bytes, static-asserted), flags (GC, cycle-buffer, const, two color bits for the collector), native class-id sentinels (string/multi/map/free-fn), borrow-word sentinels, trap codes, the opcode enum, instruction encode/decode helpers, builtin ids, limits, and a class-descriptor struct (name, flags, field count, kind array) shared by loader and runtime. `t.h` is a ~20-line assert harness (counters + report macro) — no external test framework, per doctrine. The Makefile gains a generic rule: each `test/test_*.c` builds into its own ASan binary linked against all `src/*.c` except `main.c`, and a `test` target runs them all.

- [x] Write a syntax-only compile check for the not-yet-existing header; see it fail.
- [x] Write `wob.h` with all constants above and a static assert that the object header is exactly 16 bytes; write `t.h`; extend the Makefile.
- [x] Verify the header compiles clean under `-Wall -Wextra -Werror`.
- [x] Create the format doc by copying this plan's normative format section verbatim; link it from the spec's plan references.
- [x] Record commit draft: `feat(runtime): wob.h .wob v1 contract (opcodes, kinds, traps, builtins, 16-byte header), t.h harness, Makefile test scaffold; docs/plan/oop-vm/00-wob-format.md normative reference.`

### Task 3: Arena allocator

**Files:** create `runtime/src/obj.h`, `runtime/src/obj.c`; test `runtime/test/test_arena.c`.

**Concept & reason:** spec section 4 — per-shard arena with size-class free lists. One malloc'd region; allocations round to 16 bytes; sizes up to 1024 use per-size free lists (freed blocks chain through their own first word); larger sizes fall through to plain malloc/free (caller always passes the size back on free, so no size headers are needed). Region exhaustion returns null — the VM maps that to the OOM trap, never aborts.

**Interface (in words):** init with a byte capacity, destroy, alloc(size)→pointer-or-null, free(pointer, size).

- [x] Write failing tests: same-size-class reuse returns the freed block; exceeding the region returns null; >1024 sizes succeed regardless of region cap.
- [x] See them fail (missing header), implement, see them pass under ASan.
- [x] Record commit draft: `feat(runtime): arena allocator — 16-byte size-class free lists to 1024B, malloc fallback above, NULL on region OOM; test_arena.`

### Task 4: Object model, strings, runtime context

**Files:** extend `obj.h`/`obj.c`; test `runtime/test/test_obj.c`.

**Concept & reason:** everything on the heap carries the 16-byte header — class objects, strings, containers — so drop/GC/borrow logic can dispatch on any value uniformly. A `wo_rt` context struct bundles what every module needs: the arena, the class-descriptor table, the cycle-candidate buffer (filled by Task 8), and an output stream pointer (so tests can capture builtin `print`). Object creation zeroes all field slots and, for `@gc` classes, sets the GC flag and refcount 1. Strings are header + length + inline bytes; concat allocates a new owned string; freeing a const-flagged string is a no-op (const strings are interned by the loader and outlive everything).

**Interface (in words):** rt init/destroy; object-size-of-class helper; object-new by class id (null = OOM); string new/concat/content-equality/free; a fields-accessor giving the slot array behind a header.

- [x] Failing tests: owned object comes back zeroed with free borrow word; `@gc` object starts at rc 1 with the GC flag; string round-trip + concat + equality; const-flagged string survives a free call.
- [x] Implement; suite green under ASan.
- [x] Record commit draft: `feat(runtime): object model — wo_rt context, wo_obj_new (zeroed, @gc rc=1), strings with const interning; test_obj.`

### Task 5: Borrow-word runtime

**Files:** create `runtime/src/borrow.h`, `borrow.c`; test `runtime/test/test_borrow.c`.

**Concept & reason:** spec section 4's residual-check primitive. The header's borrow word counts shared readers; an all-ones sentinel means exclusively borrowed. Acquire-shared fails only against exclusive; acquire-exclusive fails unless completely free; releases are unconditional (the compiler emits them balanced). Failures return an error code — the VM (Task 13) turns that into the borrow trap. ~20 lines of C; kept its own module because it is the semantic heart of the hybrid model and the compiler's emit rules will cite it.

- [x] Failing tests: the full state machine — stacked shared readers block exclusive; exclusive blocks both; releases restore.
- [x] Implement; green.
- [x] Record commit draft: `feat(runtime): borrow-word runtime — shared counter / exclusive sentinel, -1 on violation (VM maps to T_BORROW); test_borrow.`

### Task 6: Native containers — multi and map (data ops)

**Files:** create `runtime/src/cont.h`, `cont.c`; test `runtime/test/test_cont.c`.

**Concept & reason:** spec section 3 — `multi T` and `map<K,V>` are runtime-provided native classes, not user generics. Multi: header + length/capacity + element-kind tag + growable u64 array (malloc/realloc-backed). Map: header + parallel key/value arrays + kind tags for both, **linear scan lookup** — deliberate milestone-1 KISS, documented; keys compare by content when the key kind is text, by bits otherwise. Map insert-or-replace hands the replaced old value back to the caller instead of dropping it, because dropping needs Task 7's machinery — the builtin layer (Task 14) does the drop. Container freeing (element drops) also lands in Task 7 for the same reason; this task's tests free backing arrays by hand.

- [x] Failing tests: multi push/get across growth + bounds miss; map insert/replace/get/has with int keys; map text keys hit by content through a different pointer.
- [x] Implement; green.
- [x] Record commit draft: `feat(runtime): native containers — multi (growable, elem-kind tag), map (linear scan, content keys, replace returns old value); test_cont.`

### Task 7: RC + drop plans (deterministic destruction)

**Files:** create `runtime/src/gc.h`, `gc.c`; test `runtime/test/test_rc.c`.

**Concept & reason:** spec section 4 — owned objects die deterministically; `@gc` objects die at refcount zero. One kind-directed dispatcher ("drop this 8-byte value known to be of field kind K") is the workhorse: scalars ignored, owned values drop recursively, gc refs decrement, texts free, containers free element-wise then their backing. A class object's drop walks its kind array over its field slots, then frees itself; native headers (string/multi/map sentinels) route to their own frees. Refcount decrement to zero releases the same way (with a guard for objects sitting in the cycle buffer — the collector owns their death, Task 8).

**Testing trick (used by all memory tests from here on):** classes given ~130 fields exceed the 1024-byte size-class ceiling, so instances take the malloc path — any missed free is a hard ASan leak report, making destructor correctness machine-checked instead of eyeballed.

- [x] Failing tests: owned-tree recursive drop; gc-ref field decrement then final decrement frees; text and multi-of-text fields freed with the holder.
- [x] Implement; whole suite ASan-clean.
- [x] Record commit draft: `feat(runtime): RC + drop plans — kind-directed drop dispatcher, recursive class drops, container element drops, rc inc/dec with zombie guard; test_rc (malloc-path classes make ASan prove every free).`

### Task 8: Budgeted cycle collector

**Files:** extend `gc.c`; test `runtime/test/test_cycle.c`.

**Concept & reason:** spec section 4 — Bacon–Rajan trial deletion, per-shard, budgeted, never global. Decrements that leave a possibly-cyclic object alive (class has any gcref/multi/map field) buffer it as a candidate, deduplicated by a header flag. A collection step takes up to *budget* buffered roots and runs the classic three phases — mark-gray (trial-delete internal edges by decrementing through them), scan (restore subgraphs that still have external count), collect-white (gather the dead) — using the two header color bits. Two deliberate simplifications, both documented in code: (1) whole strongly-connected components process atomically, so budget bounds *roots started*, with bounded overshoot; (2) all gathered whites are freed together *after* the phases (contents first — skipping already-trial-deleted gcref edges — then the objects), which removes every dangling-candidate/zombie hazard that plagues incremental freeing. Child traversal descends through container elements whose kind is gcref. The step returns how many objects it freed, which is what tests observe.

- [x] Failing tests: a two-object cycle collects fully; two independent cycles under budget 2 collect one per step; an externally-held cycle survives with counts restored and flags cleared, then collects once truly dead; a cycle routed through a multi's elements collects.
- [x] Implement; ASan-clean (cycle objects use the malloc-path trick).
- [x] Record commit draft: `feat(runtime): budgeted cycle collector — Bacon-Rajan trial deletion in epochs, candidate buffer with flag dedup, deferred freeing kills zombie hazards, container edges traversed; wo_gc_step(budget) returns freed count; test_cycle.`

### Task 9: `.wob` assembler test helper

**Files:** create `runtime/test/wob_build.h`, `wob_build.c`; test `runtime/test/test_wobbuild.c`; Makefile links the helper into every test binary.

**Concept & reason:** the VM must be testable without a compiler (plan 2 is months of OCaml away). A small builder assembles valid images in memory: append constants (int/text), classes (flags + kind bytes + padding), interfaces (slot ids accumulate in declaration order), vtable rows, and methods (code array + optional line pairs + optional drop entries of pc/owned-mask/gc-mask), then finish by computing the section offsets into a header and concatenating — returning one malloc'd image. Every later VM test and the Task-16 smoke generator is written against this helper, which also makes it the second, independent encoding of the format — disagreements between builder and loader surface as test failures, effectively cross-checking the spec.

- [x] Failing test: build a minimal one-method image and assert raw header bytes — magic, version, section counts, entry index, first constant's tag/length/content at its stated offset.
- [x] Implement (dynamic byte buffers per section; sizes computed at finish); green.
- [x] Record commit draft: `test(runtime): wob_build in-memory .wob assembler (all sections, line+drop tables, offset computation); test_wobbuild checks raw header bytes.`

### Task 10: Loader with full validation

**Files:** create `runtime/src/loader.h`, `loader.c`; test `runtime/test/test_loader.c`.

**Concept & reason:** the "no UB on any input" constraint lives or dies here. The loader parses a byte buffer through a bounds-checked cursor (every read validates remaining length), **copies everything out** into aligned, malloc'd structures — so misaligned files and lifetime coupling to the input buffer are non-issues — and interns text constants as const-flagged strings. File loading is mmap → parse → munmap. Output module: constant array, class-descriptor table (kind bytes pooled), interface slot count, vtable rows expanded to flat (class, slot, method) triples sorted for binary search, and per-method records (code, line table, drop table).

**Validation contract (what the interpreter is allowed to assume forever):** magic/version match; register counts in 1..64 with args ≤ registers; every opcode known; every static register operand within the method's register count; constant/class/callee/slot indexes in range; CALL argument windows fit the caller's frame; jump targets inside the code; the **last instruction is a terminator** (return/trap/db-stub/jump — nothing falls off the end); builtin ids in range with per-builtin arity fitting the frame and kind-immediates in range; line/drop tables strictly ascending and in range; vtable ids in range; entry (if present) is a zero-arg free fn. Field indexes on GETF/SETF are deliberately NOT static-checkable (registers are untyped) — they stay runtime checks (Task 12), matching the spec's residual-check doctrine. Every rejection fills a human-readable error naming method and pc; every failure path frees everything parsed so far.

- [x] Failing tests: a valid builder image loads with correct counts, interned const string, line entries; then a rejection battery — corrupted magic, unknown opcode, constant index out of range, non-terminator tail, register out of range, truncated buffer — each must fail with a nonempty error and no leak.
- [x] Implement; green, including ASan on the failure paths.
- [x] Record commit draft: `feat(runtime): .wob loader — bounds-checked parse, aligned copies, const-string interning, sorted vtable expansion, full static validation (opcode/reg/index/jump/terminator/arity), mmap file path; test_loader happy + 6 rejects, leak-free failures.`

### Task 11: Interpreter core — arithmetic, control flow, calls, traps

**Files:** create `runtime/src/vm.h`, `vm.c`; test `runtime/test/test_vm.c`; Makefile gains a `test-iso` target.

**Concept & reason:** the register machine per spec section 5. A VM instance owns the module pointer, a runtime context, the 4096-slot value stack, and a 256-deep frame stack (method, pc, base). Calls use Lua-style window overlap: the callee's register 0 is the caller's slot A, so argument passing and value return are the same copy — returning writes the result into the call slot and pops. Non-argument callee registers are zeroed on entry (drop masks must never see stale bits). Dispatch is the dual-flavor macro pattern: computed goto under GNU C, plain switch under the ISO define, one shared case-body text — and the ISO flavor gets its own Makefile target run in every gate so the fallback can never rot. Arithmetic is two's-complement wrapping via unsigned math (no signed-overflow UB); division traps on zero and on the INT64_MIN ÷ −1 corner. The public entry ("call method with these argument words") validates method index and arity, seeds frame zero, runs to completion, and on any trap fills the structured error — code, method name (from the name constant), source line (from the line table, looked up at the trapping pc), message — then unwinds. In this task unwinding just pops frames; drop maps arrive in Task 13. Ops owned by Tasks 12–15 exist as cases that trap "op not wired yet" — loader-legal, semantically explicit, replaced task by task.

- [x] Failing tests: two-arg add returns through the window convention; recursive fib(10) = 55 (exercises call/return, jumps, compares); division by zero traps with the right code AND the right line; self-call-forever traps stack overflow at the frame cap; depth is zero after every trap.
- [x] Implement; both dispatch flavors green (`test` + `test-iso`).
- [x] Record commit draft: `feat(runtime): interpreter core — dual dispatch (computed goto / WO_ISO_C switch), register-window calls, wrapping i64 arithmetic, structured trap errors with line lookup; unwired ops trap explicitly; test_vm (add, fib, div0+line, stack cap) + test-iso gate.`

### Task 12: Object opcodes — NEW / GETF / SETF / DROP

**Files:** modify `runtime/src/vm.c`; test `runtime/test/test_objops.c`.

**Concept & reason:** wire the object model into the loop. One shared receiver-check helper enforces the residual runtime checks the loader cannot do statically: non-null, an actual class object (not a native sentinel), field index inside the class — any failure traps BOUNDS with a message naming the reason. NEW allocates by class id (OOM traps, never aborts). SETF stores the raw word — overwriting a non-scalar field does not auto-drop the old value; that is the compiler's obligation (already documented in the format section). DROP delegates to the Task-7 dispatcher and nulls the register so unwinding cannot double-free.

- [x] Failing tests: new → set field → get field → drop → return round-trip; out-of-range field index traps BOUNDS; null receiver traps BOUNDS.
- [x] Implement; suite + ISO flavor green.
- [x] Record commit draft: `feat(runtime): object opcodes — NEW (T_OOM), GETF/SETF null/native/bounds residual checks (T_BOUNDS), DROP with register nulling; test_objops.`

### Task 13: Borrow/RC opcodes + drop-map trap unwinding

**Files:** modify `runtime/src/vm.c`; test `runtime/test/test_unwind.c`.

**Concept & reason:** the spec's core promise — "traps never leak" — becomes real. The four borrow ops and two rc ops call the Task-5/Task-7 primitives after the same receiver checks; acquire failures trap BORROW. The Task-11 unwinding stub is replaced: walk frames innermost to outermost; in each, look up the drop-table entry for that frame's current instruction (the trap pc for the innermost frame; the instruction before the saved resume pc — i.e. the CALL — for outer frames); apply the owned mask by recursive drop and the gc mask by decrement, nulling registers as they go. A frame with no matching entry drops nothing. A borrow held by a dying register does not block its drop — the borrower IS the dying frame (noted in a comment).

- [x] Failing tests: double-exclusive borrow traps BORROW with correct line, and the owned object named in the drop mask is freed (malloc-path class, ASan-proven); a two-frame program where the child traps mid-body frees owned objects in BOTH frames; rc inc/dec through opcodes frees at zero.
- [x] Implement; suite + ISO green, ASan silent on all trap paths.
- [x] Record commit draft: `feat(runtime): borrow/rc opcodes + drop-map unwinding — T_BORROW on violation, frame walk applies owned/gc masks so traps never leak (spec section 6); test_unwind, ASan-proven.`

### Task 14: Builtins + DB_STUB + TRAP

**Files:** create `runtime/src/builtin.h`, `builtin.c`; modify `vm.c` (three cases); test `runtime/test/test_builtin.c`.

**Concept & reason:** the runtime services bytecode can't express. One dispatcher takes the VM, the current frame's registers, and the instruction; returns success or a trap code plus message. Behaviors: `now` = wall-clock milliseconds; `print` writes a text plus newline to the runtime context's output stream (tests point that stream at a temp file and diff it — the same observation seam plan 3's conformance corpus will use); `print_int` likewise; `words` counts whitespace-separated tokens. Container builtins bridge to Task 6 with type checks on the receiver header (wrong native class traps BOUNDS): multi new/push/get/count/latest (get/latest bounds-trap; push OOM-traps), map new/set/get/has (get of a missing key traps KEY; set drops a replaced old value via the Task-7 dispatcher — closing the loose end Task 6 left open; new decodes its two kind nibbles from the immediate). DB_STUB traps DB with "engine not linked" — the spec's parse-but-trap story for SQL statements. TRAP raises its immediate as the trap code.

- [x] Failing tests: print/print_int output captured and diffed; multi push/count/latest and map set/get/has driven from bytecode; missing map key traps KEY; empty-multi latest traps BOUNDS; words counts correctly; DB_STUB traps DB.
- [x] Implement; green. (CONCAT/EQS text ops wired here too — same test file covers them.)
- [x] Record commit draft: `feat(runtime): builtins (now/print/print_int/words/multi_*/map_* with replaced-value drop) + DB_STUB (T_DB engine-not-linked) + TRAP; test_builtin with captured-stdout diffs.`

### Task 15: ICALL — interface dispatch

**Files:** modify `runtime/src/vm.c`; test `runtime/test/test_icall.c`.

**Concept & reason:** spec section 2's structural interfaces reach the VM as data: the loader already produces sorted (class, slot, method) triples. ICALL checks the receiver (null/native → BOUNDS), binary-searches the triple table by the receiver's class and the instruction's slot id, and on a hit performs exactly the CALL sequence at the same window base (receiver already sits in slot A = callee's `self`). A miss — class doesn't implement the interface — traps BOUNDS with a "no vtable entry" message; the compiler's type checker makes that unreachable in compiled code, the VM keeps it as defense (spec section 6).

- [x] Failing tests: two classes implementing one interface through different methods return different results via the same ICALL site (real dynamic dispatch, chosen by receiver class); a class without a vtable entry traps with the no-vtable message.
- [x] Implement; green both flavors.
- [x] Record commit draft: `feat(runtime): ICALL — binary search over sorted (class,slot,method) vtable triples, same window convention as CALL, defensive no-vtable trap; test_icall.`

### Task 16: `wovm` CLI, smoke test, just recipes, final gate

**Files:** create `runtime/src/main.c`, `runtime/test/mkwob.c`, `runtime/test/cli_smoke.sh`; modify `runtime/Makefile` (wovm + mkwob targets), `justfile`.

**Concept & reason:** make the VM a real program with the exit-code contract plans 2–3 script against: usage or load failure → exit 2 with the loader's message; a trap → exit 1 with one stderr line in the fixed shape "trap CODE in METHOD at line N: MESSAGE"; success → exit 0. The heap cap defaults to 64 MiB, overridable via a `WO_HEAP_MB` environment variable (the spec's configurable-arena note). A tiny generator tool built from the test-side assembler writes two fixture files: a hello program (print text, print int) and a trapping program — the smoke script builds everything, runs both, diffs stdout for the first, and asserts exit code and stderr shape for the second. Root justfile gains `wovm-build` and `wovm-test` (the full make gate: unit suite + ISO flavor + CLI smoke). The final step runs that whole gate as this plan's acceptance run.

- [x] Failing smoke: script exists and fails (no binary yet).
- [x] Implement CLI + generator + recipes; smoke green.
- [x] Run the full gate: `just wovm-test` — every unit test, the ISO dispatch flavor, and the CLI smoke, all ASan-clean. (13 suites × 2 flavors, 327 checks each, + smoke: all green.)
- [x] Record commit draft: `feat(runtime): wovm CLI (exit 0/1/2 contract, trap line format, WO_HEAP_MB) + mkwob fixture generator + cli_smoke.sh; justfile wovm-build/wovm-test full gate.`

---

## Plan self-review notes

- **Spec coverage (plan-1 slice):** header layout, borrow word, hybrid residual checks, `@gc` RC + budgeted no-pause cycle collection, deterministic owned drops, drop-map unwinding with structured errors, containers, structural-interface dispatch, DB_STUB story, computed-goto + ISO dispatch, arena OOM-as-trap — all mapped to tasks above. Deliberately out of scope here (plan 2/3): everything OCaml, ownership inference, `.wo` parsing, conformance corpus, single-binary append trick, parity harness.
- **Order rationale:** memory model before interpreter (Tasks 3–8 testable without bytecode), assembler before loader (Task 10's tests need images), loader before interpreter (validation contract is what keeps the hot loop check-free).
- **Known accepted simplifications, all documented in their tasks:** map linear scan; cycle-collector component-atomic budget; SETF no auto-drop; big-endian unsupported.

## Execution note

Executor needs only gcc/make/just (present on the dev box). Valgrind optional; ASan is the gate. OCaml is NOT needed for this plan.
