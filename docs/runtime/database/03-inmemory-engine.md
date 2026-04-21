# Phase 3 — In-Memory Engine

> RAM-primary, SSD-durable storage using io_uring, designed for OLTP e-commerce workloads on a 64 GB Linux machine.

**Previous**: [Phase 2 — The `.wo` Language & ACID Engine](./02-wo-language.md) | **Next**: [Phase 4 — Client API](./04-client-api.md) | **Index**: [database.md](../database.md)

---

Seed constraints:

- 64 GB RAM — the entire live dataset fits in memory; no page eviction on the hot path
- Dual write — every mutation goes to an in-RAM structure **and** to an on-SSD durable log simultaneously
- Linux-only — Linux kernel APIs are fair game, no portability obligation
- `io_uring` for asynchronous read/write to SSD

This is a **RAM-primary, SSD-durable** engine — the modern OLTP architecture used by TigerBeetle, ScyllaDB (via Seastar), VoltDB/H-Store, SAP HANA, and Redis-with-AOF. Reads never touch disk. Writes touch RAM immediately and SSD asynchronously, with fsync gating commit acknowledgment.

For the e-commerce workload in [Phase 2](./02-wo-language.md), this is the right physical design: checkout latency is dominated by the durability path, not lookup; and a 64 GB live set comfortably holds millions of products + recent orders + active sessions + the entire recommendation graph.

## Memory Layout

All three paradigm engines live in one address space:

```
┌──────────────────────────────────────────────────────────────┐
│                     Process address space                    │
├──────────────────────────────────────────────────────────────┤
│  Relational heap     B+ tree pages (row-store)      ~20 GB   │
│  Document store      LSM memtable + sorted runs      ~15 GB  │
│  Graph store         Node + edge arenas, adjacency   ~10 GB  │
│  Index shards        Hash, sorted, inverted           ~8 GB  │
│  Buffer for WAL      Ring buffer staged for SSD       ~2 GB  │
│  MVCC version chains Per-record visibility history    ~5 GB  │
│  Connection / query  Per-session scratch              ~2 GB  │
│  Headroom / OS       Free for kernel, page tables     ~2 GB  │
└──────────────────────────────────────────────────────────────┘
```

Key moves:

- **`mlockall(MCL_CURRENT | MCL_FUTURE)`** — pin all pages, guarantee no swap-out. A single swap-in during checkout is a latency catastrophe.
- **`MAP_HUGETLB` / Transparent Huge Pages** — 2 MB pages reduce TLB pressure on hot indexes. For 64 GB of data, 4 KB pages mean 16 M TLB entries; 2 MB pages mean 32 K.
- **`/proc/sys/vm/swappiness = 0`** — belt and braces with `mlockall`.
- **NUMA awareness** — optional on multi-socket hosts. Since the engine is single-threaded ([Phase 2 concurrency model](./02-wo-language.md#concurrency-model)), pin the one event-loop thread and bind the arena to that socket (`numactl --membind=0 --cpunodebind=0`). Cross-socket memory access is 2–3× slower; a single-socket deployment sidesteps it entirely.
- **Slab / arena allocators** — avoid `malloc` in the hot path. Pre-size arenas per paradigm at startup.

## Dual-Write Durability Path

A write is committed only when its WAL record is on the SSD with `fsync` confirmed. The in-memory structure is updated first (fast), then the log write is awaited (slow enough to matter):

```
  Transaction COMMIT
        │
        ▼
  ┌──────────────────────┐
  │ 1. Stage mutations   │  apply to RAM structures under MVCC
  │    to in-memory      │  (version chains; readers unaffected)
  │    engines           │
  └──────────────────────┘
        │
        ▼
  ┌──────────────────────┐
  │ 2. Serialize WAL     │  append-only ring buffer in RAM
  │    record            │  header + paradigm deltas + LSN
  └──────────────────────┘
        │
        ▼
  ┌──────────────────────┐
  │ 3. io_uring submit   │  IORING_OP_WRITE with O_DIRECT
  │    WAL write to SSD  │  batched with concurrent commits
  └──────────────────────┘
        │
        ▼
  ┌──────────────────────┐
  │ 4. io_uring submit   │  IORING_OP_FSYNC
  │    fsync (barrier)   │  linked SQE after the write
  └──────────────────────┘
        │
        ▼
  ┌──────────────────────┐
  │ 5. CQE received      │  commit marker flipped
  │    → ack client      │  MVCC snapshot published
  └──────────────────────┘
```

Steps 1–2 are synchronous; 3–5 are asynchronous. The event loop submits the SQEs and moves on to the next client; it reaps the CQE on a later tick — so the loop can have thousands of commits in flight without parking on any fsync syscall.

**Group commit**: the loop drains the commit queue into one fsync SQE per tick. If 500 transactions all committed within a 100 μs window, one fsync durable-s the batch. Amortizes SSD latency (~50–100 μs on NVMe) across the batch — throughput approaches `batch_size / fsync_latency`, which on a good NVMe is 500K+ commits/sec.

**What "dual write" means here**: it is *not* a two-database write where both must succeed independently. It is one logical commit that updates RAM (the query surface) and appends to the SSD WAL (the recovery record). On crash, RAM is gone; recovery replays the WAL to rebuild RAM state. The SSD is the source of truth for *durability*; RAM is the source of truth for *reads*.

## io_uring Mechanics

`io_uring` (Linux 5.1+, mature by 5.11) is the replacement for `epoll` + `libaio` for storage I/O. Two lock-free ring buffers shared between user-space and kernel:

| Ring | Direction | Contents |
| --- | --- | --- |
| **SQ** (Submission Queue) | Userland → Kernel | SQEs: `IORING_OP_WRITE`, `IORING_OP_FSYNC`, `IORING_OP_READ`, etc. |
| **CQ** (Completion Queue) | Kernel → Userland | CQEs: result code + user_data pointer back to the request |

Configuration knobs that matter for a database:

- **`IORING_SETUP_SQPOLL`** — a kernel thread polls the SQ. Userland writes SQEs without any syscall. Read/write submission becomes a memory write + memory barrier. Cost: one pinned kernel thread per ring.
- **`IORING_SETUP_IOPOLL`** — busy-poll for completions on the device instead of interrupt-driven. Lower latency on NVMe, higher CPU. Requires `O_DIRECT`.
- **`IORING_REGISTER_BUFFERS`** — pre-register WAL ring-buffer pages with the kernel. Skips per-I/O page pinning.
- **`IORING_REGISTER_FILES`** — pre-register the WAL fd. Skips fd table lookups per I/O.
- **Linked SQEs (`IOSQE_IO_LINK`)** — enforce ordering: write-then-fsync, or WAL-then-commit-marker. Kernel guarantees link order without userland waiting on the intermediate CQE.
- **`O_DIRECT`** on the WAL file — bypass the kernel page cache. The database manages its own buffering; double-caching wastes the 64 GB.

Per-commit path with full optimization: no syscalls at all for submission (SQPOLL), one memory read for completion (IOPOLL), zero page-pinning cost (registered buffers), zero fd-table lookup (registered files). The commit loop is effectively as fast as the NVMe firmware allows.

## Recovery

RAM is volatile; on restart the engine is empty. Recovery rebuilds it:

1. **Open WAL**. Scan forward from the last checkpoint LSN.
2. **Replay committed records.** Apply each to the in-memory engines in LSN order. Skip incomplete transactions (no commit marker).
3. **Load checkpoint snapshot** (optional but standard). Periodically, the engine dumps a consistent snapshot of the RAM state to SSD. On recovery, load snapshot → replay WAL from snapshot LSN forward. Avoids replaying hours of log.
4. **Rebuild indexes.** Indexes are derived from heap data — rebuilt during replay or lazily on first access.
5. **Open for traffic.**

Recovery target: 60 GB of data + a few million WAL records = seconds to a minute on NVMe, not hours. A good checkpointer runs in the background every 5–15 minutes; recovery only replays the delta since the last checkpoint.

## Concurrency in RAM

No page eviction, no buffer pool locks — and because the engine is [single-threaded](./02-wo-language.md#concurrency-model), no cross-thread races either. The concurrency story collapses to "there is no concurrency within the engine; there is a queue of clients being served sequentially by one loop". Every data structure is owned by that one loop:

| Structure | Primitive |
| --- | --- |
| B+ tree (relational) | Plain owned tree; no latches, no optimistic locks |
| LSM memtable (document) | Plain skiplist; sealed memtables still immutable for background compaction SQEs |
| Graph adjacency | Plain hashmap per node label |
| MVCC version chain | Plain singly-linked version list; no CAS |
| WAL ring buffer | Single-producer, single-consumer ring |
| Txn coordinator | Plain `u64` counter — incremented without atomics |

**Readers still see snapshots.** MVCC remains useful but its purpose changes: instead of "readers don't block writers on another thread", it's "a live-query subscriber reading in the same tick sees the pre-commit view; the post-commit delta arrives on the next tick". That semantic is cheap to implement when there is only one mutator.

**The model is sequential.** Clients are served round-robin by the loop; nothing races because nothing runs concurrently inside the engine. When one core isn't enough, [shard](./02-wo-language.md#concurrency-model) rather than bolting multi-threading onto this design.

## Capacity Planning

64 GB is a budget, not a guarantee. Three failure modes to design around:

1. **Working set exceeds RAM.** Solution path: add a tier (warm SSD-backed region for cold rows), or shard across nodes. Neither is in the Phase 2 scope — flag when live data approaches 50 GB.
2. **MVCC version chains bloat.** Long-running transactions hold old versions alive. Solution: aggressive vacuum, transaction timeouts, snapshot horizon tracking. Postgres hits this same wall.
3. **Sudden write bursts flood the WAL.** Solution: admission control — if SSD write queue depth exceeds a threshold, slow down `COMMIT` acknowledgment. Better than OOM'ing the WAL buffer.

## Comparison With Alternatives

| Aspect | In-memory + WAL (this design) | Disk-primary (Postgres) | Pure in-memory (Redis w/o AOF) |
| --- | --- | --- | --- |
| Read latency | ~100 ns (RAM) | ~10 μs (buffer cache hit) to ms (miss) | ~100 ns (RAM) |
| Write latency | ~50–100 μs (fsync) | ~50–100 μs (fsync) | ~100 ns (none) |
| Durability | Full — WAL fsync before ack | Full — WAL fsync before ack | Window of loss (AOF every-sec) |
| Dataset size | Bounded by RAM | Bounded by disk | Bounded by RAM |
| Restart time | Seconds to minutes (WAL replay) | Seconds | Immediate (empty) or minutes (AOF) |
| Ideal workload | OLTP with small-to-medium dataset | General-purpose, large datasets | Cache, session, ephemeral |

This design keeps the durability of Postgres and the read speed of Redis.

## Linux Tuning Checklist

Before production benchmarking:

- `echo 0 > /proc/sys/vm/swappiness`
- `echo never > /sys/kernel/mm/transparent_hugepage/enabled` (databases typically prefer explicit hugepages over THP's defragmentation stalls)
- `vm.nr_hugepages = <enough for the arenas>`
- `ulimit -l unlimited` (for `mlockall`)
- `blk-mq` scheduler: `none` or `mq-deadline` on NVMe (not `cfq`/`bfq`)
- `IORING_SETUP_SINGLE_ISSUER` — always, since the engine is single-threaded (Linux 6.0+)
- NUMA: `numactl --membind=0 --cpunodebind=0` to pin the loop + its arena to one socket. Multi-socket deployments should shard across sockets rather than sharing one engine
- Disable CPU frequency scaling (`cpupower frequency-set -g performance`) — saves microseconds that add up across group-commit batches
- Disable Meltdown/Spectre mitigations only if you control the hardware and understand the trade-off — they cost 10–30% on syscall-heavy paths, but io_uring with SQPOLL largely sidesteps them anyway

## Reference Implementations

- **TigerBeetle** — Zig, in-memory, io_uring end-to-end, deterministic, designed for financial OLTP. The closest living example of this exact architecture. <https://github.com/tigerbeetle/tigerbeetle>
- **ScyllaDB / Seastar** — C++, io_uring (and SPDK), shared-nothing per core, NUMA-aware. Seastar is the framework underneath. <https://github.com/scylladb/seastar>
- **VoltDB (H-Store)** — Java, in-memory OLTP, command-logging for durability. The academic ancestor of this design pattern.
- **Redis (`appendonly yes` + `appendfsync always`)** — simpler but exact same shape: RAM-primary, log-durable.
- **LMDB** — memory-mapped B+ tree; reads are literal pointer chases into mmap'd pages. Not WAL-based but worth studying for RAM-resident read paths.
- **SingleStore (formerly MemSQL)** — commercial in-memory row-store with columnar on-disk secondary. Hybrid of this design and disk-primary.
- **Readings**:
  - *The End of an Architectural Era* (Stonebraker et al., 2007) — the H-Store paper that argued disk-primary databases were legacy for OLTP.
  - *Efficient Lock-Free Durable Sets* (Zuriel et al.) — for lock-free structures that persist.
  - *io_uring by Example* (Jens Axboe) and the `liburing` documentation — the authoritative guide.

## Where This Fits in Phase 2

This replaces the storage-engine block in the [Phase 2](./02-wo-language.md) component list. Specifically:

| Phase 2 component | Becomes (with in-memory design) |
| --- | --- |
| Relational pages + buffer pool | RAM-resident B+ tree / Masstree, no page eviction |
| Document engine (LSM on disk) | LSM memtable in RAM; sealed memtables spilled to SSD only for checkpointing |
| Graph engine (disk adjacency) | RAM adjacency arena; checkpointed, not paged |
| WAL | `io_uring` + `O_DIRECT` append-only log on NVMe |
| Checkpointer | Periodic snapshot of RAM arenas to SSD for fast recovery |
| Buffer pool | **Removed** — all data is in RAM |
| Vacuum | Still needed, but for MVCC chain pruning, not for reclaiming disk pages |

The cross-paradigm transaction coordinator sketched in Phase 2 stays the same — it just drives in-memory engines instead of disk-paged ones, and the WAL append it depends on is the one `io_uring` path.

Net effect: **shorter read paths, identical durability story, same ACID guarantees**, at the cost of a hard dataset ceiling set by RAM.
