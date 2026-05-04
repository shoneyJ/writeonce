# 09 — Scale-out: thread-per-core for 10k concurrent users

**Context sources:** [`./08-sendfile-static-assets.md`](./08-sendfile-static-assets.md) (last single-threaded phase), [`./assembly/02-writeonce-stance.md`](./assembly/02-writeonce-stance.md) (the "single-threaded" policy we're now refining), [`../runtime/database/02-wo-language.md#concurrency-model`](../runtime/database/02-wo-language.md#concurrency-model) (original concurrency stance), [`docs/examples/ecommerce/`](../examples/ecommerce/) (the target workload), [`./linux/`](./linux/) (kernel primitives), [`reference/go/src/runtime/`](../../reference/go/src/runtime/) (precedent for a runtime that scales across threads).

## Context

Phases 02–08 produce a single-threaded event-loop runtime with zero external Rust dependencies — good enough for the blog sample and the first 500k ops/sec on one core. **The ecommerce workload at `docs/examples/ecommerce/` pushes past that ceiling**: ~10,000 connected websocket subscribers watching `/api/orders/live`, ~1,000 checkouts per second during peak, and every commit fanning out delta frames to a sizeable subset of the connected clients. No single core survives that, regardless of how tight the event loop is.

This phase is the refinement of the Phase-2 concurrency doctrine "shard to scale past one core" into a concrete architecture. The stance stays the same — **no Go-style goroutines, no work-stealing across threads, no shared mutable heap** — but now we have multiple event loops, each owning its core, its share of connections, and its slice of engine state.

This doc is a **master plan**. It outlines sub-phases 10–15 at a high level; each sub-phase lands as its own numbered plan doc when implementation starts. No code changes in this pass.

## Goal

Serve the ecommerce sample at 10,000 concurrent websocket subscribers + 1,000 checkouts/second on a single 8–16 core box, with per-request tail latencies (`p99`) inside 50 ms for reads and 100 ms for commits. After this phase sequence lands, `cargo run --bin wo -- run docs/examples/ecommerce` can handle production-shaped load with only the process count as the horizontal scale knob.

## Design decisions (locked)

1. **Thread-per-core, not M:N.** `N` OS threads pinned to `N` cores via `sched_setaffinity(cpu_set_t)`. Each thread runs its own event loop (the [phase-02 `runtime/` module](./02-event-loop-epoll.md)) plus a local shard of engine state. Pinned for the thread's lifetime; a connection accepted on thread K stays on thread K forever. Precedent: Seastar / ScyllaDB / Redis Cluster.
2. **Shared-nothing state.** No cross-thread mutable access to the catalog, engine rows, or subscription registry. Communication is message-passing over single-producer-single-consumer ring buffers (crossbeam-style, built on `std::sync::atomic`, per [`./assembly/02-writeonce-stance.md`](./assembly/02-writeonce-stance.md) — still no asm). If thread A needs to touch data owned by thread B, it sends a message; B processes it on its own tick.
3. **SO_REUSEPORT for listener-side load balancing.** Every thread binds a socket with `SO_REUSEPORT` on the same `:8080` — the kernel distributes incoming SYNs across the `N` listener sockets with consistent hashing on the connection 4-tuple. No user-space accept-thread bottleneck. Linux ≥ 3.9 is fine; ≥ 4.5 adds `BPF` filters for custom routing if we ever need session-affinity.
4. **Per-thread io_uring ring.** Each thread gets its own `io_uring_setup` ring with `IORING_SETUP_SINGLE_ISSUER` + `IORING_SETUP_SQPOLL` ([per `./linux/07-io_uring.md`](./linux/07-io_uring.md)). No ring sharing across threads — simpler ordering, no contention.
5. **Shard key: customer id (modulo N).** The ecommerce schema is customer-centric — one customer's orders + purchase edges + cart live on the same shard. Cross-customer queries (admin `list orders`) fan out; same-customer operations (checkout) are local. Blog shard key would be `author.id` for the same reason.
6. **Cross-shard transactions via 2PC.** A checkout that updates inventory on shard A and customer balance on shard B uses two-phase commit between the two engine threads. Phase 4's transaction coordinator (from [`../runtime/database/02-wo-language.md`](../runtime/database/02-wo-language.md) § Cross-Paradigm Transaction Coordinator) already handles this pattern for sql+doc+graph inside one process; it generalises cleanly to cross-thread.
7. **No Go-style goroutines.** Connections are not tasks that migrate. Each connection's state machine runs on its owning thread's event loop, just as it does in the single-threaded model — the difference is there are now `N` event loops running concurrently.

## What we copy from Go, what we don't

Read [`reference/go/src/runtime/netpoll_epoll.go`](../../reference/go/src/runtime/netpoll_epoll.go) and [`reference/go/src/runtime/proc.go`](../../reference/go/src/runtime/proc.go) for the shape; copy the **ideas** about fd-to-loop mapping and atomic-counter-based wake-up. Do **not** copy:

| Go feature | Why writeonce skips it |
| --- | --- |
| Goroutines (M:N scheduling, work stealing) | Goroutines pay context-switch + GC-scan costs the thread-per-core model avoids. Scylla benchmarks consistently beat Go-style runtimes at the same hardware. |
| Shared heap + GC | No heap GC — Rust ownership. Data is partitioned across threads, not shared with locks. |
| `gogo` / `mcall` / `systemstack` asm | No scheduler-controlled stack switching. See [`./assembly/02-writeonce-stance.md`](./assembly/02-writeonce-stance.md). |
| `cgo` boundary | Rust is the only language. `libc` is already ABI-compatible via `extern "C"`. |
| `asyncPreempt` preemption | Handlers run to completion on their owning thread. Back-pressure comes from bounded per-thread queues, not preemption. |

And what we **do** copy:

| Go pattern | Writeonce translation |
| --- | --- |
| Per-P netpoller (the `pp.pollDesc` model) | Per-thread `EventLoop` (the phase-02 `runtime::EventLoop`) |
| `netpollBreak` (fd wake-up via sendto) | Per-thread `eventfd` — one fd per thread, write to it to wake a sleeping `epoll_wait`. See [`./linux/02-eventfd.md`](./linux/02-eventfd.md). |
| `findrunnable` (what to do when idle) | Per-thread idle-state: drain in-process message queues, run compaction, run periodic timers (from [`./linux/03-timerfd.md`](./linux/03-timerfd.md)). |
| `runtime.GOMAXPROCS` | `WO_THREADS` env var (defaults to `std::thread::available_parallelism()`). |

## Linux primitives this phase leans on (beyond the phase-02/03/08 set)

Reference cards already exist for most; this phase adds the ones that are cross-thread-specific:

| Primitive | Use | Reference |
| --- | --- | --- |
| `SO_REUSEPORT` | N listener sockets on the same port; kernel load-balances accepts | [`reference/linux/net/core/sock_reuseport.c`](../../reference/linux/net/core/sock_reuseport.c) — worth adding `linux/12-so-reuseport.md` |
| `sched_setaffinity` + `cpu_set_t` | Pin each thread to its core | [`reference/linux/kernel/sched/core.c`](../../reference/linux/kernel/sched/core.c) |
| `futex(2)` | Fallback cross-thread wait if per-thread eventfd wake-up isn't enough | [`reference/linux/kernel/futex/`](../../reference/linux/kernel/futex/) — worth `linux/13-futex.md` |
| `membarrier(2)` | Process-wide memory barrier when a rebalance migrates state between threads | [`reference/linux/kernel/sched/membarrier.c`](../../reference/linux/kernel/sched/membarrier.c) |
| `io_uring` with `IORING_SETUP_SINGLE_ISSUER` | One ring per thread, pinned | [`./linux/07-io_uring.md`](./linux/07-io_uring.md) |
| `eventfd` per thread | Cross-thread wake-up — thread A writes to thread B's eventfd to deliver a message | [`./linux/02-eventfd.md`](./linux/02-eventfd.md) |
| `mmap(MAP_HUGETLB)` | Per-thread arena allocator backed by 2 MB pages for cache locality | [`./linux/08-mmap.md`](./linux/08-mmap.md) |

## Sub-phase sequence

Each one lands as its own numbered plan doc when ready for implementation. Smoke test (`cargo run --bin wo -- run docs/examples/ecommerce` serves correctly) stays green after every sub-phase.

### `10-thread-per-core.md` — N event loops, `SO_REUSEPORT`

Introduce a thread-pool manager at `crates/rt/src/runtime/scheduler.rs` (Go parallel: `proc.go`). Spawn `WO_THREADS` OS threads at boot; each pins itself and runs an `EventLoop`. Replace the single `Listener` with per-thread listeners bound `SO_REUSEPORT` to the same port. State is still global at first (shared `Arc<Mutex<Engine>>`) — one thing at a time. Exit criterion: `wo run` boots N threads visible in `ps -T`, accepts load balanced across them per `ss -tnp`, no regression in the 20-assertion blog smoke.

### `11-sharded-engine.md` — per-thread engine state

Partition the in-memory engine catalog + row BTreeMaps by shard id (= thread id). Shard key is `customer.id` for ecommerce / `author.id` for blog / per-type default for anything else. Add a shard router in front of every REST/WS handler: resolve the shard from the request's identifying field, send an in-process message to that thread's mailbox, await response. Shared `Arc<Mutex<Engine>>` goes away; each thread owns its slice. Cross-shard reads (admin `list orders`) fan out to every thread and merge results.

### `12-per-shard-wal.md` — one WAL file per shard

Each thread has its own `foo.wal` + `foo.data` + per-thread `io_uring` ring (phase 3's durability work, repeated per shard). Recovery is parallel across threads. No shared WAL writer thread. Group commit is per-thread.

### `13-cross-shard-subscriptions.md` — LIVE fanout

A commit on shard K that creates/updates rows of type T needs to wake subscribers on every shard watching T. Via broadcast: K writes the delta to a per-subscriber-thread mailbox — one message per destination thread, not per subscriber. The destination thread then does the fine-grained predicate match against its local subscription table. Avoids N² traffic when N connections watch the same stream.

### `14-cross-shard-txn.md` — 2PC for transactions that span shards

`fn checkout(customer, product, qty)` might touch shards A (customer), B (product), and C (order) if they hash differently. The transaction coordinator (already designed in [`../runtime/database/02-wo-language.md`](../runtime/database/02-wo-language.md) § Cross-Paradigm Transaction Coordinator) generalises to cross-shard: `begin(snapshot_ts)` broadcasts to all participating shards, `prepare()` collects votes, `commit(wal_lsn)` atomically flips markers, `abort()` if any participant refuses. The per-shard WAL entries carry the 2PC state machine.

### `15-observability-and-rebalance.md` — ops

Per-shard metrics (connections, ops/s, p99, WAL lag), Prometheus scrape endpoint on one well-known thread. A `WO_RESHARD` admin command migrates a contiguous customer-id range from shard K to shard K′ via state snapshot → replay → cutover. For a fixed-core deployment this is rare; matters when `WO_THREADS` changes between runs.

## Verification targets (after 15 lands)

Ecommerce sample on an 8-core box with `WO_THREADS=8`:

| Metric | Target | How measured |
| --- | --- | --- |
| Concurrent WS subscribers | **10,000** | `websocat` fan-out against `/api/orders/live` + persistent count |
| Checkout throughput | **1,000/s** sustained | Load driver fires `POST /api/fn/checkout` with per-customer key distribution |
| Read p99 | **< 50 ms** | `GET /api/orders?customer=X` under 10k-subscriber background load |
| Commit p99 | **< 100 ms** | Measured from `POST /api/fn/checkout` acceptance to HTTP ack |
| Memory steady-state | **< 2 GB RSS** | 10k connections × 2 KB/conn + engine working set |
| Dep count | **1** (`libc`) | `crates/rt/Cargo.toml` still has only libc after all this |
| `wo run docs/examples/blog` | still boots and serves | phase 02–08 regression test, unchanged |

## Non-scope

- **No Go-style goroutines, even after this phase.** Adding M:N scheduling is not on the roadmap. When one core runs out, add more cores (more threads) — horizontally, thread-per-core.
- **No distributed (multi-node) sharding.** This phase is single-box only. Redis-Cluster-style network sharding is a separate future phase; the in-process shard bus (phase 10's mailboxes) is not the same thing as a cluster membership protocol.
- **No dynamic thread count at runtime.** `WO_THREADS` is set at boot and pinned. Adding/removing a thread means a rolling restart. Acceptable for a database; fundamental to the zero-contention model.
- **No work-stealing.** A slow handler on thread A does not get rebalanced to thread B. Back-pressure is the thread-local queue filling up. If one thread hot-spots because of a bad shard key, the fix is to reshard — not to steal.
- **No `std::thread::available_parallelism` on exotic hosts.** `WO_THREADS` override covers kubernetes CFS-bound pods, NUMA partitioning, and single-core debug runs.
- **No new external Rust dependencies.** Same stance as phases 02–08 — `libc` only. Message passing, atomics, affinity, futex — all through libc or `std::sync::atomic`.

## Escape hatch

If the "single core per process, shard across processes" argument ([Redis Cluster model](../runtime/database/02-wo-language.md#concurrency-model)) turns out to be more operationally attractive than a single multi-threaded process, **every decision in this plan translates**. Per-thread shards become per-process shards; `SO_REUSEPORT` inside the kernel becomes a reverse proxy in front; in-process mailboxes become Unix domain sockets. The phase-02 event loop is the reusable atom regardless.

## Cross-references

- [`./08-sendfile-static-assets.md`](./08-sendfile-static-assets.md) — last prerequisite phase; feature-complete single-threaded runtime.
- [`./assembly/02-writeonce-stance.md`](./assembly/02-writeonce-stance.md) — updated to reference this phase's thread-per-core model; still no asm.
- [`../runtime/database/02-wo-language.md#concurrency-model`](../runtime/database/02-wo-language.md#concurrency-model) — the stance this plan refines.
- [`reference/go/src/runtime/proc.go`](../../reference/go/src/runtime/proc.go) — Go's scheduler, for contrast.
- [`reference/go/src/runtime/netpoll_epoll.go`](../../reference/go/src/runtime/netpoll_epoll.go) — per-P netpoller, the idea we borrow.
- [`reference/linux/net/core/sock_reuseport.c`](../../reference/linux/net/core/sock_reuseport.c) — kernel load balancer.
- [`reference/linux/kernel/sched/core.c`](../../reference/linux/kernel/sched/core.c) — affinity syscalls.
