# Storage manager — relation files on disk

`storage/smgr/{smgr.c,md.c}` is the layer between "this is relation X" and "this is a set of OS files." It maps each relation to a sequence of **segment files** on disk, capped at `RELSEG_SIZE` (default 1 GiB) — a relation that grows past 1 GiB rolls over into `<oid>.1`, `<oid>.2`, …. Reads and writes go through `smgrread` / `smgrwrite` / `smgrextend`, which delegate to the magnetic-disk implementation in `md.c`. There's a per-relation cache (`SMgrRelation`) so the OS file descriptors persist across many calls.

The writeonce equivalent is **per-type segment files** (`data/<TypeName>.seg`). Same shape, simpler: only one storage manager (no kerb-style abstractions for shared/local/whatever), no segment number — single file per type until a type's data grows past a configured cap.

## Postgres source

| File | Responsibility |
| --- | --- |
| [`storage/smgr/smgr.c`](../../../../reference/postgresql/src/backend/storage/smgr/smgr.c) | Front-door API. `smgropen`, `smgrread`, `smgrwrite`, `smgrextend`, `smgrdounlink`. Holds the `SMgrRelation` cache. |
| [`storage/smgr/md.c`](../../../../reference/postgresql/src/backend/storage/smgr/md.c) | The actual implementation against the kernel. Manages `MdfdVec` (open file descriptor handles per segment number), opens missing segments lazily. |
| [`storage/smgr/bulk_write.c`](../../../../reference/postgresql/src/backend/storage/smgr/bulk_write.c) | Optimized path for bulk-loading: writes directly to `smgrwrite` without going through shared buffers. Useful for `COPY` / `CREATE INDEX` + the recovery path's wal-replay-rebuilds-pages flow. |
| [`storage/smgr/README`](../../../../reference/postgresql/src/backend/storage/smgr/README) | Brief but worth reading — explains the relfilenode → file naming convention and how `RELSEG_SIZE` interacts with 32-bit-fs-size historical limits. |

## What `md.c` actually does

For a request to read block B of relation R, `md.c`:

1. Looks up R's `MdfdVec`. Each entry holds `(segment_number, fd)`.
2. Computes `segment_number = B / RELSEG_SIZE_BLOCKS`, `block_within_segment = B % RELSEG_SIZE_BLOCKS`.
3. If the fd for that segment isn't cached, `open(2)` the file (`<oid>.<seg>`).
4. `pread(fd, buf, BLCKSZ, block_within_segment * BLCKSZ)` — single positional read, no shared cursor.

For an `smgrextend(R, B)` (grow):

1. If we're crossing a segment boundary, `open(O_CREAT)` the next segment file.
2. `pwrite(fd, zero_buf, BLCKSZ, block_within_segment * BLCKSZ)` — extend the file by writing a zero page at the new offset. (Postgres preallocates pages explicitly because some filesystems sparsely allocate when extending without writing — `md.c` wants the page allocated *now*.)
3. Optionally `posix_fallocate` the segment up front instead.

`md.c` is also where `mdsyncfiletag` lives — Postgres' deferred-fsync mechanism. The checkpointer hands `md.c` "the list of files that need to be `fsync`'d before this checkpoint completes," `md.c` deduplicates and issues the syscalls.

## What writeonce keeps

1. **One file per relation/type.** `data/Article.seg`, `data/Comment.seg`. The Postgres-style `relfilenode` numbering is overkill — type names are stable and globally unique within a `wo run` directory.
2. **Segment rollover when files get big.** Postgres caps at 1 GiB. Writeonce inherits a sensible cap (start at 1 GiB; no observable difference until then). Beyond the cap: `<TypeName>.1.seg`, `.2.seg`, …
3. **Lazy fd open + fd caching.** First write to a type opens the file; the fd lives in an `HashMap<TypeName, RawFd>` for the lifetime of the engine. Closed on drop. No per-tick syscall churn.
4. **`pwrite` for positional writes; `pread` for positional reads.** No `lseek` cursor, so concurrent reads are safe. Pairs naturally with [`io_uring`](../linux/07-io_uring.md) when we want batched I/O.
5. **`posix_fallocate` to reserve space at segment creation.** Avoids `ENOSPC` mid-write and minimizes filesystem-level fragmentation. See [`linux/09-fallocate.md`](../linux/09-fallocate.md).

## What writeonce drops

- **Multi-fork relations.** Postgres has `main`, `fsm` (free-space map), `vm` (visibility map) forks per relation, each its own file. Writeonce starts with one fork per type — secondary indexes get their own files (`<TypeName>.<col>.idx`) when phase 12 needs them, not as a forks-of-the-same-relation abstraction.
- **`RelFileNode` indirection.** Postgres runs `(database_oid, tablespace_oid, relation_oid)` through a layer that resolves to a path. Writeonce names files by type directly. Layered indirection is something we add when (if) we ever support multi-database.
- **Tablespaces.** The whole concept of "relation X lives in tablespace Y which is a directory at path P" is a multi-tenant ops feature. Writeonce binds to one data dir per `wo run` invocation.
- **`storage/large_object/`.** TOAST + LOB. JSON values bigger than ~8 KiB pages get sliced into pieces stored in a `pg_toast_<oid>` table. Writeonce records aren't page-bound until phase 12+ adds page-level layout, and even then large records can stay in the segment as one variable-length entry.

## Hot reads in `md.c`

The single function worth porting in spirit is `mdread` / `mdwrite`. The Postgres versions are ~80 LOC each; the writeonce versions collapse to ~30 once you strip the `BLCKSZ`/segment-arithmetic + the multi-fork abstraction. The recipe:

```rust
fn read(&self, ty: &str, offset: u64, len: usize) -> io::Result<Vec<u8>> {
    let fd = self.fd(ty)?;            // opens lazily, caches
    let mut buf = vec![0u8; len];
    let n = unsafe {
        libc::pread(fd, buf.as_mut_ptr() as *mut _, len, offset as i64)
    };
    if n < 0 { return Err(io::Error::last_os_error()); }
    buf.truncate(n as usize);
    Ok(buf)
}

fn append(&self, ty: &str, bytes: &[u8]) -> io::Result<u64> {
    let fd = self.fd(ty)?;
    let offset = self.tail(ty)?;      // tracked in-memory by SegStore
    let n = unsafe {
        libc::pwrite(fd, bytes.as_ptr() as *const _, bytes.len(), offset as i64)
    };
    if n < 0 { return Err(io::Error::last_os_error()); }
    self.advance_tail(ty, n as u64);
    Ok(offset)
}
```

That's the core of phase 10's `SegStore`.

## Used by

[`docs/plan/10-storage-foundations.md`](../../10-storage-foundations.md) — segment file layout, append path. Pair with [`linux/09-fallocate.md`](../linux/09-fallocate.md) for preallocation, [`linux/12-pwrite-fsync.md`](../linux/12-pwrite-fsync.md) for the syscall details.
