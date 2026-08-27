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

## Task 5 — the `resident: keys` read path (split into 5a–5d)

> **SPLIT 2026-08-27, after 5a shipped.** This task was written as if the
> storage side were plumbing on existing functions. It is not, and that was
> measured rather than guessed: `wo_row_ptr` returns a `db_row *` into a slab
> and has **11 call sites**; `table.c` has 37 slab references; the query path
> walks slabs directly (`db.c:105-181`); `enc_val` serialises *from* the slab,
> so an insert must materialise, append, commit and only then drop the payload;
> and **no operation exists that drops a row's payload while keeping its index
> entries** — `wo_row_remove` removes from the indexes too.
>
> Note this is the *opposite* half from the earlier retraction. The record
> FORMAT genuinely needed nothing (retracted, correctly). The record STORAGE
> genuinely is deep, and leaving the step list reading as light plumbing was
> the residual error.
>
> | Sub-task | Scope | State |
> | --- | --- | --- |
> | **5a** | `wo_wal_next_offset` — exact record offsets, unit-proven | ✅ landed `ac7d8af` |
> | **5b** | `wo_wal_read_row_at` — materialise a row from an offset into VM values. **Zero storage change**, so it is additive and independently testable | this section |
> | **5c** | the shared borrow/release accessor, then id→offset storage. **Design settled 2026-08-27** — see its section | written up |
> | **5d** | rewiring the readers: remaining call sites, slab scans, FK restrict, `@unique` across the boundary | scope recorded, write-up waits on 5c |
>
> 5b and 5c are described below. **One correction:** an earlier version of
> this note said the secondary indexes point at slab slots. They store row
> **ids** (`table.h:88`) and are already indirect through the id hash, so they
> need no change — which is why 5c is one shared accessor rather than 11
> rewrites.


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

### 5b — read a row from an offset

**Files:** modify `database/src/wal.c` (beside `scan_record` at :272 and
`dec_val` at :167), `database/src/wal.h`; extend `runtime/test/test_wal.c`.

**Interfaces:**
- Consumes: 5a's `wo_wal_next_offset`, plus the already-public
  `wo_val_decode_vm` and `wo_db_val_free` (`table.h:183-187`).
- Produces: `wo_wal_read_row_at`, which 5c's map consumes as its read path.

- [ ] Add `wo_wal_read_row_at` in `wal.c`, mirroring `wo_row_read`'s contract
  (`table.c:721`) but resolving from a file offset instead of the id hash:
  `scan_record` the record, parse the `kind | class_id | id` header, `dec_val`
  each field into engine-owned slots, convert each to a fresh VM value with
  `wo_val_decode_vm`, then free the engine slots. The out-gate rule is
  unchanged — always a copy, never a pointer into anything.
- [ ] Return distinguishable outcomes: 0 ok, -1 no intact record at that
  offset or a record whose kind carries no payload (a REMOVE tombstone), -2
  OOM with `*msg` set. Silently treating a tombstone as a row would be the
  worst failure available here.
- [ ] Free every engine slot on **every** path including the partial-decode
  error path, matching what `dec_val`'s own callers already do at `wal.c:204`.
  ASan is the check, not inspection.
- [ ] Do not change any storage behaviour. Nothing calls this yet; it is
  additive, which is the whole point of separating it from 5c.
- [ ] Unit-test it against 5a's offsets: insert rows of mixed kinds
  (scalar, Text, and a nil Text), record each offset, then read every row
  back **by offset** and deep-compare field by field to what was inserted.
  Include a read at a deliberately wrong offset and at a tombstone, asserting
  the documented refusals rather than a crash.
- [ ] Verify: `make -C runtime test` and `test-iso`, both ASan+UBSan clean;
  `just oop-e2e`, `just residency`, `just employee`, `just db-actor`
  unchanged, since nothing calls the new function yet.
- [ ] Commit. Draft: `feat(db): wo_wal_read_row_at — materialise a row from a log offset`.


### 5c — the shared row accessor, then the offset map

**Design settled 2026-08-27 by reading the structures rather than guessing.
Both open questions have answers, and both make this smaller than feared:**

- **The id hash needs no new storage.** `db_table.hvals` is already `uint64_t`
  holding *global slot + 1*, with 0 meaning empty (`table.h:117-119`). An
  offset fits the same field, `offset + 1` reusing the same 0-is-empty trick.
  The interpretation is per-table and decided by the class flag, because a
  table is wholly `all` or wholly `keys` — never mixed. **No parallel map.**
- **Secondary indexes need no change at all.** `db_ibucket.ids` stores row
  **ids**, not slot indices (`table.h:88`, and `table.c:452` resolves them via
  `wo_row_ptr`). Every index is therefore already indirect through the id hash.
  An earlier note in this plan — and an analogy given to the developer — said
  these pointed at slots. That was wrong.
- **The unique shadow is the real coupling.** `idx_add_row` fetches the *other*
  row and compares columns (`table.c:452-453`), as does
  `row_apply_field_slot`'s update path. Those are the sites that need a row
  they cannot get from a slab.

**So the shape is one shared accessor, not 11 rewrites.** Every site that today
does `wo_row_ptr` then reads `r->slots[...]` becomes a borrow/release pair that
serves both modes: for `resident: all` it hands back the slab pointer and
releasing is a no-op; for `resident: keys` it materialises the record into
caller-provided scratch via 5b's `wo_wal_read_row_at` and releasing frees the
engine-owned values. One code path, two backings.

**Files:** modify `database/src/table.h` / `table.c` (the accessor, then the
`hvals` interpretation), `database/src/db.c` (the insert path's drop-payload
step); extend `runtime/test/test_table.c`.

**Interfaces:**
- Consumes: Task 3's class flags, 5a's `wo_wal_next_offset`, 5b's
  `wo_wal_read_row_at`.
- Produces: `wo_row_borrow` / `wo_row_release`, which 5d rewires every
  `wo_row_ptr` call site onto.

- [ ] Add `wo_row_borrow(db, cid, id, scratch, msg)` returning a `db_row *`,
  and `wo_row_release(db, cid, row, scratch)`. For a fully-resident table the
  borrow is exactly today's `wo_row_ptr` and the release does nothing, so the
  hot path gains at most a branch. Prove that first, alone, with **no
  keys-table anywhere** — this step must be a pure refactor.
- [ ] Size the scratch honestly: a borrow needs `row_size` bytes plus the
  engine-owned values its slots point at. Decide whether the caller supplies a
  stack buffer sized from `row_size` or the accessor allocates; the unique
  check runs inside a loop over a bucket, so an allocation per candidate would
  turn an O(1) probe into an allocation storm.
- [ ] Verify: `make -C runtime test` and `test-iso` unchanged, `just oop-e2e`,
  `just employee`, `just db-actor`, `just residency` unchanged, and `just
  db-bench --quick` shows the resident read path inside its baseline tolerance.
  A pure refactor that moves a number is not a pure refactor.
- [ ] Commit that refactor on its own, before any offset storage exists.
- [ ] Then: teach `hvals` the second interpretation, gated on the class flag —
  `slot + 1` for `all`, `offset + 1` for `keys`. Keep the accessors for
  reading it in one place so the two meanings cannot be confused at a call
  site.
- [ ] Then: the insert path for a keys-table — apply to RAM (required, since
  `enc_val` serialises *from* the slab), capture the offset, append, commit,
  and only then drop the payload while leaving the id hash and every index
  entry standing. This is the operation that does not exist today;
  `wo_row_remove` also unhooks the indexes, so it cannot be reused.
- [ ] Verify: a keys-table insert leaves the id hash and indexes populated, the
  slab slot recycled, and `wo_row_borrow` able to return the row from its
  offset. ASan clean — the drop path frees engine values that the record now
  owns instead.
- [ ] Commit. Draft: `feat(db): id->offset storage for resident:keys tables`.

### 5d — rewire the readers

Deliberately not written up until 5c's accessor exists, because its shape
decides how much of this is mechanical. Known scope: the remaining
`wo_row_ptr` call sites, the slab scans at `db.c:105-181` (a keys-table scan
walks the log sequentially instead), FK restrict's referrer scan, and the
`@unique` shadow across the boundary — the correctness core, since a
constraint that silently checks only resident rows must never ship.

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
