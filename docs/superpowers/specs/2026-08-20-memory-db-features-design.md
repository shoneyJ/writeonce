# Iteration 18 — memory-rich features over the embedded database: design

> **Status: spec, awaiting review (2026-08-20).** Decisions were settled in
> [the iteration](../../stories/language-runtime-database/18-memory-db-features.md);
> this spec makes them buildable. The plan follows after review.
> Board: [docs/00-status.md](../../00-status.md).
>
> Per repo convention: concept, reason, and required behavior in words
> only — no implementation code.

## Goal

Four pieces, one theme — the single binary's memory and its durable store
are the same process, so features other stacks assemble from Redis, a
broker, and an outbox pattern become a class, a table, and one language
block: a TTL cache (`framework/cache.wo`), `@table`-backed feature flags
with a cached read (`framework/flags.wo`), a durable job queue drained
in-process (`framework/jobs.wo` + a serve-loop seam), and `transaction { }`
exposing the WAL's existing staged batch so a job enqueue and the business
write it belongs to are ONE commit.

## Part A — `transaction { }` (the one language + engine seam)

### Observable semantics (normative)

- `transaction { <statements> }` is a statement. Every `insert`, `update`
  (field assignment on a table row), and `delete` inside the block becomes
  durable together: exactly one WAL write + fdatasync at the closing
  brace. Before that point, none of it is durable.
- Reads inside the block see the block's own writes (RAM stays applied
  immediately — the engine's RAM-authoritative doctrine is unchanged).
- A trap that unwinds OUT of the block aborts it: the staged WAL batch is
  discarded and every RAM effect of the block is undone — rows inserted
  are removed (indexes included), updated rows revert to their pre-images,
  deleted rows are restored. The trap then continues to the enclosing
  handler exactly as it would have without the block; `try` INSIDE the
  block that catches a trap keeps the transaction alive (statement-level
  failure, e.g. a WO_T_UNIQUE insert, stages nothing for that statement —
  same as today).
- Commit failure (WAL write/fdatasync error at the closing brace) is the
  existing engine failure trap; the batch stays staged per the WAL
  contract and the abort path above runs as the trap unwinds.
- Nesting is rejected at compile time: a `transaction { }` lexically or
  dynamically inside another is **WO-E110** (lexical nesting is a parse
  check; a transactional function called inside a block traps WO_T_DB
  "nested transaction" at run time — the compiler cannot see across
  calls, the VM can). WO-E108/E109 stay reserved for parked iteration 17.
- An empty block commits nothing and costs no syscall (the WAL's
  empty-batch rule, already in the contract).
- Crash between commit and anything else: recovery replays the WAL —
  either the whole block's rows exist or none do. This is the acceptance
  criterion's SIGKILL proof.

### Engine seam (`database/src`)

`db.c` today calls `wal_append_*` then `wo_wal_commit` per statement — the
staging machinery is already transactional in shape. The change: a
transaction-depth flag on the db handle; while set, statements append but
do NOT commit; the block's end commits once. Abort needs pre-images: while
the flag is set, the engine records an undo entry per statement BEFORE the
RAM apply (insert → the new row id, to remove; update → a copy of the row
before the change; delete → a copy of the removed row, to restore, index
entries included). Abort walks the undo list in reverse, then discards the
staged batch. The undo list exists only while a transaction is open —
zero cost otherwise.

### VM + compiler surface

No new opcodes, no `.wob` version bump: the block lowers to
compiler-emitted internal builtins (begin / commit) that user code cannot
name, plus an abort marker on the trap-unwind path — the catch-frame
machinery already unwinds regions; a transaction region behaves like a
catch frame whose only action is "abort the transaction, keep unwinding".
The parser adds the `transaction` keyword and the WO-E110 nesting check;
the ownership and GC passes see an ordinary block.

## Part B — the framework pieces (pure `.wo`)

Framework-owned tables use the `wf_` name prefix — a dependency's tables
land in the consuming app's database, so the prefix marks whose they are
(disclosed in the framework README).

### `framework/cache.wo` — TTL + capacity cache

A `Cache` class the app holds as a field on any long-lived instance
(App, a middleware, a handler): `ttl_ms: Int`, `cap: Int`, insertion-order
key list, value map, stamp map (`time.now` is wall-clock milliseconds).
`get(key) -> ?Text`: nil when absent or older than ttl_ms (the expired
entry is removed on that read — lazy expiry, there are no timers by
design). `put(key, value)`: stores, stamps, and when size exceeds `cap`
evicts the OLDEST-INSERTED entries until within capacity — FIFO, decided
over LRU: true LRU needs reordering on every read (O(n) in the key list)
for a benefit v1 does not measure; the tradeoff is stated in the file.
Values are Text — the language has no generics; structured values go
through `json.encode`/`decode` (stated in the file).

### `framework/flags.wo` — feature flags

`@table(name: "wf_flags")` class `Flag { name @unique, on: Int }` — `on`
is 0/1 because Int columns are the proven storage ground; a Bool column
is not, and flags do not get to be the probe. A `Flags` wrapper class
(held like the cache): `read(name, default) -> Bool` answers from an
in-memory map filled from the table on first read; `set(name, on)` writes
the table (update-or-insert) AND updates the map in the same call — one
process, so "cache invalidation" is an assignment. A restart rebuilds the
map from the table: flags are durable.

### `framework/jobs.wo` + the drain seam — background jobs

- `@table(name: "wf_jobs")` class
  `Job { kind: Text, payload: Text, attempts: Int, not_before: Int }`
  (`not_before` in `time.now` milliseconds; 0 = immediately due).
- `enqueue(kind, payload)` inserts a due job — called by app code, and
  called INSIDE `transaction { }` next to the business write it belongs
  to; that composition is the point of Part A.
- `JobRunner` interface: `fn run(kind: Text, payload: Text) -> Bool` —
  true = done, false = keep. The app implements it as a class (the
  Handler doctrine), dispatching on `kind` itself in v1.
- Registration: `App` gains `jobs(take r: Jr, budget: Int)` (`Jr` wraps
  the interface value, the `Mw`/`Route` pattern). No registration = the
  seam costs nothing.
- The seam: the `Dispatcher` interface gains `fn idle()`; the serve loop
  calls it **after accepting a connection, before parsing its first
  request**. That placement is deterministic where "after the response"
  is not: a job enqueued by connection A provably does NOT run before A
  closes, and a SIGKILL after A's response provably leaves the row —
  which is exactly what the durability acceptance needs to observe. The
  cost — up to `budget` jobs of latency ahead of the next request — is
  the disclosed price of drain-on-request; an IDLE server drains nothing
  (iteration decision, restated in the file; fibers (11) replace the
  scheduler, the table and interface stay).
- Draining: query up to `budget` due jobs (`not_before <= time.now`,
  registration-order `take budget`), each inside `try`: true → the row is
  deleted; false or trap → `attempts` increments and the row stays
  (retry/backoff policy is app-side in v1 — the app can rewrite
  `not_before` from its own runner).

### `web-app` demonstration

`CreateOrder` wraps its insert and a `confirm` enqueue in one
`transaction { }`; a runner class answers `confirm` by printing an
order-confirmation line to stderr (observable in the gate's server log)
and returning true. `GET /jobs` (behind the existing auth) answers the
pending-job count as JSON — the gate's counting window. Flags demo:
`POST /flags/:name` (auth'd) flips a flag through `Flags.set`, and the
product list answers an extra response header while the flag is on —
small, observable, durable across restart.

## Gate (`scripts/web-app-accept.sh` + corpus)

Corpus (single-project fixtures — no manifest needed, so `transaction`
tests live here, unlike 17's):

- `run/transaction-commit`: two inserts in one block; both rows readable
  after.
- `run/transaction-abort`: a block whose second insert traps
  (WO_T_UNIQUE); after the trap is caught OUTSIDE the block, the FIRST
  insert's row must be gone too, and inserts after the abort still work.
- `compile-fail/transaction-nested`: lexical nesting is WO-E110.

Gate additions (order matters):

1. `POST /orders` answers 201 (the transactional enqueue); `kill -9`
   the server immediately; restart; the confirmation line appears in the
   restarted server's log on the next request (`GET /jobs` → 0 after the
   drain) — the job survived the kill because it committed WITH the order.
2. A flags check: `POST /flags/:name` flips it, the next `GET /products`
   carries the flag-gated header, restart, still carries it.
3. The standing matrix stays green; the count goes wherever it lands
   (numbers are dynamic in the script).

The cache class is gate-covered indirectly and probe-covered directly:
its fixture (`run/cache-ttl`) injects stamps rather than sleeping —
expiry logic must be testable without wall-clock waits.

## Out of scope (restated from the iteration)

Pub/sub and WebSockets (behind 8/11); job priorities and cron shapes;
retry/backoff policy in the framework; exposing the WAL batch API beyond
`transaction { }`; generic cache value types (no generics in the
language); Bool table columns; multi-node anything.

## Success criteria

1. **Given** two inserts in `transaction { }` and SIGKILL before the next
   request, **when** the server restarts, **then** both rows exist —
   and **given** a trap unwinding out of the block, **then** neither
   does, and the process keeps serving (`run/transaction-abort` +
   gate check 1).
2. **Given** an order POST, **then** its job runs after the next accepted
   connection within budget, never before the posting connection closes,
   and survives a kill in between (gate check 1).
3. **Given** an expired or evicted cache entry, **then** `get` answers
   nil without any timer having existed (`run/cache-ttl`).
4. **Given** a flag flipped and the process restarted, **then** the flag
   holds (gate check 2). All standing gates stay green; VM opcode set and
   `.wob` format unchanged.
