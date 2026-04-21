# 08 — `mmap` + `madvise`

Memory-map a file (or anonymous region) into the process's address space. The kernel manages the page cache; your code sees a `&[u8]` slice. `madvise` hints the kernel about access patterns so it can pre-fetch sequentially, evict aggressively after scans, or map huge pages.

Central to Phase 3's storage engine: segment files are `mmap`ed read-only for O(1)/O(log n) indexed lookups without copying bytes into heap memory.

## Kernel source

| Path | What |
| --- | --- |
| [`reference/linux/mm/mmap.c`](../../../reference/linux/mm/mmap.c) | VMA creation, `SYSCALL_DEFINE6(mmap, ...)`, `SYSCALL_DEFINE2(munmap, ...)`. |
| [`reference/linux/mm/madvise.c`](../../../reference/linux/mm/madvise.c) | `SYSCALL_DEFINE3(madvise, ...)` + every `MADV_*` handler. |
| [`reference/linux/include/uapi/linux/mman.h`](../../../reference/linux/include/uapi/linux/mman.h) | `MAP_*` flags, huge-page sizing macros. |
| POSIX `<sys/mman.h>` | The other half of the constants (`PROT_*`, `MADV_*`). Usually folded into `linux/mman.h` by libc. |

## Man pages

`man 2 mmap`, `man 2 madvise`, `man 2 munmap`, `man 2 msync`, `man 2 mprotect`.

## Rust FFI via `libc`

```rust
use libc::{mmap, munmap, madvise, msync, mprotect};
use libc::{PROT_READ, PROT_WRITE, PROT_NONE, PROT_EXEC};
use libc::{MAP_SHARED, MAP_PRIVATE, MAP_ANONYMOUS, MAP_FIXED};
use libc::{MAP_POPULATE, MAP_HUGETLB, MAP_HUGE_2MB, MAP_HUGE_1GB};
use libc::{MADV_SEQUENTIAL, MADV_RANDOM, MADV_WILLNEED, MADV_DONTNEED};
use libc::{MADV_HUGEPAGE, MADV_NOHUGEPAGE, MS_SYNC, MS_ASYNC};
```

## Direct-syscall example

```rust
unsafe {
    // 1. Map a segment file read-only. Use MAP_POPULATE to pre-fault all pages
    //    so lookups don't hit a minor page fault mid-request.
    let fd  = libc::open(path.as_ptr(), libc::O_RDONLY);
    let len = libc::lseek(fd, 0, libc::SEEK_END) as usize;

    let ptr = libc::mmap(
        std::ptr::null_mut(),
        len,
        libc::PROT_READ,
        libc::MAP_SHARED | libc::MAP_POPULATE,
        fd,
        0,
    );
    if ptr == libc::MAP_FAILED {
        return Err(io::Error::last_os_error());
    }

    // 2. Hint access pattern — sequential scan for a full compaction pass,
    //    random for indexed lookups. MADV_DONTNEED after a scan releases page cache pressure.
    libc::madvise(ptr, len, libc::MADV_RANDOM);

    // 3. Use it as a byte slice
    let slice: &[u8] = std::slice::from_raw_parts(ptr as *const u8, len);
    let record = &slice[offset..offset + record_len];

    // 4. Clean up
    libc::munmap(ptr, len);
    libc::close(fd);
}
```

## Key flags

| Flag | Meaning |
| --- | --- |
| `PROT_READ` / `PROT_WRITE` | Obvious. Combine as needed. `PROT_NONE` makes a guard page. |
| `MAP_SHARED` | Writes go back to the file. Required for write-through semantics (WAL staging into a `mmap`ed region). |
| `MAP_PRIVATE` | Copy-on-write. Writes never hit the file. Use for read-only segments where you want CoW safety. |
| `MAP_POPULATE` | Pre-fault the whole mapping at `mmap` time. Trades boot latency for zero-fault request path. **Use it for hot segments.** |
| `MAP_HUGETLB` / `MAP_HUGE_2MB` | Back with huge pages. 512× fewer TLB entries for a 64 GB arena. Requires `vm.nr_hugepages` configured. |
| `MAP_ANONYMOUS` | Not file-backed — just zero-initialised pages. Used for arenas the engine allocates internally. |
| `MAP_FIXED` | Place at the exact requested address. Dangerous — will silently overwrite existing mappings. Only when you know what you're doing (e.g. placing guard pages). |

| `madvise` | Meaning |
| --- | --- |
| `MADV_SEQUENTIAL` | "I'll read sequentially." Kernel prefetches ahead, drops pages behind. Full scans, compaction. |
| `MADV_RANDOM` | "Lookups will be random." Kernel disables read-ahead. Index lookups. |
| `MADV_WILLNEED` | "Bring these pages in now." Async prefetch for an upcoming working set. |
| `MADV_DONTNEED` | "I'm done; drop these pages." Frees page-cache slots immediately — good after a scan to avoid polluting the cache. |
| `MADV_HUGEPAGE` | Opt this range into Transparent Huge Pages. |

## Gotchas

- **Shared writable mappings and `fsync`.** Writes to a `MAP_SHARED` region are *not* durable until you `msync(MS_SYNC)` or `fsync` the underlying fd. For write paths that need durability, prefer explicit `pwrite` — don't rely on `msync` for the hot path.
- **`SIGBUS` on truncated files.** If the file shrinks beneath your mapping, accesses past the new end raise `SIGBUS`. Arrange for sealed files (`memfd_create(MFD_ALLOW_SEALING)` + `F_SEAL_SHRINK`) or just don't truncate.
- **Page faults block the single thread.** In a single-threaded runtime, a minor fault during a request freezes the whole loop. `MAP_POPULATE` at boot sidesteps this for hot data. Use `mlockall(MCL_CURRENT \| MCL_FUTURE)` if faults must never happen — but that requires `CAP_IPC_LOCK` or `RLIMIT_MEMLOCK` headroom.
- **`madvise` hints are advice, not commands.** The kernel may ignore them under memory pressure. Don't rely on them for correctness; only for perf.
- **Huge pages need config.** `vm.nr_hugepages` has to have enough entries for your arenas. Startup-time check, not request-time.

## Used by

Phase 3 of the database series — see [`docs/runtime/database/03-inmemory-engine.md`](../runtime/database/03-inmemory-engine.md) § Linux Tuning Checklist. The relational B+ tree, the LSM memtables' on-disk segments, and the document store's arenas all live behind `mmap`.

## v1 port source

**None directly** — v1's wo-seg reads with `pread`, not `mmap`. This is new code territory for the new storage engine.
