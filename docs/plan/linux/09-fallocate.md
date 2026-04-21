# 09 — `fallocate` + positional I/O (`pread`, `pwritev2`)

`fallocate` pre-allocates disk space for a file without writing any bytes — lets the filesystem commit to a contiguous extent, so later writes don't fragment and can't fail mid-operation due to disk pressure. `pread` / `pwritev2` read and write at an explicit offset without touching the file's cursor — letting many concurrent readers share one fd safely.

Together they form the backbone of the storage engine's on-disk layout: segment files are pre-allocated to their target size at creation, then written into via `pwritev2`; readers hit them via `pread` or `mmap` (see [08-mmap.md](./08-mmap.md)).

## Kernel source

| Path | What |
| --- | --- |
| [`reference/linux/fs/open.c`](../../../reference/linux/fs/open.c) | `SYSCALL_DEFINE4(fallocate, ...)`. The syscall delegates to `file->f_op->fallocate` — per-filesystem. |
| [`reference/linux/fs/read_write.c`](../../../reference/linux/fs/read_write.c) | `SYSCALL_DEFINE4(pread64, ...)`, `SYSCALL_DEFINE4(pwrite64, ...)`, `SYSCALL_DEFINE6(pwritev2, ...)`. |
| [`reference/linux/include/uapi/linux/falloc.h`](../../../reference/linux/include/uapi/linux/falloc.h) | `FALLOC_FL_*` flags. |

## Man pages

`man 2 fallocate`, `man 2 pread`, `man 2 pwrite`, `man 2 pwritev2`.

## Rust FFI via `libc`

```rust
use libc::{fallocate, pread, pread64, pwrite, pwrite64, pwritev2, iovec, off_t};
use libc::{FALLOC_FL_KEEP_SIZE, FALLOC_FL_PUNCH_HOLE, FALLOC_FL_ZERO_RANGE,
           FALLOC_FL_COLLAPSE_RANGE, FALLOC_FL_INSERT_RANGE};
// pwritev2 has its own flags:
use libc::{RWF_SYNC, RWF_DSYNC, RWF_HIPRI, RWF_NOWAIT, RWF_APPEND};
```

## Direct-syscall example

```rust
unsafe {
    let fd = libc::open(path.as_ptr(), libc::O_RDWR | libc::O_CREAT, 0o644);

    // 1. Pre-allocate 64 MB so writes can't fail with ENOSPC later.
    //    Omit FALLOC_FL_KEEP_SIZE to make the size reflect the allocation
    //    (common for WAL rings); include it to reserve space without growing
    //    the file's apparent size (common for LSM SSTables before finalization).
    if libc::fallocate(fd, 0, 0, 64 * 1024 * 1024) < 0 {
        return Err(io::Error::last_os_error());
    }

    // 2. Positional write from a scattered set of buffers — no shared cursor,
    //    no extra copy to concat. pwritev2 also accepts per-call flags like
    //    RWF_SYNC for integrity barriers on a specific write.
    let iovs = [
        iovec { iov_base: header.as_ptr() as *mut _, iov_len: header.len() },
        iovec { iov_base: body.as_ptr()   as *mut _, iov_len: body.len()   },
    ];
    let offset: off_t = 4096;
    let written = libc::pwritev2(fd, iovs.as_ptr(), iovs.len() as i32, offset, libc::RWF_DSYNC);

    // 3. Concurrent readers hit the same fd with pread — no locking needed,
    //    no interference with the writer's implicit cursor (there isn't one).
    let mut buf = vec![0u8; 8192];
    let n = libc::pread(fd, buf.as_mut_ptr() as *mut _, buf.len(), record_offset as off_t);
}
```

## Key flags

### `fallocate` modes (first arg after fd)

| Flag (bitwise-OR into `mode`) | Meaning |
| --- | --- |
| `0` (default) | Allocate and extend the file if offset+len > size. WAL growth. |
| `FALLOC_FL_KEEP_SIZE` | Allocate without changing the reported file size. SSTables-in-progress. |
| `FALLOC_FL_PUNCH_HOLE` (+ `KEEP_SIZE`) | Release blocks in a range. Sparse-file compaction. |
| `FALLOC_FL_ZERO_RANGE` | Zero a byte range efficiently (filesystem marks it unwritten). Faster than `pwrite(zeros)` for segment reset. |
| `FALLOC_FL_COLLAPSE_RANGE` / `FALLOC_FL_INSERT_RANGE` | Move extents — remove or create holes without re-writing. Log compaction. Requires filesystem support (ext4 / xfs). |

### `pwritev2` flags (6th arg)

| Flag | Meaning |
| --- | --- |
| `RWF_SYNC` | `O_SYNC` semantics for this call only — data + metadata barrier. |
| `RWF_DSYNC` | `O_DSYNC` semantics — data barrier, metadata not guaranteed. **WAL commits.** |
| `RWF_HIPRI` | Best-effort high priority; polls for completion on NVMe. Pair with `IOPOLL` rings. |
| `RWF_NOWAIT` | Return `EAGAIN` rather than blocking if the kernel would sleep. Useful for async paths. |
| `RWF_APPEND` | Equivalent to `O_APPEND` for this call, even if the fd wasn't opened with it. |

## Gotchas

- **`fallocate` is per-filesystem.** ext4 and xfs support every flag above; tmpfs supports `0` but not `PUNCH_HOLE`; network filesystems may silently no-op. Check `statfs(2) / f_type` at startup if cross-fs portability matters — or just require ext4/xfs.
- **`pread`/`pwrite` don't update the fd's file offset.** Great for concurrent readers. If you have code that alternates seek+read, don't mix it with `pread`-based readers on the same fd — it'll work but the mental model gets confusing.
- **`pwritev2` is Linux 4.6+.** Older kernels need `pwritev` + `fdatasync`. Not a concern for the runtime's target kernel (5.1+ for `io_uring` anyway).
- **`RWF_DSYNC` ≠ fsync.** It's a per-call data barrier. If you've opened with `O_DIRECT`, pages bypass the cache and the barrier is cheap. Otherwise still cheaper than a full `fsync` because only this call's metadata barrier is enforced.
- **Filesystem ENOSPC is silent in `fallocate` on some fs**. It can return 0 then fail at first write. Test against your target filesystem; don't assume the guarantee.

## Used by

Phase 3 of the database series — WAL pre-allocation, SSTable extent reservation, segment punching for compaction. Also [`07-io_uring.md`](./07-io_uring.md) pairs beautifully with positional I/O: `IORING_OP_WRITE` / `IORING_OP_READ` take an offset, so they're `pwrite`/`pread` under the hood.

## v1 port source

**None.** V1's wo-seg writes sequentially with `write` + `sync_all`; no pre-allocation. New territory for the v2 storage engine.
