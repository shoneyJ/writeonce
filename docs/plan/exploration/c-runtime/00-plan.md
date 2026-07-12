# wo-rt-c roadmap — multi-threaded io_uring RAM database runtime, in C

> **Kanban: ✅ done** — phases A–F all shipped with measured exit evidence below. Board: [../../00-kanban.md](../../00-kanban.md)

**Context sources:** [`prototypes/wo-rt-c/wo-rt.c`](../../../../prototypes/wo-rt-c/wo-rt.c) (phase 0 — the single-threaded epoll baseline), [`../../09-concurrency-scaleout.md`](../../09-concurrency-scaleout.md) (the thread-per-core doctrine every phase here miniaturizes), [`../../10-storage-foundations.md`](../../10-storage-foundations.md) / [`11-wal-and-recovery.md`](../../11-wal-and-recovery.md) / [`12-engine-disk-cutover.md`](../../12-engine-disk-cutover.md) (the storage track), kernel reference cards [`../linux/07-io_uring.md`](../linux/07-io_uring.md), [`08-mmap.md`](../linux/08-mmap.md), [`09-fallocate.md`](../linux/09-fallocate.md), [`12-pwrite-fsync.md`](../linux/12-pwrite-fsync.md), [`02-eventfd.md`](../linux/02-eventfd.md).

## Goal

Evolve the [`prototypes/wo-rt-c/`](../../../../prototypes/wo-rt-c/) prototype from a single-threaded epoll reference into a **multi-threaded runtime environment for writeonce applications**: thread-per-core io_uring event loops at million-scale read/write concurrency, the whole database resident in RAM (one mmap arena, addressed per shard — no duplication), **ACID** commits that dual-write RAM-first-then-disk, and a boot path that loads the hard drive's state back into RAM before serving. Still one C file's worth of honesty per concern, still **zero dependencies beyond libc** — raw io_uring syscalls, no liburing.

Each phase is the executable proving ground for the matching Rust plan (09–12): get the syscall sequence right here in a few hundred lines, then port with confidence.

## Design decisions (locked — by plan 09 and by the project owner)

1. **Thread-per-core, shared-nothing.** `WO_THREADS` (default `sysconf(_SC_NPROCESSORS_ONLN)`) pthreads, each pinned with `sched_setaffinity`, each owning its event loop, its listener, its connections, its shard. No work stealing, no connection migration, no locks or atomics on the data path (plan 09 decisions 1, 2, 7).
2. **One RAM arena, sharded by address — no duplication.** A single `mmap` region partitioned into N shard slices. Thread *t* touches only addresses inside slice *t*. Data exists exactly once in memory; ownership, not copying, is the concurrency model.
3. **Physical RAM via kernel primitives.** `MAP_POPULATE` to fault pages in up front, `mlock` to pin them (no swap on the read path), `MAP_HUGETLB` attempted with 4 KB fallback. The read path never touches a file descriptor.
4. **Per-thread io_uring ring, raw syscalls.** `io_uring_setup` + mmap'd SQ/CQ rings + `io_uring_enter`, `IORING_SETUP_SINGLE_ISSUER`. No ring sharing (plan 09 decision 4). No liburing — the zero-dep stance is the point of the prototype.
5. **ACID at per-shard scope.**
   - *Atomicity* — WAL records are framed `len | crc32 | payload | COMMIT`; a record replays whole or not at all.
   - *Consistency* — invariants checked in-thread before the RAM apply; an aborted op writes nothing.
   - *Isolation* — one thread is one serial execution stream: per-shard serializability by construction.
   - *Durability* — the HTTP ack is sent only after the fsync completion for the commit's WAL tick.
   Cross-shard transactions (2PC) belong to the Rust track ([plan 09e](../../09-concurrency-scaleout.md)) — non-scope here.
6. **Dual-write order.** Commit = apply to the RAM slot → append WAL record (write SQE) → one `fdatasync` SQE per loop tick covering all of that tick's commits (group commit) → ack. Boot = replay per-shard WAL/snapshot from disk into the arena, in parallel, before listeners open.

## Phase sequence

One phase per implementation pass; `make` + the smoke endpoints stay green after every phase.

### Phase A — thread-per-core skeleton — ✅ shipped
*Maps to [plan 09a](../../09-concurrency-scaleout.md); cards: `SO_REUSEPORT`, [`eventfd`](../linux/02-eventfd.md).*

`WO_THREADS` pthreads, each pinned to its core; per-thread epoll loop (io_uring arrives in C), per-thread `SO_REUSEPORT` listener on the same port (kernel balances accepts by 4-tuple), per-thread `conns[]` and request counters. Shutdown: thread 0 owns the `signalfd`; on signal it writes each thread's `eventfd`, every loop exits, `pthread_join` all. Store stays per-thread arrays until B. Responses gain `"shard":t`; `/` reports `"threads":N` and per-shard counters.

**Exit (met):** all endpoints green at `WO_THREADS=4`; `/proc/<pid>/task/*/status` shows each thread pinned to its own core (tid→cpu 0,1,2,3); `/` counters proved 200 concurrent requests spread `[68,36,44,53]` across 4 shards; SIGTERM broadcast joined all shards cleanly; `WO_THREADS=1` behaves like phase 0 (shard 0, sequential ids). Note ids are now interleaved per shard (t+1, t+1+N, …) for coordination-free global uniqueness.

### Phase B — the RAM arena — ✅ shipped
*Maps to [plan 10](../../10-storage-foundations.md); cards: [`mmap`](../linux/08-mmap.md), [`fallocate`](../linux/09-fallocate.md).*

One arena: a header page (magic, version, shard count, slot geometry) + N shard slices of fixed-size row slots + a per-shard allocation bitmap. `mmap(MAP_ANONYMOUS|MAP_PRIVATE [|MAP_HUGETLB], MAP_POPULATE)` then `mlock` (graceful fallback + warning if `RLIMIT_MEMLOCK` refuses). Rows move from arrays into slots; every access is a typed pointer into the owning thread's slice — decision 2 made literal.

**Exit (met):** same API; `/` reports `"arena":{bytes, mapped, hugepages, mlocked, slot geometry}` + `shard_used[]`; `smaps_rollup` showed `Locked: 276 kB` exactly matching the mapping; hugepage attempt fell back to 4 K pages gracefully on a box with no hugepage pool; rows live at `(shard, slot)` addresses (`"slot":n` in create responses) — the stable coordinates phase D's WAL records will carry.

### Phase C — io_uring event loops + keep-alive — ✅ shipped
*Maps to plan 09 decision 4; card: [`io_uring`](../linux/07-io_uring.md).*

Replace each thread's epoll loop with a raw ring: `io_uring_setup`, mmap SQ/CQ, `io_uring_enter`; multishot accept, `recv`/`send` SQEs, `user_data = fd | (op << 32)`, the shutdown `eventfd` watched via a POLL_ADD SQE. Rewrite the connection state machine for **HTTP keep-alive** with a per-connection write queue — connection-per-request caps throughput far below the million-scale target. Largest phase (~400 LOC); the ring-setup block is the C reference the Rust port reads.

**Exit (met):** four requests over one socket (`curl` reported `num_connects: 1, 0, 0, 0`), and a create+list pair on one connection lands on the same shard with both rows visible; `strace -c` over 60 keep-alive requests showed **124 `io_uring_enter` and zero `epoll_wait`/`recvfrom`/`sendto`/`accept`** — the only `read`/`write` calls were the signalfd/eventfd shutdown path; SIGTERM broadcast joined all shards cleanly. Raw ring (`io_uring_setup` + SINGLE_MMAP rings + `io_uring_enter`), multishot accept with `CQE_F_MORE` re-arm, one outstanding SQE per connection, pipelined-tail carry-over.

### Phase D — WAL dual write: RAM first, then the hard drive — ✅ shipped
*Maps to [plan 11](../../11-wal-and-recovery.md) + 09c; cards: [`pwrite/fsync`](../linux/12-pwrite-fsync.md), [`fallocate`](../linux/09-fallocate.md).*

Per-shard `shard-<t>.wal`, preallocated with `fallocate`. The commit path is decision 6 verbatim: RAM apply → framed record append (write SQE at the shard's tail offset) → one `fdatasync` SQE per tick → ack on the fsync CQE. CRC32 hand-rolled. Acks for all commits in a tick ride the same fsync — group commit, exactly the Rust runtime's doctrine.

**Exit (met):** crash test — 60 concurrent POSTs, `kill -9` mid-stream — showed **60/60 acked writes present and CRC-valid in the WALs, zero acked-but-missing** (offline verification via the new `wo-rt wal-check <file>` mode, which is also phase E's replay skeleton); a copy truncated mid-record reported `TORN at byte 2584 — 17 whole records before it`, dropping the partial whole; 200 concurrent durable commits in 102 ms (~1,960 commits/s, curl-process-bound — each tick's fsync covers every commit staged that tick via double-buffered write→fsync `IOSQE_IO_LINK` pairs). Failed fsync closes the batch's connections without acking — a client never sees a 201 for a non-durable write. Phase D boots with `O_TRUNC` (fresh log); replay lands in E.

### Phase E — first load: hard drive → RAM — ✅ shipped
*Maps to plans [11](../../11-wal-and-recovery.md)/[12](../../12-engine-disk-cutover.md).*

Boot, before any listener opens: each thread replays its own WAL into its arena slice — parallel recovery — validating frame CRC + commit marker, truncating at the first torn record. Clean shutdown writes a snapshot (`pwrite` of live slots to `shard-<t>.data`) and truncates the WAL; boot prefers snapshot + WAL tail. Recovery time printed at startup.

**Exit (met):** the full cycle verified — (1) 40 writes + `kill -9` + restart: `shard_used [10,7,15,8]` identical, replayed from WAL alone (`recovered 0 snapshot rows + N wal records in 1 ms` per shard); (2) SIGTERM wrote four snapshots and truncated the WALs; (3) restart loaded the snapshots instantly; (4) 5 more writes + `kill -9` + restart recovered **snapshot + WAL tail combined** (`recovered 7 snapshot rows + 2 wal records`), totals exact at 45/45; (5) a restart with `WO_THREADS=8` against a 4-shard data dir **refuses to boot** (`meta` guard — resharding is 09f, never silent data loss). Recovery 1–3 ms at demo geometry; large-arena timing rides phase F's harness, which can generate volume natively (geometry is `-D` overridable: `SLOTS_PER_SHARD`/`SLOT_SIZE`).

### Phase F — million-scale harness + ACID verification — ✅ shipped
*Maps to [plan 09's verification-targets table](../../09-concurrency-scaleout.md).*

`setrlimit(RLIMIT_NOFILE)` raised at boot. A small C load client under `prototypes/wo-rt-c/bench/` (keep-alive, pipelined GETs, latency timestamps — `wrk` would be an external dep). Measure honestly on the dev box and commit the numbers to the prototype README: aggregate read req/s across cores (goal order 10⁶/s on 8–16 cores), concurrent open connections (goal order 10⁵–10⁶; ~8 KB/conn + fd limits are the ceiling), commits/s under group fsync, p99 read latency under write load. ACID scripts: torn-WAL injection (atomicity), single-shard interleaving probe (isolation), the phase-D crash test under load (durability). A `just rt-c-bench` recipe runs it all.

**Exit (met):** measured on a 20-core box (table in the [prototype README](../../../../prototypes/wo-rt-c/README.md)): **908,916 reads/s p99 154 µs and 643,250 fsync-acked commits/s p99 177 µs** on 8 shards — vs Go `net/http` on 20 cores at 495k/355k with ~8× worse p99 and no durability (.NET unavailable on the box); 10k idle connections, 0 errors; only 2xx counted (the client tracks status codes). **The crash-under-load test found two real durability bugs the phase-D test missed** — an ack-armed-before-fsync race in `conn_continue` (route parks the response *during* `try_process`; the pre-check missed it) and an fd-reuse ABA hazard in batch ack-parking (fixed with per-connection generation stamps). After both fixes, three `kill -9`-mid-bench rounds at ~1–2M commits each showed **WAL records ≥ acked, every round** (one exact). Isolation: 300 concurrent commits → 300 distinct interleaved ids. Geometry scaling via `-DSLOTS_PER_SHARD` (bitmap region generalized to multi-page); 512 MB arena verified mlocked.

## Non-scope

- **No cross-shard transactions.** 2PC is plan 09e on the Rust track; the prototype's rows are independent by design.
- **No liburing, ever.** Raw syscalls are the deliverable — the Rust port needs the sequence, not a wrapper.
- **No multi-node anything.** Single box, threads-as-shards, same stance as plan 09.
- **No TLS, no HTTP/2.** The HTTP layer stays minimal; the runtime is the subject.
- **The prototype directory stays a prototype.** Lessons flow into `crates/rt`; production code never imports from there.

## Cross-references

- [`../../09-concurrency-scaleout.md`](../../09-concurrency-scaleout.md) — the doctrine; this prototype is its executable proving ground (A↔09a, C↔09 decision 4, D↔09c).
- [`../../10-storage-foundations.md`](../../10-storage-foundations.md), [`11-wal-and-recovery.md`](../../11-wal-and-recovery.md), [`12-engine-disk-cutover.md`](../../12-engine-disk-cutover.md) — the storage track phases B/D/E miniaturize.
- [`../../../../prototypes/wo-rt-c/README.md`](../../../../prototypes/wo-rt-c/README.md) — current state and module map (phase 0).
- [`./01-architecture.md`](./01-architecture.md) — the target architecture traced through one memory address at million-connection concurrency, plus improvement proposals (seqlock reads, registered buffers, SEND_ZC, SQPOLL) that slot into phases C/F.
- [`./02-single-binary.md`](./02-single-binary.md) — the end goal: how the `wo build` single binary runs on this runtime environment (Go model, not JVM — the kernel is statically linked into every app; the embedding contract between compiler payload and runtime kernel).
- [`../../../../prototypes/wo-db/`](../../../../prototypes/wo-db/) — the query-layer sibling; one day a phase-G could splice its engine on top of this runtime.
