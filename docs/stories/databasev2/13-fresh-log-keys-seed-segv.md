---
track: databasev2
iteration: "13"
status: done
readiness: ready
---

# databasev2 13 — fresh-log keys-resident seed SEGV

> Part of [Story — databasev2: the database beyond RAM](00-story.md). Found
> 2026-09-10 by codd-zack while smoke-testing
> [databasev2 7](07-single-file-db.md); independent of that iteration — the
> ledger reproduces it against both `WO_DATA` forms.

## Symptom

`seed` of [`docs/examples/residency`](../../examples/residency/README.md) (a
`resident: keys` table) segfaults, rc 139, on a FRESH log — in both the
directory form and the file form of `WO_DATA`, so the crash is independent of
databasev2 7. AddressSanitizer reports a SEGV on the zero page inside
`wo_wal_fold_row_at` (`database/src/wal.c:1871`), called from
`keys_fold_into` (`database/src/table.c:787`), `wo_row_borrow`
(`database/src/table.c:849`), `wo_idx_probe` (`database/src/table.c:373`),
`wo_builtin_db` (`database/src/db.c:215`).

## Root cause (confirmed by the fix)

Two defects compose. First, `wo_wal_fold_row_at` writes through `*msg`
unguarded at `database/src/wal.c` lines ~1861, ~1871, ~1884 and ~1890, while
`wo_idx_probe` (`database/src/table.c:373`) calls its borrow with `msg =
NULL` — the zero-page write IS the SEGV. Second, the arm the fold enters is
"record header is malformed", i.e. the row's stored offset points at a
record that is not a row record at all: on a fresh log the databasev2 12
schema head record is staged LAZILY, inside the first real append
(`database/src/wal.c:479-490`), which runs AFTER `database/src/db.c` has
already captured `koff = wo_wal_next_offset(w)` (`database/src/db.c:78/99/293`)
— so the first keys-resident row's recorded offset points at the schema
record, not at itself. Confirmed against the code by codd-zack, with one
correction to the original hypothesis: `wo_wal_ensure_schema` was test-only —
production never called it, so nothing outside `stage()` staged the head
before this fix; the fix deliberately does not commit inside
`wo_wal_next_offset` (only stages the head), keeping "one barrier per drain".

Consistent with: restart then `order` works (replay rebuilds every offset
from the real log, so the schema-head race cannot recur there), and with the
open `.claude/agents/codd.md` "Next bugs" entry for `residency.keys.fit` rc
74 "replay rebuilds the row offsets". The two do **not** share this root
cause — `keys.fit` reproduces unchanged on top of this fix (different code
path: compaction/replay of keys-resident offsets, not fresh-log first-insert)
— and it stays open under codd.md's "Next bugs" as its own defect.

A third finding, not an engine defect: `scripts/residency-accept.sh:155` ran
the example's `seed` without checking its exit code, so the 2026-09-10 gate
reported **20 checks, 0 failures** while `seed` was segfaulting under it.
Fixed by codd-cyril, `e274f4a` (databasev2 7 task 4).

## Fix

Owner: codd-zack, prefix `fix(db2-keys)`. Landed on `dev`:

- `6310078 fix(db2-keys): stage the schema head before the first offset
  capture` — root cause: `wo_wal_next_offset` is no longer a pure inline; it
  stages the pending schema head first (`stage_schema_head`, the one helper
  `stage()` and `wo_wal_ensure_schema` also use) and only then returns
  `off + len`, so "the offset the next record lands at" is exact by
  construction for every caller. `database/src/db.c` is untouched — the head
  is still staged lazily (a log that never appends still stays schema-less).
  Test: `test_keys_resident_fresh_log_first_row` (`runtime/test/test_wal.c`).
- `1b6750d fix(db2-keys): wo_wal_fold_row_at tolerates msg == NULL` — guard:
  the fold no longer writes through a NULL `msg` (a throwaway local sink is
  substituted), independent hardening of the same call path. Test:
  `test_fold_row_at_tolerates_null_msg` (`runtime/test/test_wal.c`).

## Acceptance Criteria

Met:

- **Given** a fresh log (no prior records) and a `resident: keys` table's
  first insert, **when** that row is read back in the same run, **then** the
  row is returned correctly and the process does not crash — proven in both
  the directory form and the file form of `WO_DATA`. ✅ unit:
  `test_keys_resident_fresh_log_first_row` (`runtime/test/test_wal.c`),
  failing-first (ASan SEGV at `wal.c:1871`, `koff != 0` FAIL) then green.
  Gate: `runtime/build/wovm_asan` residency `seed` under both `WO_DATA` forms
  — dir rc 0 (`seeded: products=2 carts=1 SKU-1 stock=10`; restart `order`
  rc 0), file rc 0, same lines; no sanitizer output. A control build with
  only `wal.c`/`wal.h` reverted to pre-fix reproduces the identical SEGV
  trace, confirming the fix (not a coincidental pass).
- **Given** `wo_idx_probe`'s NULL-`msg` borrow, **when** `wo_wal_fold_row_at`
  runs, **then** it tolerates a NULL `msg` rather than writing through it.
  ✅ unit: `test_fold_row_at_tolerates_null_msg` (`runtime/test/test_wal.c`),
  failing-first (ASan SEGV at `wal.c:1886` from the test) then green.
- **Given** the residency gate (`scripts/residency-accept.sh`), **when**
  `seed` runs, **then** the gate checks its exit code and fails loudly on a
  non-zero rc, instead of reporting the run green while `seed` crashed. ✅
  `e274f4a` (databasev2 7 task 4): `residency-accept.sh:158-162` "example:
  seed exits 0" — failing-first on the stale binary (`FAIL example seed --
  rc=139`), green (`ok example: seed exits 0`) once `runtime/wovm` is
  rebuilt with this fix in the tree.

Counts: `test_wal` 6629 → 6660 pass, 0 fail (+31: the two new tests plus their
helper). `make -C runtime test`: 21 suites, **8462 pass / 0 fail** (was
8431/0). `make -C runtime wovm-asan` builds clean (-Werror). FINAL
`just residency`: **residency-accept: 32 checks, 0 failures** — up from
20 checks (the pre-13 gate did not check `seed`'s rc) and from the 1-failure
intermediate reading (`FAIL example seed -- rc=139`) once this fix landed in
the tree.

## Out Of Scope

- The `residency.keys.fit` compaction-integrity rc 74 bug
  (`.claude/agents/codd.md` "Next bugs") — **confirmed a separate defect**:
  it reproduces unchanged on top of this fix (`db-bench-quick`'s residency
  legs, still rc 74), a different code path (compaction/replay of
  keys-resident offsets, not fresh-log first-insert). Stays open under
  codd.md's "Next bugs", not claimed by this fix.
- Any change to databasev2 7's file-form path resolution — this defect
  reproduces identically in the pre-existing directory form, so it predates
  and is independent of that iteration.

## History

- 2026-09-10: found by codd-zack while smoke-testing databasev2 7 (ledger
  `.dev/zack/databasev2-7.md`); filed as this story; fix in flight.
- 2026-09-10: fixed and closed — `6310078` (root cause) + `1b6750d` (NULL
  guard); `test_wal` 6660/0, `make -C runtime test` 21 suites 8462/0,
  `just residency` 32/0.
