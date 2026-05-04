# 12 — `pwrite` + `fsync` durability syscalls

The previous cards cover positional I/O ([`09-fallocate.md`](./09-fallocate.md)) and the page cache backstory ([`08-mmap.md`](./08-mmap.md)) but skip the actual durability primitives. This card fills the gap. Every persistent-storage phase (10, 11, 12) leans on these.

## The four syscalls

| # | Syscall | What it guarantees | When to use |
| --- | --- | --- | --- |
| 1 | `pwrite(2)` / `pwritev2(2)` | Bytes are in the OS page cache at the given offset. NOT on disk. | Every write — the durability barrier is `fsync`, not `write`. |
| 2 | `fdatasync(2)` | Bytes for this fd are on durable storage; metadata except size NOT guaranteed. | WAL commits — we don't read inode timestamps for replay, so metadata is irrelevant. |
| 3 | `fsync(2)` | Bytes + all inode metadata for this fd are on durable storage. | After file rename / extension where the metadata change matters (control file, segment rollover, parent directory). |
| 4 | `sync_file_range(2)` | Linux-specific async hint — start I/O on a byte range without committing the metadata. | Optional optimization — kick off WAL writeback early, then `fdatasync` at commit. |

## Postgres uses all four

| Postgres call | Wraps | Where |
| --- | --- | --- |
| `pg_pwrite()` | `pwrite64` | [`storage/file/fd.c`](../../../../reference/postgresql/src/backend/storage/file/fd.c) — every block-aligned write. |
| `pg_fsync()` | `fsync` (or platform variant) | [`storage/file/fd.c`](../../../../reference/postgresql/src/backend/storage/file/fd.c) — wraps `wal_sync_method` GUC dispatch. |
| `pg_fdatasync()` | `fdatasync` | Same. Selected when `wal_sync_method = fdatasync`. |
| Async writeback | `sync_file_range` | [`access/transam/xlog.c`](../../../../reference/postgresql/src/backend/access/transam/xlog.c) — `issue_xlog_fsync` calls `sync_file_range(SYNC_FILE_RANGE_WRITE)` to start I/O on the WAL ahead of the durability barrier. |

The Postgres GUC matrix (`wal_sync_method`) lets the operator pick between `fsync`, `fdatasync`, `open_sync`, `open_datasync`, `fsync_writethrough`. **Writeonce picks one** — `fdatasync` for the WAL, `fsync` for control files and segment rollovers — and ships it.

## Kernel source

| Path | What |
| --- | --- |
| [`reference/linux/fs/read_write.c`](../../../reference/linux/fs/read_write.c) | `SYSCALL_DEFINE4(pread64, ...)`, `SYSCALL_DEFINE4(pwrite64, ...)`, `SYSCALL_DEFINE6(pwritev2, ...)`. |
| [`reference/linux/fs/sync.c`](../../../reference/linux/fs/sync.c) | `SYSCALL_DEFINE1(fsync, ...)`, `SYSCALL_DEFINE1(fdatasync, ...)`, `SYSCALL_DEFINE4(sync_file_range, ...)`. |
| [`reference/linux/include/uapi/asm-generic/fcntl.h`](../../../reference/linux/include/uapi/asm-generic/fcntl.h) | `O_SYNC`, `O_DSYNC`, `O_DIRECT`. |
| [`reference/linux/Documentation/filesystems/ext4/journal.rst`](../../../reference/linux/Documentation/filesystems/ext4/journal.rst) | What ext4's journal commits when `fsync` runs. Worth understanding what the kernel actually does on the durability path. |

## Man pages

`man 2 pwrite`, `man 2 pwritev2`, `man 2 fsync`, `man 2 fdatasync`, `man 2 sync_file_range`, `man 2 posix_fadvise`.

## Rust FFI via `libc`

```rust
use libc::{
    pwrite, pwritev2,
    fsync, fdatasync,
    sync_file_range,
    posix_fadvise,
    iovec, off_t,
    SYNC_FILE_RANGE_WRITE, SYNC_FILE_RANGE_WAIT_AFTER, SYNC_FILE_RANGE_WAIT_BEFORE,
    POSIX_FADV_DONTNEED, POSIX_FADV_RANDOM, POSIX_FADV_SEQUENTIAL,
    RWF_DSYNC, RWF_SYNC,
};
```

## Direct-syscall example — WAL group commit

```rust
// 1. Append a record to the WAL via pwrite — bytes land in the page cache.
let n = unsafe {
    libc::pwrite(wal_fd, rec.as_ptr() as *const _, rec.len(), wal_tail as i64)
};
if n < 0 { return Err(io::Error::last_os_error()); }

// 2. Group-commit fence: drain all pending commits this tick, then ONE barrier.
//    fdatasync is enough — the WAL file is fixed-size (fallocated), no metadata change
//    to commit. Saves a journal-update on ext4.
if unsafe { libc::fdatasync(wal_fd) } < 0 {
    return Err(io::Error::last_os_error());
}

// 3. Now ack each pending commit.
for fd in commits.drain(..) { send_response(fd, /* 200 OK */); }
```

## Direct-syscall example — control file rename-on-write

```rust
// 1. Write the new control file content to a sibling temp file.
let tmp = data_dir.join("control.tmp");
let fd = unsafe { libc::open(tmp.as_ptr(), libc::O_WRONLY | libc::O_CREAT | libc::O_TRUNC, 0o644) };
let _ = unsafe { libc::pwrite(fd, ctl.as_ptr() as *const _, ctl.len(), 0) };

// 2. fsync (NOT fdatasync) — the file's size + inode metadata must be durable
//    before the rename, otherwise the rename could publish a zero-byte control
//    file after a crash.
if unsafe { libc::fsync(fd) } < 0 { return Err(...); }
unsafe { libc::close(fd); }

// 3. rename(2) is atomic on POSIX-compliant filesystems.
unsafe { libc::rename(tmp.as_ptr(), final_path.as_ptr()); }

// 4. fsync the parent directory so the rename's directory-entry update is durable.
let dfd = unsafe { libc::open(data_dir.as_ptr(), libc::O_RDONLY) };
unsafe { libc::fsync(dfd); libc::close(dfd); }
```

## `fsync` vs `fdatasync` — when each matters

`fsync` flushes:
- All dirty page-cache pages backing the file.
- The inode metadata: size, atime/mtime/ctime, link count.

`fdatasync` flushes:
- All dirty pages.
- The inode metadata **only if it would change a future read** — i.e. file size if the write extended the file.

Why this matters for writeonce:

- **WAL writes**: the file is `posix_fallocate`'d up front to its full segment size, so writes never extend it. `fdatasync` is enough. Saves the metadata journal update, which on ext4 is roughly half the cost of an `fsync`.
- **Control file**: `fsync` because we follow with a `rename(2)` whose effect we want to be durably visible — and renames involve directory inode metadata.
- **Segment rollover** (creating a new `.seg` file or extending past the fallocate'd size): `fsync` once at rollover, `fdatasync` for steady-state writes.

## `sync_file_range` — when the optimization is worth it

`sync_file_range(fd, offset, length, SYNC_FILE_RANGE_WRITE)` starts I/O on a byte range without waiting. Use case: between WAL appends and the commit fence, kick the kernel to start writing the just-appended bytes. By the time `fdatasync` runs, much of the work is already in flight, so the latency of the barrier shrinks.

Postgres uses this in `issue_xlog_fsync`. Writeonce skips it in phase 11 — the loop tick boundary is short enough (microseconds in the steady state) that the bytes are still in the page cache and the kernel writeback hasn't kicked in. Revisit if benchmarks show fsync latency dominating.

## `posix_fadvise` — drop pages from the cache after writes

`posix_fadvise(fd, offset, len, POSIX_FADV_DONTNEED)` tells the kernel "I won't read these bytes again soon" — the kernel can reclaim the page cache for them. Useful after **bulk inserts** (`COPY` in Postgres, future `wo bulk-load` in writeonce) when you don't want the page cache to evict useful working-set pages to make room for cold inserts.

Not needed in phase 10/11. Possibly useful later for archival writes.

## Why writeonce does NOT use `O_DIRECT`

`O_DIRECT` bypasses the page cache: writes go straight to the block device, reads come straight from it. Saves a memcpy at the cost of:

- **No read-ahead.** Every read is a disk seek.
- **No write coalescing.** Two adjacent `pwrite`s of the same page hit the device twice.
- **Strict alignment.** Buffer pointers, offsets, and lengths must be multiples of the device's logical block size.
- **Recovery cost.** With `O_DIRECT`, the page cache is dark. Recovery has to physically read every WAL byte. With buffered I/O, the recently-written WAL is already in the page cache — replay is a memcpy.

Writeonce trades the memcpy for the recovery speed and the simpler programming model. If a future phase identifies a workload where the memcpy is the bottleneck (it almost never is), `O_DIRECT` can be added per-fd.

## Gotchas

- **`fsync` returning success doesn't always mean the bytes are durable.** Some consumer SSDs lie about cache-flush completion. Postgres has documented this extensively (`wal_sync_method = open_datasync` was added partly as a workaround). For writeonce on a real server-class SSD, `fdatasync` + `O_DSYNC` give correct semantics; on flaky hardware, no software can compensate.
- **`fdatasync` is enough only because we fallocated.** If a future phase appends to a non-fallocated file, the size change requires `fsync` to commit the new size to the inode — otherwise a crash recovery sees the old size and our written bytes appear missing.
- **`pwrite`'s atomicity is limited.** On regular files, writes ≤ `PIPE_BUF` (4 KiB) are atomic with respect to other writes. Larger writes can be torn — another reader interleaved with the writer might see half the new bytes. The single-thread loop has no other writers, so this is moot, but it's worth knowing if a future phase introduces a second writer.
- **Don't `close` then `fsync`**. The fsync is a no-op against a closed fd. If you must close, fsync first.
- **`rename` requires a parent-directory fsync to be durable across a crash.** A common bug — Postgres' `fsync_parent_path` exists exactly for this.

## Used by

- [`docs/plan/10-storage-foundations.md`](../../10-storage-foundations.md) — segment append uses `pwrite` + later `fdatasync`.
- [`docs/plan/11-wal-and-recovery.md`](../../11-wal-and-recovery.md) — group-commit uses `pwrite` + `fdatasync`; control file uses `fsync` + `rename` + parent-dir `fsync`.
- [`docs/plan/12-engine-disk-cutover.md`](../../12-engine-disk-cutover.md) — checkpoint uses `fsync` per active segment fd.

Pair with [`postgresql/wal.md`](../postgresql/wal.md), [`postgresql/buffer-and-checkpoint.md`](../postgresql/buffer-and-checkpoint.md) for the design context.

## v1 port source

**Partial.** `reference/crates/wo-seg/src/writer.rs:92` calls `file.sync_all()` (Rust stdlib's `fsync` wrapper). Phase 11 replaces with explicit `libc::fdatasync` for the WAL path; segment files keep `fsync` semantics for rollover events.
