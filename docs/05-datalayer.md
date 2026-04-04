# Data Layer — Implementation Status

The embedded data layer described in [02-recovery.md](./02-recovery.md) and [03-data.md](./03-data.md) has been implemented as a Cargo workspace with 8 crates. All 44 tests pass. No external database, no AWS, no tokio — direct Linux syscalls on a custom event loop.

## Workspace Structure

```
writeonce-all/
  Cargo.toml                    # workspace root
  docs/                         # architecture documentation
  sample-content/               # 5 test articles for validation
  crates/
    wo-model/                   # content model
    wo-seg/                     # .seg file format
    wo-index/                   # index files
    wo-store/                   # unified storage engine
    wo-watch/                   # inotify file watcher
    wo-event/                   # epoll event loop
    wo-sub/                     # subscription system
    wo-rt/                      # custom runtime
```

## Crate Summary

| Crate | Purpose | Tests | Key Types |
|-------|---------|-------|-----------|
| **wo-model** | Article structs matching existing JSON schema, `ContentLoader` for directory walking | 8 | `Article`, `ArticleContent`, `ArticleBody`, `Section`, `CodeSnippet`, `ContentLoader` |
| **wo-seg** | Binary `.seg` file format — length-prefixed records, tombstoning, positional I/O | 6 | `SegWriter`, `SegReader`, `SegHeader` |
| **wo-index** | Three index types for O(1) and O(log n) access patterns | 8 | `TitleIndex`, `DateIndex`, `TagIndex` |
| **wo-store** | Unified storage engine composing seg + indexes, cold-start rebuild | 3 | `Store` |
| **wo-watch** | Content directory watcher using inotify | 4 | `ContentWatcher`, `ContentChange` |
| **wo-event** | Custom event loop on epoll with eventfd, timerfd, signalfd | 5 | `EventLoop`, `EventFd`, `TimerFd`, `SignalFd` |
| **wo-sub** | Subscription manager with fd-based notifications, `register!` macro | 6 | `SubscriptionManager`, `Subscription`, `Notification` |
| **wo-rt** | Runtime tying all crates together — single process, single event loop | 4 | `Runtime`, `RuntimeHandle`, `Config` |

## Linux Kernel Syscalls Used

| Syscall | Crate | Purpose |
|---------|-------|---------|
| `pread` / `pwrite` | wo-seg | Positional read/write for .seg records without seeking |
| `fallocate` | wo-seg | Pre-allocate .seg file space to reduce fragmentation |
| `epoll_create1` / `epoll_ctl` / `epoll_wait` | wo-event | Event-driven I/O multiplexing for the main loop |
| `eventfd` | wo-event, wo-sub | Lightweight signaling between watcher and subscription manager |
| `timerfd_create` / `timerfd_settime` | wo-event | Periodic tasks (compaction, keepalive) as file descriptors |
| `signalfd` | wo-event | SIGINT/SIGTERM delivered as fd events for graceful shutdown |
| `inotify_init1` / `inotify_add_watch` | wo-watch | File system change detection on the content directory |
| `pipe2` | wo-sub (tests) | Mock subscriber fds for testing notification delivery |

## .seg File Format

```
Offset  Size     Field
0       4        Magic: b"WOSF"
4       2        Version: u16 LE (1)
6       2        Flags: u16 LE (reserved)
8       8        Record count: u64 LE
16      8        Data start offset: u64 LE
24      8        Reserved
32+     variable Records: [u32 length][u8 flags][bincode payload]...
```

- Records are addressed by byte offset from file start
- Flags: `0x00` = active, `0x01` = tombstoned
- Payload: bincode-serialized `Article` struct

## Index Files

| File | Format | Access Pattern |
|------|--------|----------------|
| `title.idx` | On-disk hash table (Robin Hood, load factor 0.5), 138 bytes/slot | O(1) lookup by `sys_title` |
| `date.idx` | Sorted `(i64 timestamp, u64 offset)` array, 16 bytes/entry | Binary search for date ranges, latest N |
| `tags.idx` | Bincode-serialized `HashMap<String, Vec<u64>>` | Tag-to-offsets inverted index |

All indexes are derived from `.seg` and rebuildable from `content/` on cold start.

## Subscription Model

No SSE. No WebSocket. Notifications are written directly to subscriber file descriptors.

- **Subscribe**: `SubscriptionManager::subscribe(fd, Subscription::ByTitle("linux-misc"))`
- **Notify**: on content change, length-prefixed `Notification` written to matching fds
- **Cleanup**: `EPOLLHUP` on epoll triggers automatic `unsubscribe(fd)`
- **Dedup**: if a fd matches multiple patterns (title + tag), it receives only one notification

Subscription patterns:
- `Subscription::ByTitle(sys_title)` — single article
- `Subscription::ByTag(tag)` — all articles with tag
- `Subscription::All` — all content changes

## Store Query API

```rust
store.get_by_title("linux-misc")        -> Option<Article>
store.list_published(skip, limit)       -> Vec<Article>
store.list_by_tag("rust")               -> Vec<Article>
store.list_by_date_range(start, end)    -> Vec<Article>
store.count_published()                 -> usize
store.article_version("linux-misc")     -> Option<u64>
store.rebuild()                         // full rebuild from content/
```

## Runtime Event Loop

Single `epoll` instance multiplexing all file descriptors:

| Token | Fd | Handler |
|-------|----|---------|
| `WATCHER` | inotify fd | Process file changes → update store → notify subscribers |
| `SIGNAL` | signalfd | SIGINT/SIGTERM → graceful shutdown |
| `TIMER` | timerfd | Periodic tasks (compaction, stats) |
| `NOTIFY` | eventfd | Subscription notification signal |
| `1000+` | subscriber fds | Hangup detection → unsubscribe + cleanup |

## External Dependencies

| Crate | Version | Purpose |
|-------|---------|---------|
| `serde` | 1.x | Serialization derives |
| `serde_json` | 1.x | JSON parsing for article files |
| `bincode` | 1.x | Compact binary serialization for .seg records and notifications |
| `libc` | 0.2.x | Raw Linux syscall bindings |

No tokio. No async-std. No database driver. No HTTP framework (yet).

## What Comes Next

The data layer delivers everything the HTTP server and UI layers need:

1. **`Store` with zero-copy query access** — all article queries resolve in-process
2. **Subscription system accepting raw fds** — HTTP layer hands socket fds to `subscribe()`
3. **Shared event loop** — HTTP listener socket registers on the same epoll
4. **Automatic cold-start** — if `data/` is missing, rebuilds from `content/` on startup
5. **Graceful shutdown** — SIGTERM triggers clean fd cleanup

Next phases per [02-recovery.md](./02-recovery.md):
- **HTTP server** — route handlers using the `Store` query API, embedded in the same binary
- **HTMLX templates** — server-rendered HTML with `{{bindings}}` per [04-ui.md](./04-ui.md)
- **Frontend collapse** — serve static assets from the binary, eliminate the Angular app
3