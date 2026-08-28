# WAL group commit — design

> databasev2 [4](../../stories/databasev2/04-io-uring-commit.md), part A.
> Brainstormed and approved 2026-08-28.
>
> **This spec covers batching only.** The iteration was split during the
> brainstorm: part A amortises one durability barrier across many statements,
> part B (io_uring submission) is deferred until A's measurement says whether
> the blocking boundary is still the bottleneck. That split matches the
> iteration's own fork 1 — "drop-in behind `wo_wal_commit` first, an async
> variant only if the scheduler proves the blocking boundary is the
> bottleneck" — and it means the throughput win arrives behind a much smaller
> correctness surface.

## Decisions taken (the brainstorm's forks, settled)

| Fork | Decision |
| --- | --- |
| Scope | **Batching first, io_uring later.** Two independent wins were being carried as one; only the first needs a new syscall interface, and it is where most of the number lives |
| Batch boundary | **Queue-drain.** Shard 0 stages every pending write request, then commits once. No timer, no tunable |
| Failure | **Fatal, diagnosed abort.** Any failure between "RAM mutated" and "record durable" ends the process |
| Batch cap | **None initially.** Measure peak staged bytes; add a cap only if the queue's existing upstream bound proves insufficient |
| Abort coverage | Unit-test the failure *return*; the abort path itself stays covered by inspection, and that gap is disclosed |

## The problem, read off the engine

The story says "replace fsync-per-commit with io_uring group-commit". Read
against the code, the premise needed correcting: the engine does not commit per
*commit*, it commits per **statement**. `db.c` calls `wo_wal_commit`
immediately after every append, at all six sites — insert, update and remove,
each on both the inline and the DB-actor path. Every single row change is one
`pwrite` plus one `fdatasync`.

That is what the numbers say too. Iteration 22's baseline records durable writes
at **4460 ops/s** single-shard and mixed writes at **1023 ops/s**, p50 **430 µs**,
p99 **664 µs** — against **1.28M ops/s** for durable reads. Writes are roughly
290× slower than reads, and the barrier is the whole of it.

**The batching machinery already exists and is simply never used.**
`wo_wal_commit` writes `w->buf` for `w->len` bytes — a staged buffer that can
hold any number of records. Today it never holds more than one, because the
caller commits immediately after staging. So part A is closer to removing calls
than to adding a mechanism.

## The design

### The commit path

The six `wo_wal_commit` calls come out of `db.c`. Applying to RAM and staging
the record stay exactly where they are; only the barrier moves, up to the point
where shard 0 runs out of work.

Shard 0 owns the WAL — DB statements from other shards arrive as marshaled
request envelopes and are executed on shard 0's thread, serialized, and a reply
envelope unparks the requester. The change is that **the reply is held rather
than sent**: shard 0 executes and stages each queued request, keeps draining
while requests remain, then issues one barrier, and only then releases every
held reply.

Each requester therefore unparks having been acknowledged after the barrier that
carried *its* record — the ack contract the story states, which today is true
only because every batch has one member.

A statement executing inline on shard 0 (rather than arriving as a request)
stages and commits before returning, as it does now. It has no reply to hold —
it returns into its own fiber — and because the drain always commits before it
ends, nothing uncommitted is ever left staged when the inline path runs.

### Why queue-drain, and what it costs

The batch boundary is the queue going empty, not a tick and not a timer. Two
properties follow, and they are the reason to prefer it:

- **A lone writer pays nothing.** One queued request means a batch of one, which
  is today's path at today's latency. Batching engages only under genuine
  contention, so an idle system is not taxed to serve a busy one.
- **The batch self-tunes.** Its size is whatever actually accumulated between
  drains, so it grows with load rather than with a configured number. There is
  nothing to set and nothing to set wrong.

The rejected alternative was the iteration's recorded leaning, the shard tick.
That leaning was recorded when the batch was assumed to ride an io_uring
submission; with batching landing first, a tick boundary would add up to one
quantum of latency even to a lone writer — paying the cost of batching when
there is nothing to batch with.

**No batch cap ships initially, and that is a decision rather than an
oversight.** databasev2 1 established that unbounded growth is precisely how
this engine dies without warning, so the instinct to bound it is right. But the
request queue is already bounded upstream by iteration 24's mailbox caps, and a
second bound on the same quantity is a knob that can only be wrong. The proof
plan measures peak staged bytes so the question is settled by a number.

## Failure: one rule, replacing three behaviours

Today's rollback is uneven, and the code says so. An `insert` whose commit fails
removes the row again, under a comment claiming RAM never claims what disk has
not acknowledged. An `update` or a `delete` whose commit fails does **not** roll
back — its comment admits the state plainly: RAM ahead of disk, trap, do not
ack. Nothing acknowledged is lost, but the process continues with divergent
state, and batching would multiply that from one row to as many as the batch
held.

The rule that replaces it: **once a statement has mutated RAM, the only outcomes
are durable or process death.** It covers both failure points identically —
a staging failure and a barrier failure have the same consequence, RAM ahead of
disk with no way back, and only one of the three verbs can undo itself.

Retrying is not an alternative worth designing for. On Linux a failed `fsync`
may already have discarded the dirty pages, so a second call can report success
having written nothing; the recovery that actually works is replay, which
returns exactly the last durable state. That is what the log is for.

**This removes `WO_T_IO` from the write path.** A program can no longer catch a
disk failure on a write. The removal is deliberate — there was never a
recovery a program could meaningfully perform with its RAM ahead of its disk —
but it is language-visible and must be stated in the story banner and the error
catalogue, not slipped in.

The diagnostic has to earn the abort: the failing operation, the `errno` text,
the WAL path, and the number of records in the batch, on stderr, then exit with
a status of its own. Exit 1 is a trap and exit 2 is a refusal, so a durability
failure takes a third. `abort()` is rejected — a core dump on a full disk is
noise, not evidence.

## Proof plan

| Claim | How it is proven |
| --- | --- |
| The payoff is real | `durable.*.seed` and `mixwrite` measured before and after on one machine, recorded in `perf-targets.md`. Today: 4460 and 1023 ops/s, p99 664 µs |
| Durability is unchanged | Iteration 22's crash battery, unaltered: concurrent writers, `kill -9` mid-stream, replay. **The critical test** — a kill between staging and the barrier must lose only unacknowledged writes |
| Batches actually form | New metrics for mean and peak batch size under contention. If batches are always one, the feature is inert and any throughput change came from somewhere else |
| No idle tax | Single-writer p99 must not regress against the current baseline |
| The cap question is answered | Peak staged bytes recorded per run |
| A failure is detected | `test_wal.c` asserts `wo_wal_commit` reports failure on a bad descriptor |

**One disclosed gap.** Forcing a genuine `fdatasync` failure needs a full or
read-only filesystem, which the gate cannot arrange without mount privileges.
The unit test proves the error is *detected*; the abort that follows it stays
covered by inspection. The alternative — a fault-injection switch — means
shipping a binary that can be told to kill itself, which is a worse trade. This
gap is recorded rather than hidden, because iteration 40 was exactly a fatal
path that nothing exercised.

## Out of scope

- **io_uring submission.** Part B, and it only earns its complexity if A's
  measurement shows the blocking boundary still dominating. A's parking and ack
  machinery is what B would build on, so nothing here is wasted either way.
- **`transaction { }`** — language iteration 18. A transaction already *is* a
  staged batch, so the two compose without either knowing about the other; that
  is a reason not to entangle them now.
- **Checkpoint and compaction** — databasev2 3. This changes when the barrier
  runs, never what the log contains.
- **The read path.** databasev2 1 measured that appending under memory pressure
  costs about 1% while random reads cost 273×, so the pressure is on reads —
  but that is iteration 2's `resident: keys` question, not this one.
- **Rollback with pre-images.** Rejected above: it would add per-write cost on
  every statement to serve a path that ends the process anyway.

## Alternatives rejected

**Tick-boundary batching** — the iteration's recorded leaning, superseded by
the split. It taxes an idle system to serve a busy one.

**Count-or-timer batching** — two tunables, and the timer reintroduces the tick
problem with extra configuration.

**Full rollback with an undo log** — keeps `WO_T_IO` catchable, at the price of
capturing pre-images for every update and delete, paid on every write, to
support continuing in a state the engine cannot trust.

**Keeping today's per-verb behaviour** — turns a rare one-row divergence into a
routine N-row one, silently.
