# SurrealDB — Runtime Case Study

Reference repository: [github.com/surrealdb/surrealdb](https://github.com/surrealdb/surrealdb)

```bash
git submodule add https://github.com/surrealdb/surrealdb.git references/surrealdb
```

## The Question

Does SurrealDB only rely on async/await for concurrency?

**No.** SurrealDB uses a layered concurrency model — async/await is one layer, but it also uses OS thread pools, CPU-affinity-pinned workers, lock-free data structures, and parallel computation frameworks. Each layer serves a different purpose.

## Architecture Overview

SurrealDB is a single Rust binary that ships a multi-model database (documents, graphs, key-value) with real-time live queries. It supports multiple deployment modes:

- **Server**: `surreal start` runs HTTP/WebSocket API via Axum + storage engine
- **Embedded**: the library crate embeds directly in Rust applications
- **WASM**: runs in the browser with IndexedDB backend

## Concurrency Layers

### Layer 1: Tokio — Async I/O and Request Handling

The primary runtime. Handles:
- HTTP/WebSocket connections (via Axum)
- Network I/O (accept, read, write)
- Timer-based operations
- Task scheduling (M:N scheduling of futures onto OS threads)

```
Client request → Axum handler (async) → parse query → execute → respond
```

Every request handler is an async function. Tokio's multi-threaded executor distributes tasks across OS threads using work-stealing.

### Layer 2: Rayon — Parallel CPU-Bound Computation

For operations that are compute-heavy, not I/O-bound:
- Query plan execution across partitions
- Data processing and transformation
- Parallel iteration over result sets

Rayon provides `par_iter()` — automatic parallelism across CPU cores. It has its own thread pool, separate from tokio's.

### Layer 3: affinitypool — CPU-Pinned Storage I/O

SurrealDB's custom crate. Runs blocking storage operations on a dedicated thread pool where **each thread is pinned to a specific CPU core** via `libc` CPU affinity syscalls.

Used by:
- RocksDB backend (blocking disk I/O)
- SurrealKV embedded storage
- In-memory engine for heavy operations

This bridges the async world (tokio) and the blocking world (disk I/O) without polluting the tokio thread pool with blocking calls.

### Layer 4: Lock-Free Data Structures

The hot path in storage engines uses concurrent data structures that avoid locks entirely:

| Crate | Data Structure | Used For |
|-------|---------------|----------|
| `crossbeam-skiplist` | Concurrent skip list | Index structures in SurrealKV and surrealmx |
| `crossbeam-deque` | Work-stealing deque | Task distribution |
| `crossbeam-queue` | Lock-free queue | Message passing |
| `papaya` | Concurrent HashMap | In-memory engine (surrealmx) |
| `dashmap` | Sharded concurrent map | Pub/sub routing for live queries |
| `arc-swap` | Atomic pointer swap | Hot-swapping data structures without locks |
| `parking_lot` | Fast mutex/rwlock | Where locking is needed (faster than std) |

## No Fibers

SurrealDB does **not** use fibers, green threads, or any custom scheduling mechanism. The concurrency model is:

```
Tokio tasks (async/await)      — for I/O-bound work
Rayon threads (par_iter)       — for CPU-bound work
affinitypool threads (pinned)  — for blocking storage I/O
Lock-free structures           — for concurrent data access
```

This is pragmatic — each concurrency mechanism is used where it fits, rather than forcing everything through one model.

## Live Queries / Real-Time Subscriptions

SurrealDB's live query system pushes changes to connected clients in real-time:

1. Client registers a live query via WebSocket: `LIVE SELECT * FROM person WHERE age > 21`
2. Server tracks the query in a `dashmap` (concurrent map)
3. When a transaction commits changes to `person`, the engine evaluates which live queries are affected
4. Matching subscribers receive the diff via their WebSocket connection
5. Transport: `tokio-tungstenite` for WebSocket, `async-channel` for internal pub/sub routing

### Comparison with writeonce Subscriptions

| Aspect | SurrealDB | writeonce |
|--------|-----------|-----------|
| Transport | WebSocket (tokio-tungstenite) | Raw socket fd (kernel-level write) |
| Query registration | SQL-like live query over WebSocket | `register!` macro binding fd to content pattern |
| Change detection | Transaction commit triggers evaluation | inotify detects file change |
| Notification routing | `dashmap` + `async-channel` | `SubscriptionManager` HashMap + direct `write(fd)` |
| Runtime | Tokio multi-threaded executor | Single-threaded epoll event loop |
| Protocol framing | WebSocket frames | Length-prefixed payloads (no protocol) |

SurrealDB's live queries are the architectural inspiration for writeonce's subscription model (as noted in [03-data.md](../03-data.md)), but the implementation is fundamentally different — SurrealDB uses a full async runtime with WebSocket transport, while writeonce uses kernel fd notifications with no protocol layer.

## Storage Engine Architecture

SurrealDB supports 5 backends:

| Backend | Type | Concurrency |
|---------|------|------------|
| **surrealmx** | In-memory | Lock-free (papaya, crossbeam-skiplist, arc-swap) |
| **surrealkv** | Embedded persistent | Tokio async + crossbeam + parking_lot |
| **RocksDB** | Embedded persistent | affinitypool (CPU-pinned blocking threads) |
| **TiKV** | Distributed | Async TiKV client over gRPC |
| **IndxDB** | Browser/WASM | IndexedDB via wasm-bindgen-futures |

### Comparison with writeonce Storage

| Aspect | SurrealDB | writeonce |
|--------|-----------|-----------|
| Storage format | Key-value entries in LSM trees (RocksDB) or custom B-trees (SurrealKV) | `.seg` files with length-prefixed bincode records |
| Index | Built into storage engine | Separate `.idx` files (title hash, date sorted, tags inverted) |
| Concurrency | Multi-threaded with locks/lock-free structures | Single-threaded, positional I/O (pread/pwrite) |
| Transaction | ACID with MVCC | Full rebuild on change (article count is small) |
| Complexity | ~100K+ lines across storage crates | ~300 lines (wo-seg + wo-index) |

writeonce's storage is intentionally simple — the dataset is small (hundreds of articles, not millions of rows), so a full rebuild on change is fast enough and avoids the complexity of concurrent transactions.

## Key Takeaways

1. **Async/await alone is not enough for a database.** SurrealDB uses four concurrency mechanisms, each for a different workload profile.

2. **Blocking I/O needs its own thread pool.** The affinitypool pattern — CPU-pinned threads for storage operations — keeps blocking work off the async executor. writeonce avoids this entirely by using `pread` (non-blocking positional reads) in a single-threaded loop.

3. **Lock-free data structures matter at scale.** SurrealDB's hot path avoids mutexes. writeonce doesn't need this — single-threaded access means no contention.

4. **Live queries are the hard problem.** Both SurrealDB and writeonce solve "push changes to subscribers," but at vastly different scales. SurrealDB handles arbitrary SQL predicates over millions of rows. writeonce handles content queries over hundreds of articles.

5. **The right amount of complexity depends on the problem.** SurrealDB is a general-purpose database — it needs the complexity. writeonce is a content platform — the event loop model is sufficient and far simpler.

## Reference

Add SurrealDB as a submodule for code reference:

```bash
git submodule add https://github.com/surrealdb/surrealdb.git references/surrealdb
```

Key files to study:
- `crates/core/src/kvs/` — storage engine abstraction and transaction handling
- `crates/sdk/src/api/engine/` — live query subscription routing
- `lib/affinitypool/` — CPU-pinned thread pool for blocking I/O
- `crates/core/src/sql/` — query parser and execution engine
