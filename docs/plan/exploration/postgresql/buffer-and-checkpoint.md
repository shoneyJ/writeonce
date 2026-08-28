# Buffer cache & checkpoint

`storage/buffer/bufmgr.c` is Postgres' page cache: pages live in shared memory, are pinned/unpinned by readers, and are written back to disk lazily. `postmaster/checkpointer.c` is the dedicated process that periodically flushes all dirty pages, then advances the **redo pointer** in the control file — a guarantee that "everything before LSN X is on disk; recovery can start from X."

Writeonce takes the same two ideas, single-thread:

- A **clean/dirty bit per page** kept in-process. Page reads and writes hit the OS page cache directly via `pread` / `pwrite` (writeonce does not maintain its own user-space buffer pool; the kernel page cache is good enough for the workload size we target).
- A **checkpoint routine** that runs as a periodic step in the loop. It walks dirty pages, issues `fsync` on the affected files, and rewrites the control file's `last_durable_lsn`.

No separate process. No shared-buffer pinning. No dynamic-shared-memory coordination.

## Postgres source

| File | Responsibility |
| --- | --- |
| [`storage/buffer/bufmgr.c`](../../../../.dev/reference/postgresql/src/backend/storage/buffer/bufmgr.c) | Page cache front-door: `ReadBuffer`, `BufferGetPage`, `MarkBufferDirty`, `FlushBuffer`. Tracks dirty bit per buffer; pinning prevents eviction. |
| [`storage/buffer/freelist.c`](../../../../.dev/reference/postgresql/src/backend/storage/buffer/freelist.c) | Clock-sweep eviction policy. Buffers with `usage_count = 0` and `pin_count = 0` are eviction candidates; usage decremented on every sweep pass, incremented on access. |
| [`storage/buffer/buf_table.c`](../../../../.dev/reference/postgresql/src/backend/storage/buffer/buf_table.c) | Hash table from `(file, block)` → buffer slot. The lookup that `ReadBuffer` does. |
| [`postmaster/checkpointer.c`](../../../../.dev/reference/postgresql/src/backend/postmaster/checkpointer.c) | The checkpointer process. Triggered by time (`checkpoint_timeout`), WAL volume (`max_wal_size`), or signal. Runs `BufferSync()` to flush dirty buffers, then `CreateCheckPoint()` to update the control file. |
| [`postmaster/bgwriter.c`](../../../../.dev/reference/postgresql/src/backend/postmaster/bgwriter.c) | Continuously trickles dirty pages to disk between checkpoints. Smooths the I/O burst the checkpointer would cause. |
| [`storage/buffer/README`](../../../../.dev/reference/postgresql/src/backend/storage/buffer/README) | Overview of the pinning, locking, and replacement policy. Worth reading. |

## The page-cache idea worth porting

A buffer in Postgres is a `(file_id, block_number, page_data, dirty_bit, pin_count, usage_count, content_lock, io_lock)`. Strip out everything that exists for multi-process coordination (`pin_count`, locks) and you get the per-block state you need in any persistent store: **the bytes, where they came from on disk, and whether they're dirty since last fsync.**

Writeonce's phase 12 `Engine` keeps an `HashMap<(TypeName, SegmentOffset), CachedRow>` where `CachedRow = { bytes: Vec<u8>, dirty: bool }`. Rows are read on-demand (cache miss → `pread` + decode + CRC verify), written through to the segment but not flushed to disk until the next commit's WAL fsync covers them. Dirty rows accumulate; a periodic checkpoint flushes the segment fds and advances the control-file LSN.

The kernel page cache does most of the work. `pread` against an fd that already has its page cached is a memcpy. `pwrite` populates the page cache without going to disk until pressure or `fsync`. This is why writeonce explicitly does NOT use `O_DIRECT` (see [`linux/12-pwrite-fsync.md`](../linux/12-pwrite-fsync.md)) — the page cache is the one cache we want.

## Checkpoint — the writeonce shape
> **⚠ TWO CORRECTIONS, 2026-08-28** (found while brainstorming
> [databasev2 3](../../../stories/databasev2/03-wal-checkpoint.md); spec:
> [`2026-08-28-wal-checkpoint-design.md`](../../../superpowers/specs/2026-08-28-wal-checkpoint-design.md)).
>
> 1. **Postgres does NOT update its control file by rename.** The claim below
>    that "Postgres does the same in `BasicOpenFile` + `fsync_parent_path`" is
>    wrong: `update_controlfile` (`src/common/controldata_utils.c`) opens the
>    existing file `O_WRONLY`, writes a zero-padded **full block in place**, and
>    relies on **CRC32C** over the struct to detect a torn write. The
>    `fsync(parent_dir)` reasoning below is still correct *for renames* — it is
>    just not what Postgres does here.
> 2. **The checkpoint sketch below assumes writeonce has segment files.** It
>    says records before the LSN are "*known* to be in the segment files". There
>    are none: the WAL is writeonce's only durable form, replayed into RAM, and
>    [databasev2 2](../../../stories/databasev2/02-table-storage-modes.md)
>    deliberately rejected adding a paged store. This document predates the
>    databasev2 direction, so read the loop below as a design for an
>    architecture that was not chosen.
>
> What survived the comparison is the **ordering discipline**, not the
> architecture: publish the new "recovery starts here" atomically and last, so a
> crash falls back. writeonce gets that from one `rename` of the whole log —
> possible only because its records are full row images, where Postgres' are
> page deltas.


Postgres' checkpoint runs in a separate process and signals the postmaster when done. Writeonce's runs as a periodic loop step:

```text
loop tick (every CHECKPOINT_INTERVAL, e.g. 60s):
    fsync(every active segment fd)        // metadata + data barrier
    fsync(wal_dir_fd)                     // ensure recent WAL writes are visible
    write control.tmp { last_durable_lsn = current_wal_tail }
    fsync(control.tmp)
    rename(control.tmp, control)
    fsync(data_dir_fd)                    // make the rename durable
```

The `rename(2)` is atomic on POSIX-compliant filesystems — at any crash point, either `control.tmp` is missing (the rename hasn't happened) or `control` reflects the new content. `fsync(parent_dir)` is needed because rename's atomicity is in-kernel; the directory entry isn't durable until its parent inode is synced. (Postgres does the same in `BasicOpenFile` + `fsync_parent_path`.)

Recovery on startup reads `control`, finds the `last_durable_lsn`, and replays WAL forward from there. Records before that LSN are *known* to be in the segment files; records after are replayed.

## What writeonce skips

- **Pinning + content locks.** Single-thread loop has one reader and one writer of the cache: itself. No need for `LockBuffer(BUFFER_LOCK_SHARE)` etc.
- **`bgwriter` continuous trickle.** Postgres has a *separate* process slowly cleaning the buffer pool to avoid I/O spikes at checkpoint. Writeonce's checkpoints are infrequent enough (60s default) that a spike is fine; if it becomes a problem, the same loop can do "soft flush K pages per tick" without spawning anything.
- **Hash partitioning of the buffer table.** Postgres partitions `buf_table` to reduce lock contention — single-thread doesn't have lock contention.
- **`shared_buffers` GUC.** Postgres lets the operator size the buffer pool. Writeonce trusts the OS page cache and bounds its in-process cache by an LRU with a simple count limit (`WO_CACHE_ROWS=10000` default, configurable).

## Where to look in the Postgres source

For the page cache:
- `BufferAlloc()` in `bufmgr.c` — read the function header. Strip the locks and you've got the cache-miss path.
- `BufferSync()` in `bufmgr.c` — read the prologue. The dirty-buffer-walk + per-relation fsync coalescing is the checkpoint algorithm.

For the checkpoint:
- `CreateCheckPoint()` in `xlog.c` — the control-file update sequence. Read the comments around `WriteControlFile` and the surrounding `pg_fsync`s. That's the rename-on-write pattern in practice.
- The `checkpointer.c` main loop is short and worth scanning for the time-vs-WAL-volume trigger logic.

## Used by

- `docs/plan/12-engine-disk-cutover.md` (Rust-era, removed 2026-08-18) — disk-backed engine reads and dirty-row tracking.
- `docs/plan/11-wal-and-recovery.md` (Rust-era, removed 2026-08-18) — control file write sequence (phase 11 ships the control file; checkpoint as a periodic step lands with phase 12 or shortly after).

Pair with [`linux/12-pwrite-fsync.md`](../linux/12-pwrite-fsync.md) for the fsync semantics and [`linux/08-mmap.md`](../linux/08-mmap.md) for the OS page-cache backstory.
