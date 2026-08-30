# `resident: keys` delta updates — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: use
> `superpowers:subagent-driven-development` or `superpowers:executing-plans`,
> task by task. Steps are checkboxes.
>
> **House convention: this plan carries concept, reason and actions in words —
> no implementation or test code blocks.** The engineer writes the code; the
> plan says what must be true and why, and names every file and symbol
> involved. Exact values that must match are quoted inline.

**Goal:** let a row on a `resident: keys` table be updated, so the loader's
refusal of that annotation can be lifted.

**Architecture:** an update appends a **delta** record — class, id, field index,
new value, and a back-pointer to the record it supersedes — instead of a full
row. Reading walks the chain backward, first-seen-wins per field, until a
full-row record terminates it. Compaction folds every chain flat, so read cost
is bounded by updates since the last checkpoint rather than by a row's lifetime.

**Tech stack:** C11, the embedded database under `database/src/` (`wal.c`,
`table.c`, `db.c`), the runtime loader (`runtime/src/loader.c`), the C test
suite (`runtime/test/test_wal.c`), and the `.wo` example under
`docs/examples/residency` gated by `scripts/residency-accept.sh`.

**Spec:**
[`2026-08-30-keys-resident-delta-updates-design.md`](../specs/2026-08-30-keys-resident-delta-updates-design.md)

## Global constraints

- **One fold, three callers.** Reads, replay and compaction must fold
  identically. Write it once; no caller may implement its own walk. The spec
  names this the design's principal risk: a fold that differs between reading
  and replaying is a database that changes its mind at boot.
- **Commit before re-point, always.** A crash after the barrier leaves a
  durable delta and a stale map, which replay reconciles. A crash before leaves
  the row untouched. Reversing the order breaks both.
- The id map keeps **exactly one slot per row**. A map that grew per update
  would defeat the mode's premise, which is that the map is the only resident
  part.
- Indexed columns **may** change in a delta. Index maintenance runs per delta.
- Never delete-then-insert as an update. Assigning to a row field writes
  through and maintains indexes.
- `resident: all` behaviour and its log records must not change at all.
- Every task ends green on `just wovm-test`; the final tasks also on
  `just residency` and `just linkcheck`.
- Commits: bullets only, no prose paragraphs, at most 25 lines, title prefixed
  `feat(db2-delta): ` or `fix(db2-delta): `, ending with exactly
  `Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>`.
  Stage explicit paths; never `git add -A`. Do not push, branch or merge.

## File structure

| File | Responsibility |
| --- | --- |
| `database/src/wal.h` | the record-kind enum gains a fourth value; the pending-repoint list declaration |
| `database/src/wal.c` | encode/append a delta, **the one fold function**, replay's delta arm, compaction's fold-and-flatten |
| `database/src/table.c` | the update entry points stop refusing and call the append; index maintenance across a delta |
| `database/src/db.c` | the two request-path update arms stage a delta and defer the re-point |
| `runtime/src/loader.c` | the refusal is lifted — the deliverable |
| `runtime/test/test_wal.c` | the C-level proofs, including the resident-table oracle |
| `docs/examples/residency/` + `scripts/residency-accept.sh` | the end-to-end proof that only becomes possible once the refusal lifts |

The fold lives in `wal.c` because that is where records are read; `table.c`
calls it through the existing `wo_row_borrow`.

---

### Task 1 — the record kind and its encoding

**Files:** modify `database/src/wal.h` (the enum at line 46,
`WO_WAL_INSERT = 1, WO_WAL_REMOVE = 2, WO_WAL_UPDATE = 3`) and
`database/src/wal.c`.

**Produces:** `WO_WAL_DELTA = 4`, and an internal append that stages one delta
record. A delta's payload is: kind byte, class id, row id, field index, the
back-pointer offset, then the field's value encoded by the existing `enc_val`.

- [ ] **Step 1 — extend the enum.** Add the fourth value. Do not renumber the
      existing three: they are on disk in every log that exists.
- [ ] **Step 2 — write the failing test.** In `runtime/test/test_wal.c`, stage a
      delta, commit, then read the raw record back with `scan_record` and assert
      every field round-trips: kind, class, id, field index, back-pointer, and
      the value. It must fail before the encoder exists.
- [ ] **Step 3 — run it and watch it fail.** `make -C runtime build/test_wal &&
      ./runtime/build/test_wal`. Expect a compile failure naming the missing
      symbol — that is a legitimate first failure for C.
- [ ] **Step 4 — implement the append.** Follow `wo_wal_append_insert`'s shape:
      build a `wbuf`, `wput_*` the header fields, `enc_val` the one value, and
      `stage()` it. Take the back-pointer as a parameter rather than reading it
      from the map — the caller knows it and this keeps the encoder ignorant of
      table state.
- [ ] **Step 5 — run the test.** Expect pass.
- [ ] **Step 6 — commit.** Prefix `feat(db2-delta)`.

---

### Task 2 — the fold, and reads through it

**Files:** modify `database/src/wal.c` (near `wo_wal_read_row_at`, line 794)
and `database/src/table.c` (`wo_row_borrow`).

**Consumes:** `WO_WAL_DELTA` and the record layout from Task 1.

**Produces:** the single fold function every later task calls. Given a starting
offset it walks back through delta records, remembering the first value seen for
each field index, until it meets an INSERT or UPDATE record; it materialises
that base row and overlays the remembered fields. Name it once and do not
duplicate it.

- [ ] **Step 1 — write the failing test.** Insert a keys-resident row, drop its
      payload, append two deltas to two *different* fields, re-point after each,
      then borrow the row and assert both changed fields carry their new values
      and the untouched fields carry their originals.
- [ ] **Step 2 — write a second failing test that pins the ordering rule.** Two
      deltas to the SAME field. The newer must win. This is the test that
      catches a fold walking the chain in the wrong direction, which is
      otherwise invisible — a wrong-direction fold returns plausible data.
- [ ] **Step 3 — run both, watch them fail.**
- [ ] **Step 4 — implement the fold.** Walk backward from the given offset.
      Track which field indexes have been resolved; skip a delta for a field
      already resolved, because the newer one was seen first. Stop at the first
      full-row record and decode it, then overlay. Guard against a cycle in the
      back-pointers by refusing to walk more records than the log can hold —
      corrupt back-pointers must fail loudly, not spin.
- [ ] **Step 5 — route `wo_row_borrow` through it.** Its keys arm currently
      calls `wo_wal_read_row_at` directly; it now folds instead. Everything the
      borrow already guarantees stays: the per-table scratch, the `scratch_busy`
      nested-borrow refusal, and the cid/id identity check.
- [ ] **Step 6 — run the whole WAL suite.** Every existing keys test must still
      pass — a row with no deltas folds to itself, which is the case they cover.
- [ ] **Step 7 — commit.** Prefix `feat(db2-delta)`.

---

### Task 3 — updates append, indexes follow

**Files:** modify `database/src/table.c` (`wo_row_update_field` and
`wo_row_update_field_slot` — both currently refuse keys tables with
"update on a `resident: keys` table is not implemented") and
`database/src/wal.c`.

**Consumes:** the append from Task 1, the fold from Task 2.

**Produces:** a working update on a keys-resident table, and the pending-repoint
list the request path needs in Task 4.

- [ ] **Step 1 — write the failing test.** Update one field of a keys-resident
      row through `wo_row_update_field`, then read it back and assert the new
      value. It fails today with the refusal message.
- [ ] **Step 2 — write the index test, which is the one that matters.** Give the
      table a secondary index, update an **indexed** column, then query through
      that index and assert the row is found by its NEW value and NOT by its
      old. The spec allows indexed columns to change specifically because a
      catalogue indexes `price`, so this is the realistic case, not an edge one.
- [ ] **Step 3 — run both, watch them fail.**
- [ ] **Step 4 — replace the refusals.** Both entry points converge on one
      internal path: borrow the row (which folds), read its current offset with
      `wo_row_offset1`, append the delta with that offset as the back-pointer,
      commit, then `wo_row_set_offset` to the new record.
- [ ] **Step 5 — maintain the indexes.** The borrowed row is the OLD row; call
      `idx_remove_row` with it, apply the field change to the materialised copy,
      then `idx_add_row`. Both functions already exist in `table.c` (lines 452
      and 487) and both take a `db_row *`.
- [ ] **Step 6 — release the borrow on every exit**, including the failure
      arms. A keys borrow holds the per-table scratch and blocks the next
      borrow until released; an early return that skips it deadlocks the table.
- [ ] **Step 7 — run the suite.** Expect pass.
- [ ] **Step 8 — commit.** Prefix `feat(db2-delta)`.

---

### Task 4 — the request path and group commit

**Files:** modify `database/src/db.c` (the two `WO_B_DB_UPDATE_FIELD` arms at
lines 88 and 289 — one inline, one on the request path) and
`database/src/wal.c` (the pending list beside `pend`/`pend_len`/`pend_cap` in
`wal.h`, and `wo_db_flush_drops`).

**Consumes:** the update path from Task 3.

- [ ] **Step 1 — read how inserts already do this before writing anything.**
      The insert path stages, lets the drain commit once at a barrier, and only
      then applies its map change through `wo_db_flush_drops`. An update must
      follow the same shape: the re-point is a map change that must not happen
      before the barrier.
- [ ] **Step 2 — write the failing test.** Two updates to the same row inside
      one drain, then a read. Both must be visible, and the row's chain must
      have both deltas. A re-point applied too early shows up as the second
      delta's back-pointer skipping the first.
- [ ] **Step 3 — run it, watch it fail.**
- [ ] **Step 4 — add the pending-repoint list.** Mirror the pending-drop list's
      declaration and lifetime exactly. Do not reuse the drop list: a drop and a
      re-point carry the same three fields but mean different things, and
      merging them would make a future reader guess.
- [ ] **Step 5 — wire both arms.** The inline arm commits and flushes
      immediately, as it does for inserts; the request arm records the pending
      re-point and lets the drain's barrier flush it.
- [ ] **Step 6 — run the suite, and `just db-actor`** — that gate drives actors
      on worker shards through the request path, which is the arm most easily
      got wrong.
- [ ] **Step 7 — commit.** Prefix `feat(db2-delta)`.

---

### Task 5 — replay and compaction fold the same way

**Files:** modify `database/src/wal.c` (`apply_record` at line 738,
`wo_wal_replay_ex` at line 861, and `wo_wal_compact`).

**Consumes:** the fold from Task 2.

- [ ] **Step 1 — write the failing replay test.** Build a row with a chain of
      three deltas, close the log, replay it into a fresh database, and assert
      the row reads exactly as it did before the restart. Assert the indexes too:
      query through a secondary index whose column one of the deltas changed.
- [ ] **Step 2 — write the failing compaction test.** Same chain, then compact,
      then assert the row still reads identically **and its chain is now length
      zero** — the record its offset points at must be a full row, not a delta.
      That second assertion is the one proving compaction is the bound the spec
      relies on; without it the test passes even if compaction copied the chain.
- [ ] **Step 3 — run both, watch them fail.**
- [ ] **Step 4 — replay's delta arm.** A delta re-points the row's offset, the
      same way a dropped payload does. Because indexed columns may change,
      replay must also fold the delta into the indexes built from the base
      record. Note `wo_wal_replay_ex` already lends the runtime a read-only WAL
      view for the replay's duration — the fold needs it and it is already there.
- [ ] **Step 5 — compaction folds flat.** It already walks live ids and copies
      each row's current record. It must now fold each row and write ONE
      full-row record, so every checkpoint resets every chain. Keep the existing
      byte-for-byte copy for rows whose chain is already empty: it is faster and
      it is what the current code does.
- [ ] **Step 6 — the crash window, which is the criterion nothing else covers.**
      Append a delta, commit it, and then do NOT re-point the map — that is
      exactly the state a crash between the barrier and the flush leaves behind.
      Replay the log into a fresh database and assert the update IS present.
      This is the test proving the commit-before-re-point ordering earns its
      place; without it the ordering is an untested assertion in a comment.
      `test_compact_crash_battery` in the same file shows the established shape
      for simulating a crash point without actually killing a process.
- [ ] **Step 7 — run the suite plus `just residency`.**
- [ ] **Step 8 — commit.** Prefix `feat(db2-delta)`.

---

### Task 6 — lift the refusal, and prove it end to end

**Files:** modify `runtime/src/loader.c` (the `WO_CLASSF_RESIDENT_KEYS` BAIL),
`docs/examples/residency/main.wo`, `docs/examples/residency/README.md`,
`scripts/residency-accept.sh`, `docs/stories/databasev2/02-table-storage-modes.md`,
`docs/stories/00-status.md`.

**Consumes:** everything above.

- [ ] **Step 1 — audit the request path before lifting anything.** The `db.c`
      request arms were never audited for keys-residency the way the inline
      path was, and the last audit of that kind found `delete` corrupting
      memory. Read every arm that touches a row on a keys table and confirm it
      goes through borrow/release or `wo_row_next_id`, never `wo_row_ptr`
      directly. Report what you checked, not just the conclusion.
- [ ] **Step 2 — lift the loader refusal.** Delete the BAIL. Keep the second
      one: `durable: false` with `resident: keys` stays refused, because a row
      that is never logged has nowhere to be read from.
- [ ] **Step 3 — make the example real.** In `docs/examples/residency/main.wo`,
      uncomment the keys-resident `Product` declaration and make it the live
      one. `place_order` already updates `stock`, which is now the feature under
      test rather than the thing that cannot work.
- [ ] **Step 4 — gate it.** `scripts/residency-accept.sh` currently asserts the
      annotation is REFUSED at load. That leg must invert: it now asserts the
      program runs and the order decrements stock across a restart. Delete the
      refusal assertion rather than leaving it inverted-but-present.
- [ ] **Step 5 — the oracle test.** In `test_wal.c`, run the same sequence of
      updates against a `resident: all` table and a `resident: keys` table and
      assert the rows are identical at every step. The resident table is the
      oracle; this is the strongest available check that the fold agrees with
      ordinary storage.
- [ ] **Step 6 — update the docs to say what is now true.** The story's
      Outstanding criterion for updates moves to Met, recording HOW it was
      verified. The example's README loses its "why it is refused" section and
      gains what the mode costs: a read is one pread plus deltas since the last
      checkpoint. The status board gets its standup entry.
- [ ] **Step 7 — run everything.** `just wovm-test`, `just residency`,
      `just db-actor`, `just linkcheck`. All green.
- [ ] **Step 8 — commit.** Prefix `feat(db2-delta)`.

---

## What this plan deliberately does not do

- **No chain cap and no read-triggered fold.** Settled in the spec: compaction
  is the bound, and a cap is a number nobody would tune.
- **No measurement.** Whether this mode beats the kernel's own paging is
  databasev2 2's task 7, and it decides whether any of this is worth keeping.
  This plan must not be read as answering it.
- **No change to `resident: all`.** If a diff touches that path, it is wrong.

## The risk to watch while executing

The fold is written once and called from three places. The tests are arranged so
each caller is proven separately — Task 2 for reads, Task 5 for replay and
compaction, Task 6's oracle for all of them against ordinary storage. If a task
finds itself wanting a second fold "just for this caller", that is the design
failing and it should stop and say so rather than write it.
