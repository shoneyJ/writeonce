# Status board — what is done, what is next

Edges live in [00-dependency-graph.md](../00-dependency-graph.md) — mermaid
graphs of iteration and feature dependencies; anything with all-green
incoming arrows is startable. This board carries the STATES.

The single place to learn where this project stands. Organised in six buckets:
**stories** (the narrative arc), **in progress**, **done**, **pending**,
**discarded**, **learnings**. The buckets are **sections of this board, not
folders** — a doc stays where it was authored when its work lands; only its
banner and this board change. ONE exception by directive (2026-08-20):
story iteration files move physically — landed ones into
`stories/language-runtime-database/done/`, brainstorm-needing ones into
`refine/`, held ones into `hold/`, the active slice's stories into
`stories/language-runtime-database/in-progress/` (both 2026-08-21);
ready ones stay at the root. Second exception (2026-08-21): the active
slice's one marker doc lives in `docs/in-progress/` and is deleted when
the slice lands. Every plan and phase doc opens with a
`> **Status:**` banner linking back here; normative contracts
(`plan/oop-vm/`), exploration studies, reference docs and the
discarded/learnings registers carry none by design.

Update this board in the same change that finishes work — move the item to done
with _what actually landed_, set the next in-progress item, and record any
rejection in [`discarded.md`](../plan/discarded.md) with its reason.

Statuses: ✅ **done** · 🔄 **in progress** · ⬜ **pending** · ⏸ **hold**

Story files carry YAML frontmatter (`iteration`/`status`/`chain`) — the
machine-readable truth behind this board; live Obsidian Dataview views:
[`board-views.md`](board-views.md) (Kanban = view only, never edits status).

---

## ▶ NEXT PLAN

**The concurrency + fiber chain — ✅ stage 3 → 22 → 31 → 24 → 23 → 32**
(directive 2026-08-21). Active slice: **iteration 22, the measurement
backbone** — spec + plan approved 2026-08-21
([spec](../superpowers/specs/2026-08-21-db-bench-design.md) ·
[plan](../superpowers/plans/2026-08-21-db-bench.md) ·
[marker](../in-progress/2026-08-21-db-bench.md)); the four forks settled
as their leanings, plus `time.ticks` (µs clock) as the one runtime
addition and a new `db-bench` sample as the vehicle.

**Implemented last time (2026-08-21):** the arc's **stage 3 — the
transparent DB actor landed, the arc is COMPLETE** (stories
[8](language-runtime-database/done/08-shard-actor-runtime.md) +
[11](language-runtime-database/done/11-fibers.md) → done/). A worker
shard's DB statement marshals to shard 0 (requester-side slot encode),
executes serialized on the owner, and the fiber resumes with the
materialized reply — `WO_T_DB` off the primary is gone. NEW gate
`just db-actor` 8/0; ASan/TSan clean; WO_DATA pair proves worker writes
are ack-after-durable and replay.

**Key findings:** a latent stage-1 bug — io_uring ring params were ONE
shared static, rewritten by every shard's lazy init while others read
offsets from it: submits landed at garbage offsets and parked fibers
LOST WAKES (~1/20 hangs at default cores). Per-vm params fixed it; a
short `io_uring_enter` submit is now a loud trap. Also: single-binary
gates embed the runtime — rebuild the SAMPLE, not just wovm, or you
debug a stale binary.

**Learned from the last iteration:** the owner thread must never read a
requester's VM heap (concurrent mark-bit writes = TSan race) — marshal
by ENCODING on the requester's thread, execute from slots replay-style;
a plane-less park (`WO_PARK_INBOX`) + envelope wake is all an RPC reply
needs; a busy shard adopting its inbox once per reduction slice bounds
request latency.

**Dependencies unblocked:** 22's multi-shard campaign (the store is
correct under shards now); 24's serving model (fiber-per-connection has
a database it can touch from any shard); the framework ledger rows the
arc gates stay ⏸ until their own slices.

**Next steps:** 22 (baselines single- AND multi-shard + the mutex-inbox
number; precursor recorded in story 8: remote insert ≈8µs/op RAM-only)
→ 31 (lifecycle) → 24 (chat) → 23 (io_uring group-commit) → 32 (WAL
checkpoint). Held tail resumes on its own precedence notes.

**`.dev/reference` used:** `linux` (io_uring uapi struct layouts and the
"single event loop" card — both load-bearing in the ring-params fix).

---

### Landed 2026-08-20 — framework v1 (the previous NEXT PLAN)

**Framework v1 — a polished micro-framework (routing, middleware,
`Req`/`Resp`), nothing MVC-scale.** Directive 2026-08-20: iteration 17
(library kind + `internal/`) is **parked** — spec + plan approved and ready
on branch `library-internal` — and the framework itself is the work. The
v1-polish slice landed the same day (branch `framework-v1`): registration
helpers `get/post/put/delete_` (the take-Handler shape, probe-proven),
405 + `Allow` on wrong-method hits, HEAD served as GET with the body
suppressed, a `Logging` middleware, `set_header`; `just web-app` grew to
16/0. En route it exposed and fixed a real emitter bug: a Text-typed
single-segment interpolation of a place (`"${r.method}"`, `r` a loop
borrow) crossed `let`/assignment boundaries uncopied — aliased the field,
crashed the release build; `copy_place_text` now sees through `Interp`
exactly as `drop_fresh_text` does, pinned by
`tests/corpus/run/interp-borrowed-field`.

**Auth-in-core landed 2026-08-20** (same branch): `http/auth.wo` — header
parsing, pure-`.wo` base64, constant-time `ct_eq`, `req.principal` as the
blessed principal slot (Middleware.before takes `mut req`), `BearerAuth` +
`BasicAuth` middlewares; policy stays app-side. Probe matrix 26/26
ASan-clean; web-app dogfoods BearerAuth. The framework README carries the
core checklist (✅ / candidate / parked-by-design rows).

**Form-encoded bodies landed 2026-08-20** (same branch): `media_type(req)`
+ `form_values(req)` (nil on any other content-type; '+'/%XX decoded);
CreateProduct accepts form OR JSON into one insert path.

**Multipart landed 2026-08-20** (same branch): `http/multipart.wo` —
RFC 7578 fields + file parts, strict malformed-is-nil, `part_named`;
whole-body within BODY_MAX (streaming parks behind 8/11). CreateProduct
takes multipart/form/JSON; `just web-app` **21/0**. Surfaced + fixed the
RETURN flavor of the interp-of-borrowed-place emitter bug (emit_return now
sees through Interp; same corpus pin). Body-parsing hooks: all three ✅.

Scope split (2026-08-20): the surface above plus the remaining transport/
routing/security gaps is **framework v1**, tracked item-by-item in the
[framework README's status ledger](../examples/writeonce-framework/README.md)
(✅/🔶/⬜/⏸/🔧 per feature — timeouts and Unix sockets need `net` runtime
seams, crypto hashes need C builtins since the language has no bitwise
operators, streaming/cancellation park behind 8/11). The memory-rich
features are **framework v2** = iteration 18 (⏸ HELD 2026-08-21 with spec
approved + plan authored intact): TTL cache, @table flags, durable job
queue with drain-on-request, `transaction { }` over the WAL's staged
batch. The pending order is now the concurrency chain (see *Pending*
below). Edges: [00-dependency-graph.md](../00-dependency-graph.md).

---

### Landed 2026-08-15 — the executable milestone (the previous NEXT PLAN)

"Make log-watcher executable" — the difference between "it runs" and "you
can leave it running". Every item came from a measurement on the sample
itself, and all six landed:

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

Plan: [`plan/compiler/2026-08-14-logwatcher-executable.md`](../plan/compiler/2026-08-14-logwatcher-executable.md) ·
Story slice: [`docs/stories/language-runtime-database/done/07-logwatcher-proof.md`](language-runtime-database/done/07-logwatcher-proof.md)

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

The **language track**'s first goal — iterations 3 → 4 → 5 → 6 → 7, _compile
and run log-watcher_ — is met; the database engine (9/9b), deps (15), and the
web framework (16) landed on top of it. The goal is now the framework as a
polished micro-framework (17 parked; see the NEXT PLAN above and
"Implementation order" under Pending). (The prior Rust `wo` runtime was
removed from the repo 2026-08-18 — see [`discarded.md`](../plan/discarded.md).)

---

## Stories

[`docs/stories/language-runtime-database/`](language-runtime-database/00-story.md)
— one language, one runtime, one database, one binary. Twelve iterations, each
an unsplittable slice with Given/When/Then acceptance and a pointer to the plan
that sequences its tasks. Read one, approve, then the next starts.

| #   | Iteration                                                                                    | State                        |
| --- | -------------------------------------------------------------------------------------------- | ---------------------------- | ---- |
| 1   | [Principles doc](language-runtime-database/done/01-principles-doc.md)                     | ✅                           |
| 2   | [VM core (`wovm`)](language-runtime-database/done/02-vm-core.md)                          | ✅                           |
| 3   | [Compiler front (`woc`)](language-runtime-database/done/03-compiler-front.md)             | ✅ (known gaps below)        |
| 4   | [Single binary end-to-end](language-runtime-database/done/04-single-binary-e2e.md)        | ✅ (known gaps below)        |
| 5   | [Language surface](language-runtime-database/done/05-language-surface.md)                 | 🔄 grammar done; **`?T` forced handling ✅ + reject rows ✅ + WO-E205 ✅ (2026-08-18)**; `pub(read)`/`using`/`#if` still ⏸ |
| 6   | [Program mode + stdlib](language-runtime-database/done/06-program-mode-stdlib.md)         | ✅ (the surface log-watcher uses) |
| 7   | [log-watcher proof](language-runtime-database/done/07-logwatcher-proof.md)                | ✅ **landed 2026-08-15** — executable, not merely compilable: zero ASan leaks in all three modes, SIGTERM ends parked syscalls, fds flat, `LW_SOAK` gate; `just log-watcher` 7/0 |
| 7b  | [Inferred GC + mark-sweep](language-runtime-database/done/07b-inferred-gc-mark-sweep.md)  | ✅ **landed 2026-08-18** — `@gc` gone (WO-E104), GC-ness inferred, RC replaced by incremental mark-sweep, `.wob` v4; supersedes iteration 2's RC memory model |
| 8   | [Shard-actor runtime](language-runtime-database/done/08-shard-actor-runtime.md)           | ✅ **landed 2026-08-21** — the arc complete: stages 1+2 (fibers/budget/actors/io_uring plane, shards, envelopes, WO-E222) + stage 3's transparent DB actor (`just db-actor` 8/0, ASan/TSan clean, WAL replay pair) |
| 9   | [Database engine](language-runtime-database/done/09-database-engine.md)                   | 🔄 engine complete (storage/WAL/indexes/insert-update-delete); reads land with 9b |
| 9b  | [`@table`, relations, query](language-runtime-database/done/09b-table-relations-query.md) | 🔄 query surface + relations + FK done (branch query-surface); group-by parked |
| 19  | [Float + Bytes](language-runtime-database/done/19-missing-scalar-types.md) | ✅ **landed 2026-08-20** — `.wob` v5: Float constant tag, field kinds 6/7, opcodes 34-41 (IEEE-quiet f64), builtins 70-83. Full stack: literals, arithmetic, `@table` column, WAL bit-exact replay, json fractions in / shortest-round-trip out, `?Float` reserved-NaN nil, total-order index (NaN last, `-0.0` == `+0.0`), Bytes + base64. No implicit Int/Float mixing (WO-E201); `float`/`trunc` are the only bridges. Proof: web-app price is a real Float (`{"price":9.99}`), `just web-app` 23/0; corpus 103/0 |
| 11  | [Fibers](language-runtime-database/done/11-fibers.md)                                     | ✅ **landed 2026-08-21** with the arc (`just fibers` 10/0); fs-park re-scoped out of v1, disclosed in the story |
| 22  | [Durability, throughput, scale](language-runtime-database/in-progress/22-durability-throughput-scale.md) | 🔄 **spec + plan approved 2026-08-21, executing** — db-bench sample + campaign gates; forks settled |
| 31  | [Actor lifecycle](language-runtime-database/refine/31-actor-lifecycle.md) | ⬜ needs a spec first — third in chain (story written 2026-08-21) |
| 24  | [chat: WebSocket workload](language-runtime-database/refine/24-chat-websocket-workload.md) | ⬜ fourth in chain — the arc's acceptance; after 31 |
| 23  | [io_uring group-commit](language-runtime-database/refine/23-io-uring-commit.md)            | ⬜ fifth in chain, after stage 3 + 22 |
| 32  | [WAL checkpoint](language-runtime-database/refine/32-wal-checkpoint.md)            | ⬜ last in chain, after 23 — disk reclamation + bounded replay (story written 2026-08-21) |
| 20  | [Cross-program tables](language-runtime-database/hold/20-cross-program-tables.md)        | ⏸ hold (2026-08-21); channel done (branch ipc-attach keeps its manifest) |
| 21  | [Keypair attach auth](language-runtime-database/hold/21-keypair-attach-auth.md)          | ⏸ hold (2026-08-21); crypto+handshake done (branch keypair-auth keeps its manifest) |
| 25  | [HTTP service layer](../superpowers/plans/2026-08-01-http-service-layer.md)                   | ⏸ hold (2026-08-21) — story file removed; the plan doc remains |
| 26  | [Blue-green deploy](language-runtime-database/hold/26-blue-green-deploy.md)               | ⏸ hold (2026-08-21)          |
| 27  | [Query grammar corpus](language-runtime-database/hold/27-query-grammar-corpus.md) | ⏸ hold (2026-08-21)          |
| 28  | [skillhost host workload](language-runtime-database/hold/28-skillhost-host-workload.md) | ⏸ hold (2026-08-21); gaps recorded (branch query-grammar found skillhost needs no new query grammar) |
| 29  | [Compile-time metaprogramming](language-runtime-database/hold/29-compile-time-metaprogramming.md) | ⏸ hold (2026-08-21)          |
| 15  | [deps: `wo.toml [deps]`](language-runtime-database/done/15-deps-package-manager.md) | ✅ **landed 2026-08-18** (branch web-framework): [deps] inline tables, git-binary fetch, wo.lock pinning, offline-when-locked, --update-deps, WO-E106/E107; `just deps-accept` 8/0 |
| 16  | [web framework](language-runtime-database/done/16-web-framework.md) | ✅ **landed 2026-08-19** — writeonce-framework (HTTP/1.1 + router + Handler/Middleware) consumed by web-app through [deps]; h2c parked (§C) behind 8/23/11. **v1 polish landed 2026-08-20** (branch framework-v1): get/post/put/delete_ helpers, 405+Allow, HEAD, Logging middleware, set_header; `just web-app` 16/0; fixed the interp-borrowed-field emitter crash en route. **Auth-in-core landed 2026-08-20**: http/auth.wo (Bearer/Basic, ct_eq, req.principal), web-app dogfoods BearerAuth, gate 17/0 |
| 17  | [library projects + `internal/`](language-runtime-database/done/17-library-projects-internal.md) | ✅ **landed 2026-08-20** — `kind = "library"` in `wo.toml` (default `program`, so every existing manifest is byte-identical; unknown value = WO-E109 exit 2); `woc <dir>` on a library runs the FULL pipeline entry-less and writes nothing, retiring iteration 16's `--emit` workaround; the no-entry build error names the kind; lib+bin dual works. Go's `internal/` rule as **WO-E108** at the consumer's own `use`, dep-boundary-only — the library imports its own interior freely. Framework reorganized: `internal/{parse,serve}.wo` behind the line, `http/form.wo` split out to keep `media_type`/`form_values` public. Driver-only change; VM/`.wob`/GC untouched. `just web-app` **26/0** (3 new checks), every standing gate unchanged |
| 18  | [framework v2: memory-rich features](language-runtime-database/hold/18-memory-db-features.md) | ⏸ hold (2026-08-21); spec approved + plan authored, both held intact ([spec](../superpowers/specs/2026-08-20-memory-db-features-design.md), [plan](../superpowers/plans/2026-08-20-framework-v2-memory-features.md)): TTL cache + @table flags + durable job queue (drain-on-request) + `transaction { }` over the WAL's staged batch; pub/sub rejection expired with the arc (8/11 landed 2026-08-21) — revisit on unhold |

---

## In progress

| Track    | Item                                                                        | Where                                                      |
| -------- | --------------------------------------------------------------------------- | ---------------------------------------------------------- |
| Runtime  | **iteration 22: db-bench** — spec + plan approved 2026-08-21; executing | [marker](../in-progress/2026-08-21-db-bench.md) · [plan](../superpowers/plans/2026-08-21-db-bench.md) |

The active slice's marker doc lives in [`in-progress/`](../in-progress/) —
one file, deleted when the slice lands. Everything else pending is the
concurrency chain (see *Pending* below); the held tail is in `hold/`.

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
| ✅     | Principles                           | [`../00-principles.md`](../00-principles.md)                        | 13 principles, each with a why and a link to the doc that enforces it                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                         |
| ✅     | `wovm` VM core                       | [plan 1](../superpowers/plans/2026-08-01-wob-format-and-vm-core.md) | `.wob` v1 loader with full static validation, register interpreter (computed-goto + ISO-C fallback), arena with size-class free lists, borrow word, RC + budgeted Bacon–Rajan cycle collector, drop-map trap unwinding, containers, builtins, ICALL, CLI. 13 suites × 2 dispatch flavors + CLI smoke, ASan/UBSan clean                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                        |
| ✅     | `.wob` format contract               | [`oop-vm/00-wob-format.md`](../plan/oop-vm/00-wob-format.md)        | Normative; twinned with `runtime/src/wob.h`                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   |
| ✅     | `woc` compiler front                 | [plan 2](../plan/compiler/2026-08-01-woc-compiler-front.md)         | Tasks 1–8: dune scaffold, `diag` (WO-E codes, two-site related errors, ordered dedup), newline-significant lexer at rt parity, declaration + statement/expression parser with skip-on-block and multi-error recovery, typechecker (field kinds, `?T` plumbing, W201, E225, E214), MVS ownership pass with the four emitter tables, driver with directory discovery + cross-file programs. 14 + 264 checks                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                     |
| ✅     | Error catalog                        | [`oop-vm/01-error-catalog.md`](../plan/oop-vm/01-error-catalog.md)  | 14 emitted codes + 10 reserved, each with the reason it is not yet emitted                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    |
| ✅     | log-watcher `.wo` sample             | [`../examples/log-watcher/`](../examples/log-watcher/README.md)     | Eight-file port authored docs-first with its `.hx` mapping table; compiles for real in iteration 7                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            |
| ✅     | Scalar cleanup                       | [`discarded.md`](../plan/discarded.md)                              | `Money`/`SKU`/`Float` and the abstract allowlist removed; `abstract` flipped adopt → reject                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   |
| ✅     | `woc` emitter, corpus, single binary | [plan 3](../plan/compiler/2026-08-01-wob-emit-e2e-single-binary.md) | Tasks 1–6 + 8 (Task 7, a parity harness against the Rust runtime, **deferred by explicit user decision** — the two stacks diverge by design). Bytecode emitter (`emit.ml`) + disassembler (`disasm.ml`, `--dump-bc`); three-kind conformance harness (`scripts/oop-e2e.sh`, `just oop-e2e`) over `tests/corpus/{run,compile-fail,trap,gc}`; pricing-demo + ownership/trap corpora (19 fixtures); `@gc` cycle collector's post-exit pump (`WO_GC_BUDGET`/`WO_GC_TRACE`) + 2 gc fixtures (`gc/held-cycle` retired — see criterion-3 closure below); `woc build` single-binary output + relocation/corrupt-trailer smoke; `WO-E405` closing criterion 3's ASan leak (entry must return `Int`); `just oop-accept` wiring all five spec criteria + both unit gates into one command. 14 + 399 compiler checks; `oop-e2e` 25/25 against the release `wovm`. **Milestone-1 acceptance gate is fully green — all five criteria met** (see the dated acceptance note in `docs/superpowers/specs/2026-08-01-oop-compiler-vm-design.md`) |

**Known gaps carried out of iteration 3** — recorded, not silently owed:

- **`?T` is plumbed but unenforced.** Lexer/token/AST/parser/dump all handle
  `?T`; the semantics do not exist (`WO-E211`/`E212`/`E213` declared, never
  emitted — a probe returning `?Int` as `Int` exits 0). Owned by iteration 5,
  plan 8 Task 6, which is that iteration's first task because it blocks the
  log-watcher port. See [`compiler/nullable-types-implementation.md`](../plan/compiler/nullable-types-implementation.md).
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
  [`oop-vm/08-builtin-surface.md`](../plan/oop-vm/08-builtin-surface.md).

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
  open (see [`oop-vm/08-builtin-surface.md`](../plan/oop-vm/08-builtin-surface.md)).
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
  — see [`oop-vm/01-error-catalog.md`](../plan/oop-vm/01-error-catalog.md).
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
[exploration study](../plan/exploration/c-runtime/00-plan.md).

---

## Pending

### Implementation order (re-sequenced 2026-08-21 — concurrency chain)

Everything still pending IS the runtime-concurrency chain. Basis: the
2026-08-20 code-review pass (measure before optimizing, close correctness
holes before adding surface), amended 2026-08-21 by developer decision:
**stage 3 before 22** — correctness first, then one benchmark campaign
covers single- and multi-shard. The authoritative table with per-row
reasoning is [`00-story.md`](language-runtime-database/00-story.md).

Dependency rules that force the shape: 23 after stage 3 + 22 (the ring is
the arc's, the baseline is 22's); 24 after 31 (chat is dishonest without
lifecycle); h2c stays parked behind the chain; the held tail keeps its own
precedence notes for resumption.

1. ✅ **8+11 stage 3** — landed 2026-08-21 (`just db-actor` 8/0; arc
   complete, stories in done/). Was: transparent DB actor. A correctness fix, not an
   optimization: worker VMs are zero-initialized, so a DB statement off
   the primary traps `WO_T_DB` — a multi-shard program touching the
   database is broken today. Plan of record:
   [`2026-08-20-shard-fiber-arc.md`](../superpowers/plans/2026-08-20-shard-fiber-arc.md)
   (stages 1+2 landed 2026-08-20, branch `concurrency-arc`).
2. 🔄 **22** — IN PROGRESS (spec + plan approved 2026-08-21) — the
   measurement backbone: restart-persistence proof + baseline
   benchmark (durable + RAM-only), single- AND multi-shard in one
   campaign, plus the stage-2 mutex-inbox number (rings only if the mutex
   costs). It has never run — no `bench/baseline.json`, no `just db-bench`;
   the arc's stages 1+2 delta is recorded retroactively.
3. **31** — actor lifecycle
   ([story](language-runtime-database/refine/31-actor-lifecycle.md),
   written 2026-08-21): request/response (`send` is one-way and callers
   `sleep` to await), bounded mailboxes (the FIFO only grows), actor
   death/supervision, timers beyond `time.sleep`.
4. **24** — chat, the arc's acceptance; honest only after 31 (19 landed
   2026-08-20 — Bytes carries the frames).
5. **23** — io_uring group-commit; the WAL's WRITE+FSYNC chains ride the
   arc's per-shard ring (T4); after 22's baseline — the payoff, measured.
6. **32** — WAL checkpoint
   ([story](language-runtime-database/refine/32-wal-checkpoint.md),
   written 2026-08-21): the WAL is append-only forever — snapshot +
   truncate reclaims disk and bounds replay; after 23 (composes with
   group-commit), policy set by 22's aged-store numbers.

**30** — observability, CI, fuzz: named 2026-08-20, still row-only (no
story file); slots in when scheduled — nothing in the chain depends on it.

⏸ **Held** (2026-08-21, developer decision): 18, 20, 21, 25, 26, 27, 28,
29 — stories in
[`hold/`](language-runtime-database/hold/) (25's story file
removed; its [plan doc](../superpowers/plans/2026-08-01-http-service-layer.md)
remains). Half-done branches (ipc-attach, keypair-auth) keep their
manifests.

✅ **17** — landed 2026-08-20 (unparked and executed): `kind = "library"`,
check mode, and the `internal/` dep boundary (WO-E108). Driver-only.
✅ **19** — landed 2026-08-20: Float + Bytes, `.wob` v5.

### Language track — sequenced, on the critical path

| #   | Item                                                                                                                                                                           | Plan                                                                                               |
| --- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | -------------------------------------------------------------------------------------------------- |
| 5   | Haxe-parity language surface — **`?T` forced handling first**, then switch expressions, records, enum payloads, try/catch, statics, `using`, modules, `is`, `pub(read)`, `#if` | [plan 8](../plan/compiler/2026-08-01-haxe-parity-language.md)                                         |
| 6   | Program mode + systems stdlib — `fn main`, exit codes, `fs`/`proc`/`net`/`time`/`json`                                                                                         | [plan 9](../superpowers/plans/2026-08-01-program-mode-stdlib.md)                                      |
| 7   | log-watcher proof — the sample compiles and detects a silent death live                                                                                                        | [plan 10](../superpowers/plans/2026-08-01-log-watcher-sample.md)                                      |
| 8   | Shard-actor runtime                                                                                                                                                            | [arc plan](../superpowers/plans/2026-08-20-shard-fiber-arc.md) (plan 4 ✖ discarded 2026-08-21 — epoll-based) |
| 9   | Database engine binding                                                                                                                                                        | [plan 5](../superpowers/plans/2026-08-01-db-engine-binding.md)                                        |
| 9b  | `@table` + relations + language-integrated query — comprehension queries, `ref`/`backlink` navigation, GroupBy aggregates; acceptance: new `docs/examples/employee` sample     | [spec](../superpowers/specs/2026-08-15-table-relations-query-design.md) · [plan](../plan/compiler/2026-08-15-employee-relations-query.md) |
| 20  | Cross-program tables — attach to a running program's database (IPC string in wo.toml, manifest-granted rights, owner stays the single writer)                                  | **no spec yet** — four open forks recorded in the iteration; brainstorm before planning            |
| 21  | Keypair attach auth — mutual challenge–response, grants name public keys, uid superseded                                                                                       | **no spec yet** — four forks recorded; plan folds into 20's                                        |
| 22  | Durability + throughput + scale — restart-persistence, read/write benchmark, ~1M rows; the gate every later optimization re-runs                                              | **no spec yet** — four forks recorded; the measurement backbone                                    |
| 23  | io_uring group-commit write path — batched durability overlapped on shard threads, fsync fallback                                                                             | **no spec yet** — brainstorm after iterations 8 + 22                                               |
| 27  | Query grammar from real embedded-DB corpora — whole-query count + correlated exists, driven by the skillhost SQL catalogue; add only what a corpus uses | **no spec yet** — three forks; may collapse to "confirm len(query) + add exists" |
| 14  | skillhost host workload — port skillhost (MCP host + confined script runner) to writeonce; drives the missing host capabilities into the open (bounded subprocess, stdin/stdout transport, fs metadata, FFI-vs-out-of-process) | **no spec yet** — gaps recorded in the iteration; each gap brainstormed on demand, bounded-subprocess first |
| 17  | library projects + dependency privacy — `wo.toml` kind = "library" (checkable without entry, dual lib+bin) + Go-style `internal/` at the [deps] boundary; framework reorg demonstrates both | ✅ **landed 2026-08-20** — [spec](../superpowers/specs/2026-08-20-library-kind-internal-design.md) · [plan](../superpowers/plans/2026-08-20-library-kind-internal.md) |
| 10  | HTTP service layer                                                                                                                                                             | [plan 6](../superpowers/plans/2026-08-01-http-service-layer.md)                                       |
| 11  | Fibers                                                                                                                                                                         | vision §3, [blue-green exploration](../plan/exploration/blue-green-vm/00-vision.md)                   |
| 12  | Blue-green deploy                                                                                                                                                              | [spec](../superpowers/specs/2026-08-03-blue-green-vm-design.md) — plan authored after iterations 9 + 25 |

### Language track — parked until after iteration 26

Recorded 2026-08-08 by scope directive; nothing here lands before the
log-watcher proof.

- `WO-W201` `@gc`-suggestion refinement beyond the self-reference heuristic
- `WO-E225` broadened to `ref`/`multi`/`map` element types and fn signatures
- ADT container roster adoption (Stack, Queue, Set, Tree, Graph, …) — see the
  roster in [`compiler/nullable-types-implementation.md`](../plan/compiler/nullable-types-implementation.md)
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
in [`discarded.md`](../plan/discarded.md).

---

## Discarded

Settled rejections with their reasons live in [`discarded.md`](../plan/discarded.md) —
inheritance, `abstract` newtypes, `Money`/`SKU`/`Float`, `Dynamic`/`cast`/
`macro`/`extern`, AOT-to-C, Menhir, shared mutable engine state, external
deployer daemon, destructive migrations in v1, and more. Argue against the
recorded reason rather than re-opening an entry as new.

## Learnings

What attempts taught, shipped or not, in [`learnings.md`](../plan/learnings.md) —
plumbed-is-not-enforced, vacuously-passing goldens, exit-0-with-wrong-output,
the malloc-path ASan trick, deferred checks that never reach the runtime,
validate-once-at-the-boundary, and reference-implement-in-C-first.
