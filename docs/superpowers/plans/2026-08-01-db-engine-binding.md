# DB Engine Binding Implementation Plan

> **Status: 🔄 in progress — Task 1 done 2026-08-15** (story iteration 9) — class-shaped tables, typed WAL + recovery, `insert`/`select` execution. Story iteration 9b (`@table` relations + language-integrated query) follows it and needs a spec brainstormed first. Board: [00-status.md](../../00-status.md)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
>
> **Style rule (user convention):** concept, reason, and required behavior in words only; the executor writes the code.

**Goal:** Sub-project 3 of the OOP spec — objects become rows: every class is a table, `insert`/`select` execute against a per-shard RAM engine with WAL durability and boot replay, and the `DB_STUB` trap disappears from compiled programs.

**Architecture:** Plan 5 of 7. Depends on plans 1–4 (VM, compiler, emit/corpus, shard runtime). The storage substrate is again already proven in C: `runtime/wo-rt.c` phases B/D/E shipped the mmap arena with per-shard slices, framed CRC WAL records with group-commit fsync, torn-tail detection, and parallel boot replay (`docs/plan/exploration/c-runtime/00-plan.md`, exits measured). This plan generalizes those patterns from the demo's fixed row shape to class-shaped rows driven by the `.wob` class table, and wires the language's DB statements through them. The C++ prototype `prototypes/wo-db/` is the semantic reference for the query subset. The Rust engine's index doctrine carries over verbatim: secondary indexes are maintained ONLY through the engine's row-insert/row-remove path — nothing touches table storage directly.

**Tech Stack:** C11 + libc (pwrite/fdatasync or io_uring per the shipped phase-D pattern, fallocate, mmap). Hand-rolled CRC32 (already exists in wo-rt.c to port).

## Global Constraints

- All plan-1/plan-4 constraints carry over (libc only, no commits — drafts to `.dev/commit.md`, ASan/TSan gates, docs under `docs/`).
- **Every class IS a table** — `@table(...)` configures storage (name, indexes), never toggles table-ness (CLAUDE.md doctrine).
- **Per-shard ACID at the c-runtime plan's definition:** atomicity via framed replay-whole-or-not-at-all WAL records; isolation by the one-thread-one-stream execution model; durability = ack only after the covering fsync; group commit per tick.
- **Id discipline:** ids interleave per shard (`t+1, t+1+N, …`); a row's owner shard is `(id-1) % N`; point ops on a foreign row hop once via the plan-4 mailbox — creates are always local.
- **Index doctrine:** secondary indexes update only inside the engine's insert/remove path; direct storage mutation is a defect by definition.
- **RAM is authoritative:** reads never touch a file descriptor (phase-B doctrine); disk exists for durability and boot.
- **The GC bulkhead (analysis 2026-08-15, spec'd in the 9b design section 6):** values cross between VM heap and row storage only by copy, and a GC-managed value in a stored field is a compile error — so the collector never traces engine memory and the engine never touches reference counts. When iteration 7b makes GC-ness inferred, inference must classify classes **before** table-field validation so this error keeps firing, with the message naming the inference reason.
- **Format changes go through the format doc:** DB operations extend the builtin table (ids appended to `docs/plan/oop-vm/00-wob-format.md`); no opcode-space or version change.

---

## File Structure

```
database/src/            the engine is its own top-level directory (user decision
                         2026-08-15), statically linked into wovm — one binary, unchanged
  table.c table.h        class-shaped row storage: slabs, slots, id alloc, indexes (Tasks 1, 4)
  wal.c wal.h            typed-row WAL records, group commit, replay (Task 2)
  db.c db.h              statement executors: insert/select/update-point (Tasks 3, 5)
compiler/src/            DbStub nodes become typed DB AST + lowering (Task 3)
tests/corpus/db/         DB fixtures incl. crash/replay (Task 6)
docs/plan/oop-vm/04-db-binding.md   row format, WAL record layout, query subset (Task 1)
```

The engine's headers are included by `runtime/src` (the VM calls the row API);
`runtime/Makefile` compiles `database/src/*.c` into every `wovm` target,
sanitizers included. `database/` gets its own CODE-LOGIC.md as code lands.

---

### Task 1: Class-shaped row storage

**Concept & reason:** generalize phase B. Per shard, per class: a slab of fixed-size row slots sized from the class's field count (16-byte row header — id, class, flags — plus the same 8-byte slots the VM object layout uses, so a row and an object share their field encoding; text and container fields store engine-owned copies, not VM pointers). An allocation bitmap per slab; slab growth by arena extension. Id allocation interleaved per shard for coordination-free global uniqueness (shipped phase-A behavior). Row create/read/remove go through one API that Task 4's indexes hook — the doctrine choke point. The binding doc pins the row format, the field-encoding rules (what happens to each of the six kinds when a value crosses from VM heap to row storage — scalars copy, texts copy, owned objects flatten by value, `@gc` references are a compile error in stored fields already, `ref` is an id, containers copy element-wise), and the query subset promised by Task 5.

- [x] Tests first: round-trips across every kind (Text/owned-nested/multi/map
      copies proven by mutating the originals), nil encodings incl.
      `WO_NIL_SCALAR`, id interleave at N=3, slab growth past three slabs
      with stable addresses, removal reuses the slot while never reusing the
      id, misuse (unknown class, double remove). `test_table` 827/0 under
      ASan+UBSan.
- [x] Implemented in `database/src/table.{c,h}` (the 2026-08-15 directory
      decision), linked into every wovm and test binary via the Makefile's
      `DBSRC`. Binding doc written (`docs/plan/oop-vm/04-db-binding.md`:
      row format, encoding table, id discipline, choke points). All prior
      gates stay green with the engine linked (oop-e2e 71/0, log-watcher
      7/0) — it is dead code until Task 3 wires the first builtin.
- [x] Committed locally (2026-08-15). N=1 today: iteration 8 has not landed,
      so everything is N-parametric and tested at N=3 through the API.

### Task 2: Typed WAL + boot replay

**Concept & reason:** port phase D/E to typed rows. Records frame `length | crc | payload | commit-mark` (replay-whole-or-not-at-all); payload = record kind (insert/remove/update), class id, row id, encoded fields. Per-shard WAL files, fallocate-preallocated, appended on commit AFTER the RAM apply, one fdatasync covering the tick's batch, ack after the sync completes — the shipped commit order, verbatim. Boot: per-shard parallel replay before listeners open; torn tails detected and dropped whole (phase E behavior). The offline `wal-check` verification mode ports too — it is the crash test's oracle.

- [ ] Failing tests: commit-then-kill crash battery (the phase-D test shape: concurrent writes, SIGKILL mid-stream, offline verification proves every acked write present and CRC-valid, zero acked-but-missing); torn-tail drop; parallel replay rebuilds identical RAM state (deep-compare against pre-crash snapshot dump).
- [ ] Implement; green.
- [ ] Record commit draft: `feat(runtime): typed-row WAL + replay — framed CRC records over class rows, group-commit ack-after-fsync, parallel boot replay with torn-tail drop, wal-check oracle; crash battery green.`

### Task 3: `insert` executes

**Concept & reason:** the compiler's DbStub node for `insert` becomes a typed AST: target class, field initializer list (defaults applied for omitted fields with defaults — the explicit now() form computes at execution), returning the new id. Typechecking validates fields against the class exactly like constructor literals. Lowering emits DB builtins (ids appended to the format doc's builtin table): the executor allocates the id, encodes fields from registers, applies to RAM through the Task-1 API, stages the WAL record; the VM sees the id as the result. Inserts targeting the local shard complete inline; there is no remote insert — creates are always local by the id discipline. The pricing corpus's `set_price` fixture flips from expecting the DB trap to expecting success — the milestone's most satisfying diff.

- [ ] Failing tests: compiler goldens (typed insert AST, emitted builtins); runtime fixtures (insert then read back through select-by-id once Task 5 lands — interim: through a test hook on the row API); default-value application; the flipped pricing fixture.
- [ ] Implement both halves; green.
- [ ] Record commit draft: `feat: insert executes — DbStub becomes typed insert AST with constructor-grade field checking, DB builtins apply RAM-then-WAL through the row API; pricing set_price fixture flips from trap to green.`

### Task 4: Secondary indexes

**Concept & reason:** `@table(index: [a, b])` becomes real. Per-shard hash indexes (open addressing; text keys by content) from indexed field value to row id, maintained exclusively inside the row API's insert/remove — the doctrine choke point built in Task 1 pays off here. Unique constraints (`@unique` fields) enforce at insert with a constraint trap (new trap code, format doc updated). Index rebuild on boot replay happens through the same path for free.

- [ ] Failing tests: indexed lookup hits; unique violation traps; replay rebuilds indexes (crash battery re-run asserting post-replay index lookups); index maintenance survives remove-then-reinsert.
- [ ] Implement; green.
- [ ] Record commit draft: `feat(runtime): secondary indexes — per-shard hash indexes maintained only inside the row API, @unique constraint trap, replay rebuild; doctrine enforced by construction.`

### Task 5: `select` subset + cross-shard point reads

**Concept & reason:** the milestone query subset, semantics per the C++ `wo-db` reference where they overlap: select-by-id; select with a WHERE conjunction over indexed fields (index-backed) or a full shard scan (explicitly allowed, explicitly slower); dotted-path field access in the projection; results materialize as VM objects (rows decode back through the field-encoding rules — the Task-1 doc's table read in reverse). Local rows resolve inline; a by-id read of a foreign row hops once via the plan-4 mailbox (point ops hop once; the requesting job pumps its inbox while waiting — never blocks). List queries stay shard-local in this plan; scatter-gather fan-out is future work, stated in the doc. `update` limited to point-by-id field sets (the method-transaction pattern the pricing demo uses); no joins, no aggregations beyond the existing builtins, no RETURNING chaining — all named as out-of-scope in the binding doc.

- [ ] Failing tests: by-id local and cross-shard (deterministic two-shard fixture); WHERE over an index vs scan parity (same results both paths); projection decoding across kinds; point update round-trip with WAL coverage.
- [ ] Implement; green.
- [ ] Record commit draft: `feat: select subset — by-id (cross-shard hop-once), indexed/scan WHERE conjunctions, projection decode to VM objects, point update; scatter-gather and joins explicitly deferred.`

### Task 6: DB corpus + acceptance

**Concept & reason:** the corpus grows a `db/` kind wired into the standard runner: insert/select round-trip programs with exact stdout; the crash/replay battery as a scripted scenario (run, kill, reboot, assert identical query results); a unique-violation trap fixture; the C++ `wo-db` smoke overlap — where `prototypes/wo-db`'s `.wo` smoke files exercise semantics this subset implements, run both and compare (manifest-scoped like the plan-3 parity harness). `just oop-accept` gains the DB corpus and the crash scenario. The kanban and CLAUDE.md sync: Stage-3-adjacent language ("insert/select execute in the C runtime") replaces the DB_STUB story.

- [ ] Add fixtures + scenario + manifest; all green under ASan; docs synced.
- [ ] Record commit draft: `test(corpus): db suite — round-trips, crash/replay scenario, unique-violation trap, wo-db overlap manifest; oop-accept gains the DB gate; docs sync.`

---

## Plan self-review notes

- **Spec coverage (sub-project 3):** objects↔tables, DB_STUB retired, per-shard ACID with the shipped WAL discipline, id/owner discipline, index doctrine — all tasked. Explicitly deferred and documented: scatter-gather lists, joins/aggregations, RETURNING chaining, cross-shard transactions, `LIVE` (plan 7), the Postgres mirror (a Rust-runtime feature; revisit after parity).
- **Order rationale:** storage before WAL before statements (each layer is the next one's substrate); indexes after insert exists but before select needs them; corpus last.
- **Consistency check:** field-encoding rules defined once (Task 1 doc) and cited by insert (encode) and select (decode); the row API choke point defined in Task 1 is the only mutation path Tasks 3–5 use.
