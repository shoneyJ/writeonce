# databasev2 2 — table residency implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: use
> `superpowers:subagent-driven-development` (recommended) or
> `superpowers:executing-plans` to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.
>
> **This plan contains no code, deliberately.** That is a repo rule, not an
> omission: `docs/plan/discarded.md` records "Raw code in plan documents —
> plans carry concept, reason, and required behavior in words; the executor
> writes the code", and all six preceding plans in this directory have zero
> code fences. Every step below names the exact file and line region to change
> and the behaviour required; the executor writes the code and the tests.

**Goal:** let a table declare what it keeps in memory, so a 120 GB table works
on a 32 GB host, without changing anything for tables that fit.

**Architecture:** one storage engine, log-structured. The WAL already holds
every row; this plan stops discarding the payload — an in-RAM id→offset map
plus `pread` becomes the read path for tables that ask for it. Two optional
`@table` arguments, both defaulting to today's behaviour. No second engine, no
user-space row cache (the kernel page cache is the hot copy).

**Tech Stack:** OCaml (compiler front end, stdlib only), C11 (runtime and
engine, libc only), the existing `.wob` image format, `tests/corpus` +
`compiler/test/golden` for fixtures, `just` recipes as gates.

**Spec:** [`../specs/2026-08-26-table-residency-design.md`](../specs/2026-08-26-table-residency-design.md)

## Global constraints

- **Defaults preserve behaviour exactly.** `durable: true`, `resident: all`.
  All 28 existing `@table` declarations compile and run unchanged; no golden is
  reblessed. Any task that breaks this is wrong.
- **The resident read baseline must not regress.** 1.3M ops/s, p50 1µs
  (iteration 22). A measurable regression on `resident: all` tables is grounds
  to reject the implementation, not to tune it.
- **Durability semantics are untouched.** Ack after fsync, replay whole-or-
  nothing, torn tail dropped by CRC. `resident:` never changes whether an ack
  means durable.
- **Every new diagnostic is catalogued in the same commit** that adds it, in
  `docs/plan/oop-vm/01-error-catalog.md`. That catalog went stale once by doing
  this later; ten codes had to be back-filled on 2026-08-26.
- **C11 + libc only** in `runtime/` and `database/`; OCaml stdlib only in
  `compiler/`. No new dependency.
- **`WOB_VERSION` moves once**, in Task 3, with the format contract
  (`docs/plan/oop-vm/00-wob-format.md`) in the same commit.
- Gates that must be green at the end of every task: `just woc-test`,
  `just wovm-test`, `just oop-e2e`, `just employee`, `just db-actor`.
  Tasks 5 onward add `just db-bench`.

---

## Task 1 — grammar: the two arguments exist and default correctly

**Files:** modify `compiler/src/ast.ml` (`table_cfg` at :521-524),
`compiler/src/parser.ml` (`parse_table_cfg` at :302-345, `table_code` at :128),
`compiler/src/dump.ml` (the `@table` dump line);
create goldens under `compiler/test/golden/ast/`.

**Interfaces:**
- Produces: `Ast.table_cfg` gains two fields — a durability flag defaulting to
  true, and a residency selector defaulting to "all". Tasks 2, 3 and 4 read
  both off `class_info.table` (`compiler/src/types.ml:49`).

- [ ] Extend `table_cfg` at `ast.ml:521-524` with the two fields beside
  `table_name` and `indexes`. Both carry their default in the record's initial
  value so an unannotated `@table` is byte-identical to today.
- [ ] Add a `durable` arm to `parse_table_cfg`'s key match (`parser.ml:313`,
  beside `name` and `index`). Accept only the `true`/`false` keyword tokens —
  they already exist as `KwTrue`/`KwFalse`. Reject a second occurrence with
  `table_code`, matching the shape of the existing "given twice" failure at
  `:315`.
- [ ] Add a `resident` arm accepting the bare identifiers `all` and `keys`,
  read the same way the `index:` arm reads column identifiers (`:322-327`).
  Reject a second occurrence, and reject any other identifier.
- [ ] Update the unknown-argument message at `:334` to list all four supported
  keys. Add an explicit, friendlier refusal for the retired brainstorm words —
  `ram`, `cold`, `tiered`, `paged`, `mmap`, `buffer`, and `index` as a
  `resident` value — naming the two real arguments, so vocabulary explored and
  rejected during design does not become folklore in user code.
- [ ] Extend the `@table` line in `dump.ml` to print both properties, so
  `--dump-ast` shows them and the goldens pin them.
- [ ] Add three AST goldens: a bare `@table`, one with `durable: false`, one
  with `resident: keys`. Bless once, by hand.
- [ ] Catalogue the new refusals in `docs/plan/oop-vm/01-error-catalog.md`
  under WO-E102's row — same code, new causes — and update the
  `@table` annotation row in `docs/guides/language-surface.md` §2 to match
  what the parser now accepts.
- [ ] Verify: `just woc-test` — the three new goldens pass and **every
  pre-existing golden is unchanged**. If any existing golden moved, the
  defaults are wrong; fix the defaults, do not rebless.
- [ ] Commit. Draft: `feat(woc): @table durable/resident arguments, defaults preserve behaviour`.

---

## Task 2 — the cross-table check: a durable row may not reference a volatile one

**Files:** modify `compiler/src/types.ml` (the class/field check pass that
already resolves `Ref` field types against the symbol table);
create fixtures under `tests/corpus/compile-fail/`.

**Interfaces:**
- Consumes: Task 1's two `table_cfg` fields, read via `class_info.table`
  (`types.ml:49`).
- Produces: one new `WO-E2xx` code. Pick the next free number by sweeping
  `<stage>_prefix ^ "NN"` across `compiler/src/*.ml` — note a grep for the
  literal `WO-E2NN` finds only comments, which is how ten codes went missing
  before.

- [ ] In the field-checking pass, for every field whose type is a `Ref` to a
  declared table class: if the *declaring* table is durable and the *referenced*
  table is not, report the new code at the field's own position. The message
  must name both classes and say why — a persistent row cannot hold a foreign
  key into a table that evaporates on restart, and FK-restrict cannot save it.
- [ ] Apply the same check to a `backlink` field, whose inverse is a `ref` and
  therefore has the same hazard in the other direction.
- [ ] Leave the reverse direction legal: a volatile table may reference a
  durable one. The dangling case only exists when the survivor points at the
  casualty.
- [ ] Catalogue the code with its message and a worked example.
- [ ] Add two `compile-fail` fixtures: a durable table with a `ref` into a
  volatile one, and the `backlink` twin. Each pins the exact code.
- [ ] Add one `run` fixture proving the legal direction still compiles and
  runs, so the check cannot be over-broad without a gate noticing.
- [ ] Verify: `just woc-test`, `just oop-e2e` — the new fixtures pass, nothing
  else moved.
- [ ] Commit. Draft: `feat(woc): refuse a durable ref into a volatile table`.

---

## Task 3 — format: the class descriptor carries both properties

**Files:** modify `runtime/src/wob.h` (`WOB_VERSION` at :16, `wo_classdesc` at
:547-563), `runtime/src/loader.c` (descriptor parse and validation),
`compiler/src/emit.ml` (descriptor emission), `compiler/src/disasm.ml`,
`docs/plan/oop-vm/00-wob-format.md`.

**Interfaces:**
- Consumes: Task 1's `table_cfg` fields.
- Produces: two descriptor fields the engine reads in Tasks 4 and 6. Name them
  in `wob.h` and use those exact names downstream.

- [ ] Add both properties to `wo_classdesc` at `wob.h:547-563`, in the house
  comment style, positioned so the struct's existing field order and any
  alignment assumption in `wo_obj_size` (:567) are preserved.
- [ ] Bump `WOB_VERSION` at `wob.h:16` to 7, extending the existing comment to
  say what v7 adds — the file's comment history is the format's changelog.
- [ ] Emit both properties from `emit.ml` where the class descriptor is
  written, defaulting exactly as Task 1 does.
- [ ] Extend the loader's descriptor validation to bounds-check both fields and
  **refuse the meaningless combination** — not durable and not fully resident —
  at load time. The loader's contract is that whatever it accepts, the
  interpreter may trust; this combination must never reach the engine.
- [ ] Confirm an image built before this change is refused on version rather
  than misread. This is what the version bump is for.
- [ ] Print both properties in `--dump-bc` (`disasm.ml`) so the corpus can pin
  them from the compiler side.
- [ ] Update `00-wob-format.md`'s descriptor section and its version history in
  this commit.
- [ ] Verify: `just woc-build`, `just wovm-build`, `just wovm-test`,
  `just oop-e2e` all green; a stale `.wob` is rejected clearly.
- [ ] Commit. Draft: `feat(wob): v7 — class descriptor carries durability and residency`.

---

## Task 4 — engine: a volatile table skips the WAL

**Files:** modify `database/src/db.c` (the three WAL-append sites at :26-32,
:48-51, :71-74), `database/src/wal.c` (replay), `database/src/table.h` /
`table.c` if the per-table property needs to reach the choke point;
create fixtures under `tests/corpus/run/`.

**Interfaces:**
- Consumes: Task 3's descriptor fields.
- Produces: the observable behaviour Task 7's gate asserts — no WAL growth for
  a volatile table, and an empty table after restart.

- [ ] Reach the per-table durability property at the three append sites in
  `db.c`. Each already guards on `vm->rt.wal` being non-null; the condition
  becomes "a WAL is open **and** this table is durable". Do not add a second
  choke point — `database/src/CODE-LOGIC.md` names these as the only places
  storage may be staged, and that invariant is worth more than the convenience.
- [ ] Confirm the ack path is unchanged for durable tables: RAM applied, record
  staged, one commit before the ack, and a failed commit still removes the row
  so RAM never claims what disk never acknowledged (the comment at `db.c:29-31`
  states this contract — preserve it exactly).
- [ ] Make replay skip records belonging to a class whose descriptor now says
  volatile. A WAL written when a table was durable and replayed after the
  source changed is a real case; silently resurrecting rows into a table
  declared not to have any is worse than refusing.
- [ ] Decide and implement the mismatch policy for that case: refuse at startup
  naming the class, rather than skipping silently. A silent skip is a data-loss
  surprise; the spec's Migration section commits to refusing on mismatch.
- [ ] Add a `run` fixture: two tables, one durable and one volatile, inserts
  into both, and a second process run proving the durable rows replayed and the
  volatile table is empty.
- [ ] Verify: `just oop-e2e`, `just employee`, `just db-actor` green; measure
  that a volatile table's inserts produce no WAL growth (compare file size,
  do not assert it).
- [ ] Commit. Draft: `feat(db): durable:false skips the WAL append and replay`.

---

## Task 5 — the `resident: keys` read path

> **Retraction, 2026-08-26.** This plan originally had a Task 5 that rewrote
> `db_val_encode`/`db_val_decode` into a "self-contained, offset-based" record
> format, described as the iteration's one real rewrite. **That task was
> fictional and has been deleted.** `table.c`'s `db_val_encode` builds the
> *in-memory slot*; the *file* record is a separate encoding in `wal.c`, and it
> has been flat since iteration 9: `enc_val` inlines every kind recursively with
> no pointer anywhere, `dec_val` reads it back into fresh engine values, a record
> is `WO_WAL_INSERT | class_id | id | <value per field>` inside the
> `len|crc|payload|mark` frame, and `scan_record(fd, off, …)` already `pread`s
> and CRC-verifies a record at an arbitrary offset. Nothing about the row
> encoding needs to change.
>
> **The real difficulty is offset capture, and it lives in this task.**
> `wo_wal_append_insert` calls `stage()` into a buffer (opened at 1 MiB in
> `main.c:210`), so a record's final file offset is unknown at append time and
> known only when that buffer flushes. Threading an accurate offset back to the
> caller through a buffered writer — correct across a partial flush, a failed
> commit, and a torn tail — is where to expect the bugs.


**Files:** modify `database/src/table.c` / `table.h` (the per-table id map, the
slab path), `database/src/db.c` (insert/read/update/delete), `database/src/wal.c`
(boot map rebuild); create fixtures under `tests/corpus/run/`.

**Interfaces:**
- Consumes: Task 3's descriptor fields, and `wal.c`'s existing `enc_val`/`dec_val`/`scan_record` — see the note below.
- Produces: a table whose rows are not resident, serving reads by offset.

- [ ] **Offset capture first, before any map exists.** Make the staging path in
  `wal.c` able to report the file offset a record will occupy. Decide between
  computing it as the buffer's base file offset plus the record's position
  within the buffer, or deferring the report until flush; whichever is chosen,
  the offset must be wrong in *no* case — a wrong offset reads a neighbouring
  record and passes its CRC.
- [ ] Prove offset capture in isolation before it has a consumer: a unit test
  that appends records straddling a buffer boundary, flushes, then reads each
  back by its reported offset via `scan_record` and asserts the recovered id
  matches the one appended. Include a failed-commit case, where no offset must
  be published for a record that never reached disk.
- [ ] For a `resident: keys` table, replace the slab retention with an
  id→offset map. Keep every index resident: the id map, each secondary index,
  and each `@unique` shadow. That residency is what makes the arithmetic work
  (≈3.8 GB of index for a 120 GB table) and what makes the constraints
  correct.
- [ ] Insert: append the record as Task 4 leaves it, then record id→offset
  instead of retaining a slab row.
- [ ] Read by id: map lookup, `pread` at the offset, verify the CRC the frame
  already carries, decode via `dec_val`. Use `pread` and **not** `O_DIRECT` — the
  kernel page cache is deliberately the hot copy.
- [ ] Update: append a new record, repoint the offset. The superseded record
  becomes garbage; do not attempt reclamation here — that is databasev2 3.
- [ ] Delete: append a tombstone, drop the id from the map and from every
  index.
- [ ] Scan: walk the log sequentially rather than issuing one `pread` per row,
  because a sequential walk is the case this layout is best at and a per-row
  read would make scans pathological.
- [ ] Boot: rebuild the id→offset map by replaying the log. Correct, and
  O(entire history) — record that cost in the iteration and note that
  databasev2 3's snapshot must persist the map so boot stops rescanning.
- [ ] Confirm the constraints hold across the boundary, with a fixture each:
  `@unique` refuses a duplicate whose conflicting row is not resident, and
  FK-restrict refuses a delete whose only referrer is not resident. These two
  are the correctness core; a constraint that silently checks only resident
  rows must never ship.
- [ ] Confirm `ref` navigation still works, at the cost of a read per hop.
- [ ] Add a fixture: a table whose row count exceeds any plausible resident
  budget, read back by id and scanned in full, byte-identical.
- [ ] Verify: `just oop-e2e`, `just employee`, `just db-actor` green; ASan
  clean; **`just db-bench` shows the `resident: all` read baseline unmoved**.
- [ ] Commit. Draft: `feat(db): resident:keys — rows read from the log by offset`.

---

## Task 6 — the two runtime refusals

**Files:** modify `runtime/src/main.c` (the `WO_DATA` block at :199-215),
plus wherever per-table accounting lands from Task 6.

**Interfaces:**
- Consumes: Task 3's descriptor fields, Task 5's accounting.

- [ ] **Refuse `durable: true` with no `WO_DATA`.** Today `main.c:199` opens a
  WAL only when the variable is set, and `db.c` skips appends when it is not —
  so a program that declares durability and is given nowhere to put it silently
  loses everything. Refuse at startup, naming the first durable class found.
  This is the single most valuable line in the plan and is independent of
  residency.
- [ ] Provide the escape hatch the refusal implies: a program that genuinely
  wants an ephemeral run must be able to say so, either by declaring its tables
  volatile or by an explicit opt-out flag. Decide which and document it — a
  refusal with no stated way forward is a worse bug than the silent loss.
- [ ] Implement the byte budget: estimated resident footprint across all
  tables, breached loudly with a message naming the largest offending table and
  the exact annotation to add.
- [ ] Default the budget to a fraction of host-detected available memory, **not
  to "none"** — a budget nobody sets cannot produce the diagnostic that is this
  design's main deliverable, and the 120 GB developer would still meet the OOM
  killer. Use a conservative placeholder fraction and mark the value explicitly
  unset-pending in both the code comment and the iteration: the real number
  comes from databasev2 1's swap-onset measurement.
- [ ] Make the accounting's error bound explicit where it is documented. It
  estimates RSS; it is not RSS, and pretending otherwise would make the budget
  untrustworthy the first time someone checked it.
- [ ] Add CLI-smoke coverage for both refusals, including the exit code and the
  first line of stderr — the shape scripts depend on.
- [ ] Verify: `bash runtime/test/cli_smoke.sh`, `just wovm-test`,
  `just employee`, `just db-actor` green; every sample that sets `WO_DATA`
  still runs, and one that does not is now refused or explicitly opted out.
- [ ] Commit. Draft: `feat(rt): refuse silent volatility, and bound resident footprint`.

---

## Task 7 — measure, gate, document, close out

**Files:** modify `scripts/db-bench.py` and `docs/examples/db-bench/`,
`bench/baseline.json`, `docs/plan/perf-targets.md`,
`docs/plan/oop-vm/04-db-binding.md`, `database/src/CODE-LOGIC.md`,
`docs/stories/databasev2/02-table-storage-modes.md`,
`docs/stories/databasev2/00-story.md`, `docs/stories/00-status.md`,
`docs/guides/language-surface.md`.

- [ ] Add db-bench legs for the `resident: keys` read and write paths at a
  size that exceeds resident memory, and a volatile-write leg showing the
  per-table saving from Task 4.
- [ ] Add the resulting rows to `bench/baseline.json` with per-class tolerances
  following iteration 22's policy, and confirm the gate bites on a doctored
  metric — a gate that only prints is not a gate.
- [ ] Publish the read amplification in `perf-targets.md`: how much slower a
  non-resident read is than a resident one, as a number a developer can plan
  around, alongside the measured per-row index footprint.
- [ ] Run the crash battery on a `resident: keys` table: `kill -9` mid-append
  and mid-read, replay, and assert no acked write lost and no row visible
  twice.
- [ ] Update `04-db-binding.md` with the residency contract and the record
  layout, and `database/src/CODE-LOGIC.md` with the encode/decode rewrite and
  the boot-rebuild cost.
- [ ] Rewrite `docs/stories/databasev2/02-table-storage-modes.md` around what
  shipped and remove its supersede banner; correct the track index
  (`00-story.md`), whose §"The lever" still describes the three-mode `cold`
  design the brainstorm replaced.
- [ ] Set the iteration's frontmatter `status: done` and add the board standup
  entry answering the six questions, including which `.dev/reference` projects
  were used.
- [ ] Verify: the whole battery — `just woc-test`, `just wovm-test`,
  `just oop-e2e`, `just oop-accept`, `just employee`, `just db-actor`,
  `just db-bench`, `just web-app`, `just site`, `just linkcheck`.
- [ ] Commit. Draft: `feat(db): table residency — measured, gated, documented`.

---

## Self-review against the spec

Checked section by section; recorded here so a reviewer can see what was and
was not covered.

| Spec section | Task |
| --- | --- |
| Grammar (two arguments, defaults) | 1 |
| The four combinations, incl. the refused one | 1 (parse), 3 (loader) |
| Compiler refusals: bad value, given twice, retired words | 1 |
| Compiler refusal: durable ref into volatile | 2 |
| Format / descriptor / version bump | 3 |
| `durable: false` skips the WAL | 4 |
| Replay skips or refuses on mismatch | 4 |
| Self-contained offset-based records | **none — already exists in `wal.c` since iteration 9. Claim retracted 2026-08-26; see the spec section of the same name.** |
| Read / update / delete / scan / boot for non-resident, incl. offset capture | 5 |
| `@unique` and FK-restrict across the boundary | 5 |
| Startup refusal: durable with no `WO_DATA` | 6 |
| Byte budget, default fraction, breach diagnostic | 6 |
| Proof plan: baseline, amplification, crash battery | 7 |
| Docs: catalog, language surface, binding contract, CODE-LOGIC | 1, 3, 7 |

**Gaps found and closed during review:** the spec's escape hatch for an
intentionally ephemeral run was implied but never stated — added as an explicit
step in Task 6, because a refusal with no way forward is worse than the silent
loss it replaces. The spec's note that databasev2 3's snapshot should persist
the offset map is now a recorded step in Task 5 rather than prose only.

**Deliberately not in this plan:** checkpoint and compaction (databasev2 3),
eviction and a resident row cache (databasev2 5), io_uring on the read path
(databasev2 4's note), per-shard residency for volatile tables (recorded as a
candidate in the spec), and any conversion of an existing dataset between
settings — the spec commits to refusing on mismatch, not converting.
