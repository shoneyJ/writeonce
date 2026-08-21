# Iteration 18 — framework v2 (transaction{} + cache/flags/jobs): implementation plan

> **Status: ⏸ hold (2026-08-21, developer decision)** — story iteration 18
> sits in `stories/language-runtime-database/hold/`; plan was ready to
> execute (2026-08-20) and stays intact for resumption. Board:
> [docs/00-status.md](../../stories/00-status.md).

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps
> use checkbox (`- [ ]`) syntax for tracking.
>
> **Style rule (user convention):** concept, reason, and required behavior
> in words plus verification commands only — no implementation or test
> code blocks; the executor writes the code.

**Goal:** `transaction { }` makes multi-statement writes one WAL commit
(trap = abort), and three framework pieces ride the single process:
a TTL cache, `@table` feature flags with a cached read, and a durable
`@table` job queue drained in-process — the web-app proves the headline
(order + job enqueue, ONE commit, SIGKILL-survivable).

**Architecture:** the engine already stages WAL batches
(`wal_append_*` → `wo_wal_commit`, per-statement today in
`database/src/db.c`'s three write cases); a transaction defers the commit
and keeps an undo log for RAM. The VM learns two internal builtins and a
transaction-flagged catch frame; the parser one keyword. Cache/flags/jobs
are pure `.wo` in the framework plus one serve-loop seam. No new opcodes,
no `.wob` version bump, GC untouched.

**Tech Stack:** C11 libc-only (engine + VM), OCaml stdlib-only (`woc`),
pure `.wo` (framework), bash gates.

**Spec:** [`../specs/2026-08-20-memory-db-features-design.md`](../specs/2026-08-20-memory-db-features-design.md)
(approved 2026-08-20, normative). Story:
[`18-memory-db-features.md`](../../stories/language-runtime-database/hold/18-memory-db-features.md).

## Global Constraints

- Branch `framework-v2` off master; commits local only, never push.
- Exit/trap doctrine unchanged: WO-E110 nested `transaction` (compile),
  WO_T_DB "nested transaction" (dynamic, through a call); commit failure
  is the existing WO_T_IO shape with RAM never ahead of disk.
- Gates that must stay green after every task: `just woc-test`,
  `just oop-e2e`, `just deps-accept`, `just web-app`, `just log-watcher`,
  `just employee` — plus the oop-accept ASan clause for anything the VM
  touches.
- Framework tables carry the `wf_` prefix; policy stays app-side.
- `time.now` is wall-clock MILLISECONDS everywhere in this iteration.

## Spec deviations, disclosed up front

1. **`return`/`break`/`continue` crossing a `transaction { }` boundary is
   rejected at compile time (new WO-E112).** The spec is silent on early
   exit; commit-on-return vs abort-on-return is exactly the ambiguity a
   v1 must not guess at. A later iteration may define it; today the block
   has one entrance and one exit (a TRAP still aborts — that path is
   defined).
2. **The cache exposes a time-injected seam** (`get_at`/`put_at` taking a
   now-milliseconds argument, with `get`/`put` sugar reading `time.now`)
   so the corpus fixture injects stamps instead of sleeping — the spec's
   "stamps injected, not waited" made concrete.
3. **Flags are gate-proven, not corpus-proven**: a `@table` needs
   `WO_DATA` and a persistent directory, which the corpus harness does
   not provide; `just web-app` carries the flags checks (the spec's gate
   section already put them there).
4. **The WAL gains an explicit staged-batch discard** (`wo_wal_abort`):
   today a failed commit "stays staged" by contract; abort needs to drop
   the batch deliberately. Same file, same batch machinery, new entry
   point.

---

## Task 1 — engine transactions: defer commit, undo log, abort

**Files:**
- Modify: `database/src/db.h` (txn state on `wo_db`, three new entry
  points), `database/src/db.c` (the three write cases ~lines 17–80),
  `database/src/wal.h` + `wal.c` (`wo_wal_abort`),
  `database/src/table.h` + `table.c` only if the replay-only fixed-id
  create/index pair needs a non-static wrapper.
- Test: `runtime/test/test_txn.c` (new, mirroring the existing
  `test_*.c` harness shape), wired into the runtime test recipe.

**Interfaces:**
- Produces: `wo_db_txn_begin(db)` (0 ok; nonzero = already open — the
  dynamic-nesting signal), `wo_db_txn_commit(db, wal)` (the ONE
  `wo_wal_commit`; on failure the undo walk runs and the error returns),
  `wo_db_txn_abort(db, wal)` (reverse undo walk + `wo_wal_abort`).
  Task 3's builtins call exactly these three.

- [ ] `wo_db` grows a transaction flag and an undo log (a growable array
  of entries: op kind, class id, row id, and for update/delete a copy of
  the row's bytes — `wo_row_ptr` + the class's `row_size` are the copy;
  the log exists only while a transaction is open, zero cost otherwise).
- [ ] The three write cases in `wo_builtin_db`: when the flag is set,
  record the undo entry BEFORE the RAM apply (update/delete pre-images;
  insert records just the new id AFTER apply), still `wal_append_*`, and
  SKIP the per-statement `wo_wal_commit`. Flag clear = byte-identical
  behavior to today (every existing gate is the proof).
- [ ] Statement-level failures inside a transaction change nothing: a
  unique violation traps before apply and stages nothing (already true —
  `wo_row_insert` refuses first); a failed append keeps the pre-existing
  error shape.
- [ ] Abort walks the undo log in REVERSE: inserted row → removed;
  updated row → bytes restored and index entries fixed the replay way
  (remove + re-add through the same engine-internal pair `wal.c` replay
  uses); deleted row → re-created with its FIXED id and re-indexed (the
  replay-only create), then `wo_wal_abort` discards the staged batch.
- [ ] Reads inside a transaction need no change: RAM stays applied, so
  scans/point-reads see the block's own writes for free.
- [ ] `test_txn.c`: begin→insert+insert→commit = both rows + ONE wal
  flush; begin→insert→abort = zero rows, next insert works; update and
  delete pre-images restored on abort (indexed column included);
  begin-while-open refused; abort with an empty log is a no-op.
- [ ] Run the runtime test suite + `just employee` (engine untouched when
  no txn opens). Commit.

## Task 2 — the language surface: keyword, block, WO-E110/E112

**Files:**
- Modify: `compiler/src/token.ml` + `lexer.ml` (KwTransaction),
  `compiler/src/ast.ml` (a Transaction statement holding a body),
  `compiler/src/parser.ml` (block statement + LEXICAL nesting =
  WO-E110), `compiler/src/types.ml` (walk the body; E-code constants),
  `compiler/src/owner.ml` (treat as a plain nested scope),
  `compiler/src/dump.ml` (labels).

**Interfaces:**
- Consumes: nothing new.
- Produces: the AST node Task 3 lowers; WO-E110 (parsing prefix, nested
  block), WO-E112 (parsing prefix, `return`/`break`/`continue` whose
  jump would cross the block boundary — a loop wholly INSIDE the block
  keeps its own break/continue).

- [ ] Keyword + statement parse; the body is an ordinary statement list.
  A `transaction` token while one is already open (parser-tracked depth)
  is WO-E110 at the inner keyword.
- [ ] WO-E112: while parsing the block, a `return` at any depth, or a
  `break`/`continue` not enclosed by a loop that itself started inside
  the block, names the rule ("a transaction has one exit; lift the
  return out or end the block first").
- [ ] Types/owner: the body checks exactly like a bare block — no new
  typing rule (the ownership pass sees a scope; values born inside drop
  inside, exactly as today).
- [ ] Compile-fail fixtures: `transaction-nested` (WO-E110),
  `transaction-early-return` (WO-E112). Verify both + `just woc-test`
  (dump labels) + `just oop-e2e`. Commit.

## Task 3 — VM lowering: internal builtins + the abort-on-unwind frame

**Files:**
- Modify: `runtime/src/wob.h` (two builtin ids in the internal range),
  `runtime/src/builtin.c` (dispatch to Task 1's three entry points),
  `runtime/src/vm.c` (transaction-flagged catch frame; the vm_trap walk;
  unwind/rt-destroy cleanup), `compiler/src/emit.ml` (lower the
  Transaction statement), `runtime/test/test_unwind.c` (frame cleanup on
  a trap that leaves the whole method).

**Interfaces:**
- Consumes: Task 1's `wo_db_txn_begin/commit/abort`, Task 2's AST node.
- Produces: the observable spec semantics — one fdatasync at the closing
  brace; trap unwinding OUT aborts and keeps unwinding; `try` INSIDE the
  block keeps it alive.

- [ ] Two builtin ids the emitter emits directly from the Transaction
  case (no name in any user-callable table — nothing to collide with):
  begin pushes a TRANSACTION-FLAGGED catch frame (the existing TRY frame
  machinery with one flag bit) and calls txn_begin (nonzero = the
  WO_T_DB "nested transaction" trap); end calls txn_commit FIRST and
  pops the frame only on success — a commit failure traps with the frame
  still in place, so the abort path below runs and RAM is rolled back
  (the "RAM never ahead of disk" rule, transaction-sized).
- [ ] vm_trap's handler search: a transaction-flagged frame is not a
  handler — abort the transaction, pop it, CONTINUE searching. An inner
  `try` frame sits ABOVE it and catches first (the spec's
  inner-try-keeps-it-alive rule falls out of frame order, no special
  case).
- [ ] Program exit / rt teardown with a transaction somehow open (a trap
  that reaches main uncaught) must abort, not leak the undo log.
- [ ] Emitter: begin, body statements, end — plus the WO-E112 guarantee
  from Task 2 meaning no jump ever leaves the region except a trap.
- [ ] Corpus: `run/transaction-commit` (two inserts, both rows readable
  after — needs the trap corpus's WO_DATA-less shape? No: @table without
  WO_DATA runs RAM-only with no WAL, which still exercises begin/commit
  frames; the DURABILITY half lives in Task 6's gate where WO_DATA
  exists), `run/transaction-abort` (second insert unique-traps, caught
  OUTSIDE the block: first row gone too, inserts after the abort work,
  process exits clean under ASan).
- [ ] `just oop-e2e` (ASan stage covers the new frames) + full battery.
  Commit.

## Task 4 — framework cache: TTL + capacity, pure `.wo`

**Files:**
- Create: `docs/examples/writeonce-framework/store/cache.wo`.
- Test: `tests/corpus/run/cache-ttl/` (fixture copies the class inline —
  corpus fixtures cannot `use` the framework; the framework file is the
  same code verified by the framework's standalone compile + Task 7's
  consumer build).

**Interfaces:**
- Produces: `pub class Cache { ttl_ms: Int, cap: Int, keys: multi Text,
  vals: map<Text, Text>, stamps: map<Text, Int> }` with `get_at(now,
  key) -> ?Text`, `put_at(now, key, value)`, and `get`/`put` sugar over
  `time.now` — the shape apps hold as a field on any long-lived
  instance.

- [ ] `get_at`: absent → nil; older than `ttl_ms` → remove the entry
  (lazy expiry — there are no timers by design) and answer nil; live →
  the value (caller-owned copy).
- [ ] `put_at`: store + stamp; when the key list exceeds `cap`, evict
  OLDEST-INSERTED until within capacity (FIFO — the file states the
  LRU tradeoff the spec settled). Re-putting an existing key refreshes
  value + stamp without duplicating the key entry.
- [ ] Values are Text; the file says "json.encode structure into it" —
  no generics exist, stated, not apologized for.
- [ ] Fixture drives injected stamps: fresh hit, expiry at exactly
  ttl+1, eviction order under cap pressure, re-put refresh; ASan run.
- [ ] Framework standalone compile stays clean. Commit.

## Task 5 — framework flags: wf_flags + cached read-through

**Files:**
- Create: `docs/examples/writeonce-framework/store/flags.wo`.

**Interfaces:**
- Produces: `@table(name: "wf_flags")` class `Flag { name: Text @unique,
  on: Int }` (Int 0/1 — Bool columns are unproven storage, spec's call)
  and `pub class Flags { loaded: Int, cache: map<Text, Int> }` with
  `read(name, default: Bool) -> Bool` and `set(name, on: Bool)`.

- [ ] `read`: first call fills the map from the table (query by name —
  the employee-proven point-read), later calls answer from the map;
  absent flag → the default, uncached (so a later `set` is seen).
- [ ] `set`: update-or-insert the row, then update the map in the same
  call — single process, invalidation is an assignment. Durability is
  the table's (WAL), restart rebuilds via `read`.
- [ ] Framework standalone compile; behavior proven in Task 7's gate
  (deviation 3). Commit.

## Task 6 — framework jobs: wf_jobs, enqueue, JobRunner, the idle seam

**Files:**
- Create: `docs/examples/writeonce-framework/store/jobs.wo`.
- Modify: `docs/examples/writeonce-framework/http/serve.wo` (Dispatcher
  gains `fn idle()`; the serve loop calls it after `net.accept`, BEFORE
  parsing the connection's first request), `app.wo` (`App` satisfies
  `idle`; `jobs(take r: Jr, budget: Int)` registration; `Jr { r:
  JobRunner }` wrapper, the Mw/Route pattern), `README.md` (the drain
  contract + the idle-server-drains-nothing disclosure).

**Interfaces:**
- Consumes: `transaction { }` (Tasks 1–3) only in the DEMO — enqueue
  itself is an ordinary insert, composition happens in app code.
- Produces: `@table(name: "wf_jobs")` class `Job { kind: Text, payload:
  Text, attempts: Int, not_before: Int }`; `pub fn enqueue(kind,
  payload)`; `pub interface JobRunner { fn run(kind: Text, payload:
  Text) -> Bool }`; `App.jobs(take r, budget)`; `Dispatcher.idle()`.

- [ ] Drain (in `App.idle`): no runner registered → return immediately.
  Else query up to `budget` due jobs (`not_before <= time.now`,
  registration order via `take`), each inside `try`: true → `delete`
  the row; false or trap → `attempts + 1` (update), row stays —
  retry/backoff policy is the app's (it can rewrite `not_before` from
  its own runner).
- [ ] The post-accept/pre-parse placement is the DETERMINISM the gate
  needs: a job enqueued by connection A never runs before A closes, and
  a kill after A's response provably leaves the row. Latency cost
  (≤ budget jobs ahead of the next request) stated in the README.
- [ ] Framework standalone compile; the serve loop's existing gates
  (`just web-app` current count) stay green with NO runner registered —
  the seam must cost nothing. Commit.

## Task 7 — the web-app demo + the gate

**Files:**
- Modify: `docs/examples/web-app/main.wo` (transactional CreateOrder +
  confirm runner + `GET /jobs` count + `POST /flags/:name` + the
  flag-gated header on the product list), `types.wo` (nothing — wf_
  tables come from the framework), `README.md`,
  `scripts/web-app-accept.sh`.

**Interfaces:**
- Consumes: everything above, through `[deps]` exactly like every other
  framework feature.

- [ ] `CreateOrder.handle`: `transaction { insert Order {...};
  enqueue("confirm", <order json>); }` — the headline composition, one
  commit. A `Confirm` runner class answers `confirm` by printing the
  order-confirmation line to stderr and returning true; registered via
  `app.jobs(Jr { r: Confirm {...} }, budget)`.
- [ ] `GET /jobs` (behind the existing bearer auth): pending count as
  JSON. `POST /flags/:name`: flips through `Flags.set`; the product
  list answers an extra header (e.g. `x-store-banner`) while the flag
  is on.
- [ ] Gate additions, in order: (a) `POST /orders` 201, then `kill -9`
  the server IMMEDIATELY (no further requests), restart on the same
  `WO_DATA`, then `GET /jobs` — the confirmation line appears in the
  restarted server's log (the drain ran post-accept on this very
  request) and the count answers 0: the job survived the kill because
  it committed WITH the order; (b) `POST /flags/banner` then
  `GET /products` carries the header, restart, still carries it;
  (c) the standing matrix unchanged. Counts stay dynamic in the script.
- [ ] Full battery: `just web-app`, `woc-test`, `oop-e2e`,
  `deps-accept`, `log-watcher`, `employee`. Commit.

## Task 8 — docs closeout

**Files:**
- Modify: `docs/00-status.md` (row 18 ✅ with measured results; NEXT
  PLAN advances to 20/21 per the order), story `18-memory-db-features.md`
  (landing banner) then `git mv` into `stories/.../done/` with links
  re-pathed and VERIFIED, `docs/00-dependency-graph.md` (node classes:
  18 done; TPRMW unblocks), framework `README.md` (ledger rows:
  storage-integration txn-per-request now buildable; the v2 pieces ✅),
  `compiler/src/CODE-LOGIC.md` + `runtime/src/CODE-LOGIC.md` +
  `database/src/CODE-LOGIC.md` (the txn seams, one paragraph each).
- [ ] Apply; run `just web-app` once more after doc edits; commit.

## Success criteria (spec, restated as the gate reads them)

1. Two inserts in one block: SIGKILL before the next request → both rows
   after restart (gate a); a trap unwinding out → neither row and the
   process keeps serving (`run/transaction-abort` + ASan).
2. The order's job runs after the NEXT accepted connection within
   budget, never before the posting connection closes, and survives a
   kill in between (gate a).
3. An expired or evicted cache entry answers nil with no timer having
   existed (`run/cache-ttl`, stamps injected).
4. A flipped flag holds across restart (gate b). Every standing gate
   green; opcode set and `.wob` format byte-identical.

## Self-review notes

- Spec coverage: Part A semantics → Tasks 1–3 (observable rules mapped
  one-to-one; the early-exit hole closed by deviation 1); cache → T4;
  flags → T5; jobs + seam → T6; demo + gate → T7; out-of-scope list
  untouched. Corpus/gate split follows deviations 2–3.
- Type consistency: the three engine entry points, the two E-codes
  (E110/E112), `wf_flags`/`wf_jobs`, `get_at`/`put_at`,
  `JobRunner.run(kind, payload) -> Bool`, `App.jobs(take r, budget)`,
  `Dispatcher.idle()` — spelled identically in every task that names
  them.
- Risk, disclosed: the abort walk's index restoration is the one place
  correctness is subtle (indexed column updated then aborted); Task 1's
  unit test pins exactly that case before any VM work stacks on it.
- Ordering: engine (T1) before VM (T3) with the language (T2) between so
  T3 has both; cache/flags (T4/T5) are independent and could land any
  time, kept after the critical path so the risky work gets the freshest
  attention.
