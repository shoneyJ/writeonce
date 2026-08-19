# Status board — what is done, what is next

The single place to learn where this project stands. Organised in six buckets:
**stories** (the narrative arc), **in progress**, **done**, **pending**,
**discarded**, **learnings**. The buckets are **sections of this board, not
folders** — a doc stays where it was authored when its work lands; only its
banner and this board change. Every plan and phase doc opens with a
`> **Status:**` banner linking back here; normative contracts
(`plan/oop-vm/`), exploration studies, reference docs and the
discarded/learnings registers carry none by design.

Update this board in the same change that finishes work — move the item to done
with _what actually landed_, set the next in-progress item, and record any
rejection in [`discarded.md`](plan/discarded.md) with its reason.

Statuses: ✅ **done** · 🔄 **in progress** · ⬜ **pending** · ⏸ **hold**

---

## ▶ NEXT PLAN

**Make log-watcher executable — nothing else.** The compile-and-run half of the
language track is met (2026-08-14): the sample compiles with zero diagnostics,
`woc build` produces a 106 KB standalone binary, and all three modes work —
`watch` alerts on a live file, `run` schedules a cron.d entry, `mcp` answers
JSON-RPC with all four tools returning `isError:false`. `just log-watcher`
gates it: 6 checks, 0 failures.

What is left is the difference between "it runs" and "you can leave it
running", and every item below came from a measurement on the sample itself:

1. ~~The ownership pass does not know what the stdlib returns~~ — **done
   2026-08-14**. The root cause was deeper than the table: `Text` was
   classified Copy, so no Text local was ever dropped. `Text` is now an owned
   heap value that is **copied at every ownership boundary** (container, field,
   return, binding, loop cursor), the ownership pass reads the stdlib, builtin
   and static tables, and `fs.read_all`/`net.read` no longer mis-size a short
   read's buffer. Measured: `run` **1 051 040 B → 2 112 B**, `watch`
   **128 B → 64 B**; what remains is items 2 and 3 below, by stack.
2. ~~A projected temporary is never dropped~~ — **done 2026-08-14**. The
   projection was one of six shapes with no owner: a call result compared
   against `nil`, an argument the callee only borrows, a container read's
   copy, a loop's iterable, a projected record, and any of those escaped by a
   `return` from inside the statement that built them. Measured: `run`
   **2 112 B → 64 B** and flat from 8 s to 20 s, the full MCP mix
   **21 312 B / 63 → 64 B / 1**, every handler flat from 2 to 6 requests. The
   64 bytes left are item 3, on every path.
3. ~~The runtime leaks its own argv container~~ — **done 2026-08-14**. The
   entry only borrows its arguments, so `main.c` releases the container it
   built, after the entry returns and after a trap alike. **All three modes
   now report ZERO leaks under ASan** — `watch`, `run`, and the full MCP mix —
   which is the clean baseline item 6's soak needs to read against.
4. ~~A stopping program does not stop~~ — **done 2026-08-14**. A blocking
   call that parks (`net.accept`, socket read/write, `time.sleep`, a child
   wait) now ends the program when it is interrupted with the stop flag set,
   instead of restarting the syscall. A stop is not a trap: `try` cannot
   swallow it, and the stack unwinds through the same drop machinery, so the
   exit is clean and leak-free in every mode. It also uncovered a real
   double-free: an **assignment** of a Text place was a move, not a copy, so
   `api_key = j.mcp.apiKey` aliased the record — `let` copied, assignment now
   does too. `just log-watcher` is 7 checks; the seventh is the stop.
5. ~~The MCP server never closes an accepted connection~~ — **done
   2026-08-15**. `net.close` on every path out of a serve iteration (and the
   listener on stop). Measured: 4 → 4 descriptors across 200 requests, was
   one leaked per request.
6. ~~Nothing soaks~~ — **done 2026-08-15**. `LW_SOAK=<seconds>` drives all
   three modes under load and fails on resident growth past 256 KiB or any
   descriptor growth. The soak immediately caught what every seconds-long
   check missed: ~1.6 MiB/min of **in-arena** leaks the ASan report cannot
   see (the arena is one allocation to LeakSanitizer). Five bugs fell out:
   json decode's worst-case string sizing (free lists poisoned by relabeled
   lengths), `!=` never dropping fresh operands, Int interpolation segments
   mistaken for borrows, `json.encode(Ctor{...})`'s unowned argument, and
   discarded statement results (`pop(lines);`). After: release soak 30 s per
   mode — watch 0, run 0, mcp +20 KiB, descriptors flat; ASan build flat at
   14 600 KiB across 601 686 requests in 90 s once past its ~1200-request
   quarantine warm-up.

Plan: [`plan/compiler/2026-08-14-logwatcher-executable.md`](plan/compiler/2026-08-14-logwatcher-executable.md) ·
Story slice: [`docs/stories/language-runtime-database/07-logwatcher-proof.md`](stories/language-runtime-database/07-logwatcher-proof.md)

**Deferred by name, with the measurement that says so:**

- Iteration 5's *strictness* half — **`?T` forced handling landed 2026-08-18**
  (WO-E211/212/213 + local narrowing; the samples were updated to the
  bind-then-narrow idiom and stay green), **reject rows landed 2026-08-18**
  (WO-E105 doctrine diagnostics — `super.f()` used to compile clean). Still
  open: `pub(read)` write enforcement, `using`, `#if`. Plan 8 stays open.
- Everything `@gc`: iteration 7b, `set`'s `@gc` retention gap, iteration 4's
  `gc/held-cycle` leak. The sample declares **no `@gc` class** — 35 classes,
  none with the gc flag, 0 `RC_INC`/`RC_DEC` against 78 `DROP`s — so none of it
  can affect this workload.
- Iterations 8–12 (shard-actor runtime, database engine, `@table`/query, HTTP
  layer, fibers, blue-green): unchanged, and unblocked by this plan.

The project is the **language track**: iterations 3 → 4 → 5 → 6 → 7, ending at
_compile and run log-watcher_, then the database engine (9/9b) and beyond. (The
prior Rust `wo` runtime was removed from the repo 2026-08-18 — see
[`discarded.md`](plan/discarded.md).)

---

## Stories

[`docs/stories/language-runtime-database/`](stories/language-runtime-database/00-story.md)
— one language, one runtime, one database, one binary. Twelve iterations, each
an unsplittable slice with Given/When/Then acceptance and a pointer to the plan
that sequences its tasks. Read one, approve, then the next starts.

| #   | Iteration                                                                                    | State                        |
| --- | -------------------------------------------------------------------------------------------- | ---------------------------- | ---- |
| 1   | [Principles doc](stories/language-runtime-database/01-principles-doc.md)                     | ✅                           |
| 2   | [VM core (`wovm`)](stories/language-runtime-database/02-vm-core.md)                          | ✅                           |
| 3   | [Compiler front (`woc`)](stories/language-runtime-database/03-compiler-front.md)             | ✅ (known gaps below)        |
| 4   | [Single binary end-to-end](stories/language-runtime-database/04-single-binary-e2e.md)        | ✅ (known gaps below)        |
| 5   | [Language surface](stories/language-runtime-database/05-language-surface.md)                 | 🔄 grammar done; **`?T` forced handling ✅ + reject rows ✅ + WO-E205 ✅ (2026-08-18)**; `pub(read)`/`using`/`#if` still ⏸ |
| 6   | [Program mode + stdlib](stories/language-runtime-database/06-program-mode-stdlib.md)         | ✅ (the surface log-watcher uses) |
| 7   | [log-watcher proof](stories/language-runtime-database/07-logwatcher-proof.md)                | 🔄 **runs; executable in progress** |
| 7b  | [Inferred GC + mark-sweep](stories/language-runtime-database/07b-inferred-gc-mark-sweep.md)  | ✅ **landed 2026-08-18** — `@gc` gone (WO-E104), GC-ness inferred, RC replaced by incremental mark-sweep, `.wob` v4; supersedes iteration 2's RC memory model |
| 8   | [Shard-actor runtime](stories/language-runtime-database/08-shard-actor-runtime.md)           | ⬜                           |
| 9   | [Database engine](stories/language-runtime-database/09-database-engine.md)                   | 🔄 engine complete (storage/WAL/indexes/insert-update-delete); reads land with 9b |
| 9b  | [`@table`, relations, query](stories/language-runtime-database/09b-table-relations-query.md) | 🔄 query surface + relations + FK done (branch query-surface); group-by parked |
| 9c  | [Cross-program tables](stories/language-runtime-database/09c-cross-program-tables.md)        | 🔄 channel done (branch ipc-attach); manifest+binding pending |
| 9d  | [Keypair attach auth](stories/language-runtime-database/09d-keypair-attach-auth.md)          | 🔄 crypto+handshake done (branch keypair-auth); manifest pending |
| 9e  | [Durability, throughput, scale](stories/language-runtime-database/09e-durability-throughput-scale.md) | ⬜ needs a spec first        |
| 9f  | [io_uring group-commit](stories/language-runtime-database/09f-io-uring-commit.md)            | ⬜ after 8 + 9e              |
| 9g  | [Query grammar corpus](stories/language-runtime-database/09g-query-grammar-corpus.md) | ⬜ needs a spec first        |
| 10  | [HTTP service layer](stories/language-runtime-database/10-http-service.md)                   | ⬜                           | Hold |
| 11  | [Fibers](stories/language-runtime-database/11-fibers.md)                                     | ⬜                           | Hold |
| 12  | [Blue-green deploy](stories/language-runtime-database/12-blue-green-deploy.md)               | ⬜                           | Hold |
| 13  | [Compile-time metaprogramming](stories/language-runtime-database/13-compile-time-metaprogramming.md) | ⬜ needs a spec first        |
| 14  | [skillhost host workload](stories/language-runtime-database/14-skillhost-host-workload.md) | ⬜ gaps recorded (branch query-grammar found skillhost needs no new query grammar); each gap a candidate iteration |
| 15  | [deps: `wo.toml [deps]`](stories/language-runtime-database/15-deps-package-manager.md) | ✅ **landed 2026-08-18** (branch web-framework): [deps] inline tables, git-binary fetch, wo.lock pinning, offline-when-locked, --update-deps, WO-E106/E107; `just deps-accept` 8/0 |
| 16  | [web framework](stories/language-runtime-database/16-web-framework.md) | ✅ **landed 2026-08-19** — writeonce-framework (HTTP/1.1 + router + Handler/Middleware) consumed by web-app through [deps]; `just web-app` 14/0; h2c parked (§C) behind 8/9f/11 |
| 17  | [library projects + `internal/`](stories/language-runtime-database/17-library-projects-internal.md) | ⬜ **forks settled 2026-08-20, awaiting spec/plan**: kind = "library" manifest key; Go internal/ rule, dep-boundary-only; lib+bin dual; VM/GC untouched by design (impact analysis in the iteration) |

---

## In progress

| Track    | Item                                                                        | Where                                                      |
| -------- | --------------------------------------------------------------------------- | ---------------------------------------------------------- |
| Language | Iteration 7 — make log-watcher executable (leaks, stop signal, fd lifetime, soak) | [executable plan](plan/compiler/2026-08-14-logwatcher-executable.md) |

Off-critical-path work is parked by explicit scope directive (2026-08-08).

### Landed 2026-08-14 — the compile-and-run milestone

One session, driven end to end by compiling `docs/examples/log-watcher` and
watching its diagnostic count fall (481 → 0). In order:

- **let annotations, container literals, statics, `pub(read)`** — `let x: multi
  Text = []`, `map<K, V>`, `?T`; `[]`/`[a, b]`/`{}` as expressions; `static
  const`/`static fn` with `Cls.fn(...)` calls; a `;` ends a statement so
  one-line guard bodies parse.
- **try/catch over the trap system** (plan 8 Task 5) — VM catch frames
  (`TRY`/`ENDTRY`), unwind-to-handler with the try region's own values
  released, `err_fill` for the `{code, line, method, msg}` record, expression
  and block catch arms. Uncaught traps unchanged.
- **`nil` + 23 text/container builtins** — len, byte_at, print_err,
  starts_with/ends_with, index_of/last_index_of, substr, trim, to_lower,
  char_of, parse_int, split/split_ws, join, slice, pop/shift, sort, reverse,
  remove, key_at/val_at, multi_set.
- **`for k, v in m`** over a map, and `m[i] = v` for a `multi`.
- **the systems stdlib's OS half** (`runtime/src/sysio.c`) — fs, time, env,
  net, proc behind the reserved module names, with predeclared `Stat`,
  `TimeParts` and `Proc` records and the new `WO_T_IO` trap.
- **json** (`runtime/src/json.c`) + **`.wob` v2** — per-field names, referenced
  classes and element kinds in the class table, so encode/decode are one
  metadata-driven implementation; `json.decode(t) as T` is the language's only
  cast, yielding `?T`.
- **program mode** — `fn main(args: multi Text) -> Int`, argv delivered by the
  runtime, return value as the exit code.
- **two safety fixes found by running it**: `+` on `Text` was lowering to ADD
  on two heap pointers (now WO-E201 pointing at `..`; seven sites in the sample
  were corrected), and `x == nil` was lowering to EQS, which dereferences the
  zero word (now EQ).

Gates at the end of that session: corpus 71/0, `woc` runtest 565/0, every
`wovm` unit gate green in both dispatch flavors.

---

## Done

### Language track — compiler + VM (OOP track)

| Status | Item                                 | Doc                                                              | What actually landed                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          |
| ------ | ------------------------------------ | ---------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| ✅     | Principles                           | [`../00-principles.md`](00-principles.md)                        | 13 principles, each with a why and a link to the doc that enforces it                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                         |
| ✅     | `wovm` VM core                       | [plan 1](superpowers/plans/2026-08-01-wob-format-and-vm-core.md) | `.wob` v1 loader with full static validation, register interpreter (computed-goto + ISO-C fallback), arena with size-class free lists, borrow word, RC + budgeted Bacon–Rajan cycle collector, drop-map trap unwinding, containers, builtins, ICALL, CLI. 13 suites × 2 dispatch flavors + CLI smoke, ASan/UBSan clean                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                        |
| ✅     | `.wob` format contract               | [`oop-vm/00-wob-format.md`](plan/oop-vm/00-wob-format.md)        | Normative; twinned with `runtime/src/wob.h`                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   |
| ✅     | `woc` compiler front                 | [plan 2](plan/compiler/2026-08-01-woc-compiler-front.md)         | Tasks 1–8: dune scaffold, `diag` (WO-E codes, two-site related errors, ordered dedup), newline-significant lexer at rt parity, declaration + statement/expression parser with skip-on-block and multi-error recovery, typechecker (field kinds, `?T` plumbing, W201, E225, E214), MVS ownership pass with the four emitter tables, driver with directory discovery + cross-file programs. 14 + 264 checks                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                     |
| ✅     | Error catalog                        | [`oop-vm/01-error-catalog.md`](plan/oop-vm/01-error-catalog.md)  | 14 emitted codes + 10 reserved, each with the reason it is not yet emitted                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    |
| ✅     | log-watcher `.wo` sample             | [`../examples/log-watcher/`](examples/log-watcher/README.md)     | Eight-file port authored docs-first with its `.hx` mapping table; compiles for real in iteration 7                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            |
| ✅     | Scalar cleanup                       | [`discarded.md`](plan/discarded.md)                              | `Money`/`SKU`/`Float` and the abstract allowlist removed; `abstract` flipped adopt → reject                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   |
| ✅     | `woc` emitter, corpus, single binary | [plan 3](plan/compiler/2026-08-01-wob-emit-e2e-single-binary.md) | Tasks 1–6 + 8 (Task 7, a parity harness against the Rust runtime, **deferred by explicit user decision** — the two stacks diverge by design). Bytecode emitter (`emit.ml`) + disassembler (`disasm.ml`, `--dump-bc`); three-kind conformance harness (`scripts/oop-e2e.sh`, `just oop-e2e`) over `tests/corpus/{run,compile-fail,trap,gc}`; pricing-demo + ownership/trap corpora (19 fixtures); `@gc` cycle collector's post-exit pump (`WO_GC_BUDGET`/`WO_GC_TRACE`) + 2 gc fixtures (`gc/held-cycle` retired — see criterion-3 closure below); `woc build` single-binary output + relocation/corrupt-trailer smoke; `WO-E405` closing criterion 3's ASan leak (entry must return `Int`); `just oop-accept` wiring all five spec criteria + both unit gates into one command. 14 + 399 compiler checks; `oop-e2e` 25/25 against the release `wovm`. **Milestone-1 acceptance gate is fully green — all five criteria met** (see the dated acceptance note in `docs/superpowers/specs/2026-08-01-oop-compiler-vm-design.md`) |

**Known gaps carried out of iteration 3** — recorded, not silently owed:

- **`?T` is plumbed but unenforced.** Lexer/token/AST/parser/dump all handle
  `?T`; the semantics do not exist (`WO-E211`/`E212`/`E213` declared, never
  emitted — a probe returning `?Int` as `Int` exits 0). Owned by iteration 5,
  plan 8 Task 6, which is that iteration's first task because it blocks the
  log-watcher port. See [`compiler/nullable-types-implementation.md`](plan/compiler/nullable-types-implementation.md).
- **Structural interface satisfaction is not checked** (`WO-E205` dead), along
  with type mismatch, bad arity, and unknown-fn (`E201`/`E203`/`E204`) — all
  named in plan 2 Task 6's own must-fail list. Gaps in shipped work, catalogued
  as reserved.
- Six further narrowings (W201 heuristic, E225 reach, dead code after `return`,
  unresolved-callee drops, RC table ordering, residual b-side role) are listed
  in the plan-2 SDD ledger and in the affected files' own comments.

**Known gaps carried out of iteration 4** — recorded, not silently owed:

- ~~`WO-E205` (unsatisfied interface) reachable but unenforced~~ — **closed
  2026-08-18** (branch `type-enforcement`): structural satisfaction is checked
  at call arguments, annotated `let`s, and returns; the pinned fixture moved
  to `compile-fail/unsatisfied-interface` with `fixture.code WO-E205` in the
  same change, as its comment demanded. The hybrid boundary is restored.
- **`set(m, k, v)`'s `@gc` retention gap on map keys/values is open** — the
  twin of the `push` bug Task 5 fixed for `multi`. `set` has no equivalent
  special case in `owner.ml`'s `analyze_call`, so a `@gc` key or value handed
  to `set` is under-counted and the collector can free it while the map still
  points at it. Nothing in the corpus exercises this yet. See
  [`oop-vm/08-builtin-surface.md`](plan/oop-vm/08-builtin-surface.md).

**Known gaps carried out of the 2026-08-14 compile-and-run milestone** —
recorded, not silently owed:

- **Optionals are lenient.** `?T` has its representation (the zero word) and
  its comparisons, but `WO-E211`–`E213` are still dead: a `?T` may be used
  where `T` is required, and nothing narrows inside an `if x != nil` branch.
  The workload leans on that leniency today.
- **`pub(read)` is parsed, not enforced.** The marker rides on the field
  (`Ast.field.pub_read`); no check refuses a write from outside the declaring
  class yet.
- **`using` extensions and `#if` build flags are absent**, and the reject rows
  (`extends`/`cast`/`Dynamic`/…) still have no doctrine-citing diagnostics —
  plan 8 Tasks 7–8's remainder.
- ~~A borrowed non-constant Text pushed into a container is a double-free
  hazard~~ — **closed 2026-08-14 by copy-on-push**: `push`/`set`/`m[i] = v`
  copy a TEXT element, key or value into the container, and the compiler drops
  a *freshly built* Text right after the call (a value read out of a place
  keeps its owner). The failure it fixed was real: a `tools/call` of `tail_log`
  used to answer `{"isError":true,"text":"tool failed: not a text value"}`; all
  four MCP tools now return `isError:false` with correct payloads.
  `OWNED`/`GCREF` elements still move, and `set`'s `@gc` retention gap is still
  open (see [`oop-vm/08-builtin-surface.md`](plan/oop-vm/08-builtin-surface.md)).
- **A blocking `accept`/`read` swallows SIGTERM.** `env.stopping()` installs a
  handler that only sets a flag, and `net.accept`/`net.read` retry on `EINTR`,
  so a server parked in `accept` never observes it: a plain TERM does not stop
  the process (`timeout -k` / `kill -9` does). Graceful shutdown needs an
  interruptible wait — the shard-actor runtime's event loop (iteration 8) is
  where that belongs, not a patch to the blocking calls.
- **A temporary record whose field is iterated is never dropped** —
  `for e in parse_dir(dir).entries` keeps the entries alive (good) but leaks
  the `ParseResult` shell (its drop is recorded for no register). Found in the
  same disassembly; a leak, not a corruption.
- ~~json's two documented limits~~ — **closed 2026-08-18** (branch
  `json-fidelity`): a `Bool` field encodes `true`/`false` (WOB_FIELD_BOOL /
  WOB_FIELD_NIL_BOOL in the field metadata), and a fraction/exponent is
  malformed for an Int field — the checked decode yields nil instead of
  truncating (floats stay representable via a raw `json.Value` field).
- **`net` fd lifetime is the program's problem.** `net.close` exists; the
  sample's MCP server never calls it, so a long-running `mcp` session leaks
  descriptors. That is the sample's bug to fix, not the runtime's.
- **The workload has never run under ASan**, and iteration 4's `gc/held-cycle`
  leak (above) is still open. The corpus itself stays ASan-clean.
- ~~`json.encode` Bool/nil-scalar asymmetry~~ — **closed 2026-08-18** with the
  same change: `Bool` encodes `true`/`false`, `?Bool` nil encodes `null`.
- **No corpus fixtures cover the new surface.** By explicit direction
  (2026-08-14) the acceptance for this work is the log-watcher program itself,
  not fixture pairs; `tests/corpus/` still gates every pre-existing behavior
  (71 checks, 0 failures).
- **E201/E203 and seven other `WO-E2xx` codes remain declared but unemitted**
  — see [`oop-vm/01-error-catalog.md`](plan/oop-vm/01-error-catalog.md).
- **CLOSED — milestone-1's ASan gate (`just oop-accept`) failing on
  `gc/held-cycle`.** Root cause (Task 8's finding, restated): `main.c`'s
  entry-method return value (`uint64_t ret`, `src/main.c:158`) is stored
  but never released, so `gc/held-cycle`'s "permanent external hold" was
  actually a permanent refcount inflation — LeakSanitizer's "definite
  leak" (1184 bytes / 3 allocations) was correctly reporting exactly
  that, not a false positive. Fixing it by releasing `ret` was rejected:
  the `.wob` method table carries no return-type/kind metadata, so
  `main.c` has no way to know `ret` is a pointer rather than a scalar,
  and adding that metadata is a format change out of scope here. Fixed
  instead at the source: the systems-track spec already requires the
  entry to return `Int` (its return value is the process exit code), so
  a class-returning `main` was never legal — `WO-E405`
  (`compiler/src/emit.ml`, `01-error-catalog.md`) now rejects it at
  compile time, and `gc/held-cycle` is retired because its premise (an
  externally-held cycle survives a _post-exit_ pump) is no longer
  expressible — see `oop-vm/02-corpus.md`'s "Retired" note for why, and
  for where the scenario it meant to cover is actually proven
  (`runtime/test/test_cycle.c`, plus a proper in-flight fixture scheduled
  for story iteration 7b). Spec success criterion 3 is now **MET**;
  `just oop-accept` passes all five criteria.

The C proving-ground work (`exploration/c-runtime/`, phases A–F: 859k reads/s,
618k durable commits/s) fed the current C runtime and remains as an
[exploration study](plan/exploration/c-runtime/00-plan.md).

---

## Pending

### Implementation order (sequenced 2026-08-20)

Dependency-derived order for everything not yet landed. Rules that force it:
9f explicitly after 8 + 9e; 9c "precedes iteration 10"; 9d's plan folds into
9c's; 12 only after 9 + 10 (catalog to diff, HTTP to build on); 11 rides 8's
shard scheduler; h2c parked behind 8/9f/11; the post-12 parked list stays
parked by the 2026-08-08 scope directive.

1. **7 finish** — in progress (executable plan: leaks, stop signal, fd
   lifetime, soak); the locked critical path ends here.
2. **17** — forks settled, compiler-driver-only, no runtime deps; kills the
   framework's `--emit` wart while iteration 16 is fresh. (Merge the
   `web-framework` branch first.)
3. **9g** — likely collapses to "confirm `len(query)` + add `exists`"; the
   skillhost corpus already showed no new grammar; do before 14 consumes it.
4. **14** — skillhost port, the next driving workload (log-watcher's role for
   host capabilities); stdlib-shaped, independent of shards;
   bounded-subprocess gap first.
5. **9c then 9d** — finish the half-done branches (ipc-attach: manifest +
   binding; keypair-auth: manifest) while DB context is warm; 9d folds into
   9c's plan; both must precede 10.
6. **9e** — the measurement backbone; baseline single-shard BEFORE the
   runtime restructure so 8/9f have numbers to sign against.
7. **8** — shard-actor; the big structural move; precondition (collector
   settled, 7b) already met; unblocks 9f, 11, h2c.
8. **9f** — io_uring group-commit; explicitly after 8 + 9e.
9. **11** — fibers on the shard scheduler; keeps VM-core work contiguous
   with 8/9f (same dispatch/io seams).
10. **10** — HTTP service layer; after 9c by its own precedence note;
    lowers `service` blocks onto the iteration-16 framework.
11. **12** — blue-green; prerequisites 9 + 10 now exist.
12. **13 + parked drain** — metaprogramming (spec first), then h2c,
    group-by, `pub(read)`/`using`/`#if`, ADT roster, WO-E225 — all held
    behind 12 by the scope directive.

### Language track — sequenced, on the critical path

| #   | Item                                                                                                                                                                           | Plan                                                                                               |
| --- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | -------------------------------------------------------------------------------------------------- |
| 5   | Haxe-parity language surface — **`?T` forced handling first**, then switch expressions, records, enum payloads, try/catch, statics, `using`, modules, `is`, `pub(read)`, `#if` | [plan 8](plan/compiler/2026-08-01-haxe-parity-language.md)                                         |
| 6   | Program mode + systems stdlib — `fn main`, exit codes, `fs`/`proc`/`net`/`time`/`json`                                                                                         | [plan 9](superpowers/plans/2026-08-01-program-mode-stdlib.md)                                      |
| 7   | log-watcher proof — the sample compiles and detects a silent death live                                                                                                        | [plan 10](superpowers/plans/2026-08-01-log-watcher-sample.md)                                      |
| 8   | Shard-actor runtime                                                                                                                                                            | [plan 4](superpowers/plans/2026-08-01-shard-actor-vm-runtime.md)                                   |
| 9   | Database engine binding                                                                                                                                                        | [plan 5](superpowers/plans/2026-08-01-db-engine-binding.md)                                        |
| 9b  | `@table` + relations + language-integrated query — comprehension queries, `ref`/`backlink` navigation, GroupBy aggregates; acceptance: new `docs/examples/employee` sample     | [spec](superpowers/specs/2026-08-15-table-relations-query-design.md) · [plan](plan/compiler/2026-08-15-employee-relations-query.md) |
| 9c  | Cross-program tables — attach to a running program's database (IPC string in wo.toml, manifest-granted rights, owner stays the single writer)                                  | **no spec yet** — four open forks recorded in the iteration; brainstorm before planning            |
| 9d  | Keypair attach auth — mutual challenge–response, grants name public keys, uid superseded                                                                                       | **no spec yet** — four forks recorded; plan folds into 9c's                                        |
| 9e  | Durability + throughput + scale — restart-persistence, read/write benchmark, ~1M rows; the gate every later optimization re-runs                                              | **no spec yet** — four forks recorded; the measurement backbone                                    |
| 9f  | io_uring group-commit write path — batched durability overlapped on shard threads, fsync fallback                                                                             | **no spec yet** — brainstorm after iterations 8 + 9e                                               |
| 9g  | Query grammar from real embedded-DB corpora — whole-query count + correlated exists, driven by the skillhost SQL catalogue; add only what a corpus uses | **no spec yet** — three forks; may collapse to "confirm len(query) + add exists" |
| 14  | skillhost host workload — port skillhost (MCP host + confined script runner) to writeonce; drives the missing host capabilities into the open (bounded subprocess, stdin/stdout transport, fs metadata, FFI-vs-out-of-process) | **no spec yet** — gaps recorded in the iteration; each gap brainstormed on demand, bounded-subprocess first |
| 17  | library projects + dependency privacy — `wo.toml` kind = "library" (checkable without entry, dual lib+bin) + Go-style `internal/` at the [deps] boundary; framework reorg demonstrates both | **forks settled 2026-08-20** — decisions + framework/compiler/VM/GC impact in the iteration; spec/plan next |
| 10  | HTTP service layer                                                                                                                                                             | [plan 6](superpowers/plans/2026-08-01-http-service-layer.md)                                       |
| 11  | Fibers                                                                                                                                                                         | vision §3, [blue-green exploration](plan/exploration/blue-green-vm/00-vision.md)                   |
| 12  | Blue-green deploy                                                                                                                                                              | [spec](superpowers/specs/2026-08-03-blue-green-vm-design.md) — plan authored after iterations 9–10 |

### Language track — parked until after iteration 12

Recorded 2026-08-08 by scope directive; nothing here lands before the
log-watcher proof.

- `WO-W201` `@gc`-suggestion refinement beyond the self-reference heuristic
- `WO-E225` broadened to `ref`/`multi`/`map` element types and fn signatures
- ADT container roster adoption (Stack, Queue, Set, Tree, Graph, …) — see the
  roster in [`compiler/nullable-types-implementation.md`](plan/compiler/nullable-types-implementation.md)
- Web framework as a `.wo` library; UI (`##ui` SSR + live patches);
  script-based destructive migrations; MCP/agent wrapper over the management plane
- `throw` (explicit raise) — cut 2026-08-10, 0 uses in the driving workload
  (log-watcher); catch frames ship without it
- `time.mono` — cut 2026-08-10, 0 uses in the driving workload; returns when a
  workload needs monotonic math
- `is` — cut 2026-08-10, 0 uses in the driving workload; emptied plan 8's old
  Task 7, which is deleted rather than deferred

### Frontend — removed as stale (2026-08-17)

The `##ui` / `.htmlx` LiveView frontend track — 13d pricing UI, the 14-MVC-UI
implementation plan, the 7-of-7 `ui-htmlx-live` plan, and the 9-doc
`plan/exploration/ui/` design set — was **removed**. It was built entirely on
the non-advancing Rust runtime (`.dev/reference/crates/wo-htmlx`, `cargo run`,
WebSocket live-patches) and contradicts the current woc/wovm direction. Recorded
in [`discarded.md`](plan/discarded.md).

---

## Discarded

Settled rejections with their reasons live in [`discarded.md`](plan/discarded.md) —
inheritance, `abstract` newtypes, `Money`/`SKU`/`Float`, `Dynamic`/`cast`/
`macro`/`extern`, AOT-to-C, Menhir, shared mutable engine state, external
deployer daemon, destructive migrations in v1, and more. Argue against the
recorded reason rather than re-opening an entry as new.

## Learnings

What attempts taught, shipped or not, in [`learnings.md`](plan/learnings.md) —
plumbed-is-not-enforced, vacuously-passing goldens, exit-0-with-wrong-output,
the malloc-path ASan trick, deferred checks that never reach the runtime,
validate-once-at-the-boundary, and reference-implement-in-C-first.
