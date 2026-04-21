## Linux Kernel Features

Kernel primitives that the writeonce binary can leverage, mapped to the architectural needs identified in [docs/01-problem.md](../../01-problem.md) and [docs/02-recovery.md](../../02-recovery.md).

### Per-primitive reference cards

Each primitive has its own numbered file with the kernel source path (into [`reference/linux/`](../../../reference/linux/)), Rust FFI signature via `libc`, a minimal direct-syscall example, and the v1 port source. Use these when implementing the phase docs under [`docs/plan/`](../).

| # | Primitive | Used by |
| --- | --- | --- |
| [01](./01-epoll.md)         | `epoll` — event-driven I/O multiplexing | every runtime phase |
| [02](./02-eventfd.md)       | `eventfd` — counter as fd, cross-flow wake | phase 02, subscription wakeup |
| [03](./03-timerfd.md)       | `timerfd` — timers as fds | phase 02, phase 07 debounce |
| [04](./04-signalfd.md)      | `signalfd` — signals as fds, graceful shutdown | phase 04 |
| [05](./05-inotify.md)       | `inotify` — filesystem events as fds | phase 07, future register! subscription |
| [06](./06-sendfile.md)      | `sendfile` — zero-copy file → socket | phase 08 |
| [07](./07-io_uring.md)      | `io_uring` — async I/O ring buffers | phase 3 (WAL fsync), future HTTP |
| [08](./08-mmap.md)          | `mmap` + `madvise` — memory-mapped files, page-cache hints | phase 3 (storage engine) |
| [09](./09-fallocate.md)     | `fallocate` + `pread` + `pwritev2` — positional I/O & pre-allocation | phase 3 (WAL + SSTables) |
| [10](./10-pidfd.md)         | `pidfd` — process as fd, race-free supervision | future supervisor |
| [11](./11-memfd_create.md)  | `memfd_create` — anonymous shared memory | phase 3 (index build) |

The list below is the original overview kept for context and for a handful of adjacent primitives (`fanotify`, `splice`/`tee`) that don't yet have their own reference card.

### File Watching — Content Directory

| Syscall    | Purpose                                                                                                                                                                                            |
| ---------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `inotify`  | Watch the content directory for file creates, modifications, and deletes. Triggers re-indexing and subscriber notification when articles change. Replaces the S3 + Lambda event pipeline entirely. |
| `fanotify` | Alternative to inotify with broader scope (filesystem-level events). Useful if watching needs to span mount points or require permission-based filtering.                                          |

### Async I/O — Server and Subscription Manager

| Syscall    | Purpose                                                                                                                                                                                                                               |
| ---------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `epoll`    | Event-driven I/O multiplexing for the HTTP server and SSE connections. Handles many concurrent subscriber connections without a thread per client. Foundation for the async runtime.                                                  |
| `io_uring` | Modern async I/O interface (Linux 5.1+). Supports batched, zero-copy submission of read/write/accept operations. Candidate for the embedded storage engine's disk reads and the HTTP server's socket handling in a single event loop. |

### Efficient File Serving — Content Delivery

| Syscall          | Purpose                                                                                                                                                                 |
| ---------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `sendfile`       | Zero-copy transfer from file descriptor to socket. Serves markdown files and static frontend assets directly from disk to the client without copying through userspace. |
| `splice` / `tee` | Zero-copy data transfer between file descriptors via kernel pipe buffers. Useful for streaming .seg file reads directly to HTTP responses.                              |

### Embedded Storage — .seg Files and Indexing

| Syscall              | Purpose                                                                                                                                                                                                               |
| -------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `mmap`               | Memory-map .seg index files for O(1)/O(log n) lookups by `blog-title` without loading entire files into heap memory. Keeps the storage engine's memory footprint proportional to working set, not total content size. |
| `madvise`            | Hint to the kernel about mmap access patterns (`MADV_SEQUENTIAL` for full scans, `MADV_RANDOM` for index lookups). Improves page cache behavior for both date-ordered listing and title-based retrieval.              |
| `fallocate`          | Pre-allocate disk space for .seg files and append-only logs. Prevents fragmentation and ensures writes don't fail mid-operation due to disk pressure.                                                                 |
| `pread` / `pwritev2` | Positional read/write without seeking. Allows concurrent reads from different offsets in the same .seg file without locking a shared file offset. Pairs well with io_uring for batched operations.                    |

### Event Notification — Subscription Manager

| Syscall   | Purpose                                                                                                                                                                                         |
| --------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `eventfd` | Lightweight signaling between the file watcher thread and the subscription manager. When inotify detects a content change, eventfd wakes the async event loop to push diffs to SSE subscribers. |
| `timerfd` | Timer as a file descriptor. Can drive periodic tasks (index compaction, subscriber keepalive pings) within the same epoll/io_uring event loop without a separate timer thread.                  |

### Process and Resource Management

| Syscall        | Purpose                                                                                                                                               |
| -------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------- |
| `pidfd`        | File descriptor for the process itself. Enables clean self-monitoring and graceful shutdown signaling within the single-binary model.                 |
| `memfd_create` | Anonymous memory-backed file descriptors. Useful for transient storage (e.g., building a .seg index in memory before atomically swapping it to disk). |

## Runtime

- Develop async runtime environment built on the kernel primitives above.
- Core event loop: `epoll` or `io_uring` driving all I/O (file watch, HTTP, SSE, storage reads).
- Single-threaded event loop with thread pool for blocking operations (disk I/O, index rebuilds).
- All file descriptors (inotify, eventfd, timerfd, sockets) registered on one multiplexer — one process, one loop, no external dependencies.

## Database Subscription

No SSE. No WebSocket. Subscriptions are handled at the kernel level using file descriptors and the event loop that already drives the rest of the system.

### Why Not SSE or WebSocket

Both are HTTP-layer protocols that add framing, connection management, and protocol negotiation on top of what the kernel already provides. If the binary already owns the event loop (`epoll` / `io_uring`), the content directory (`inotify`), and the subscriber sockets (raw fds), there is no reason to layer another protocol on top. The kernel *is* the subscription engine.

### Register Macro

A subscription is a registration of interest in a content query, bound to a file descriptor. The `register` macro wires a query pattern to the event loop at compile time:

```rust
register! {
    #{blog-title} => notify(fd)
}
```

This expands to:

1. **Parse the query pattern** — `#{blog-title}` resolves to a specific article's `sys_title` key in the .seg index.
2. **Bind to inotify watch** — The content file backing that `blog-title` gets an inotify watch descriptor. When the file is modified, inotify fires.
3. **Map watch → subscriber fd** — The event loop maps the inotify event to every file descriptor that registered interest in that `blog-title`.
4. **Write to fd** — The subscriber's socket fd receives the diff (or a notification payload) via a direct `write()` — no HTTP framing, no SSE `data:` lines, no WebSocket frames.

### Flow

```
  inotify (content file changed)
       |
       v
  epoll wakes → lookup blog-title in subscription table
       |
       v
  for each registered fd:
       write(fd, diff_payload)
```

### Subscription Table

The subscription table is an in-process map:

```
blog-title  →  Vec<RawFd>
```

- **Register**: client connects → fd is added to the vec for the requested `blog-title`.
- **Deregister**: client disconnects → fd is removed (detected via `EPOLLHUP` / `EPOLLRDHUP`).
- **Notify**: inotify fires for a content file → resolve `blog-title` from path → write to all registered fds.

No broker, no message queue, no protocol layer. The kernel's fd lifecycle (`epoll` for readiness, `inotify` for content changes, `close` for cleanup) handles the full subscription lifecycle.

### Query Patterns

The `register` macro supports patterns beyond single titles:

```rust
register! {
    #{blog-title}                  // single article by sys_title
    #[tag="rust"]                  // all articles matching a tag
    #[published > "2026-01-01"]    // date-range filter
    #*                             // all content changes
}
```

Each pattern resolves to a set of inotify watch descriptors. When the watched set changes (new article added that matches the filter), the subscription table updates automatically during re-indexing.

## Related: the assembly policy

Every primitive above is reached via `libc::<syscall>` or `libc::syscall(SYS_*, ...)` — no custom assembly. The reasoning lives in [`../assembly/`](../assembly/) — three files covering why runtimes use asm at all ([`00-overview.md`](../assembly/00-overview.md)), what Go's [`reference/go/src/runtime/*.s`](../../../reference/go/src/runtime/) actually contains ([`01-go-runtime-asm.md`](../assembly/01-go-runtime-asm.md)), and the writeonce policy that all of it is replaced by Rust stdlib + libc ([`02-writeonce-stance.md`](../assembly/02-writeonce-stance.md)).
