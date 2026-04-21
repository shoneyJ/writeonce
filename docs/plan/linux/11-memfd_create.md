# 11 — `memfd_create`

Anonymous memory-backed file descriptor. `memfd_create(name, flags)` returns an fd that points at a region of RAM (tmpfs-like) with no filesystem path. Size it with `ftruncate`, fill with writes or `mmap`, pass the fd across processes for zero-copy IPC, or use it as a scratchpad that vanishes on close. With `MFD_ALLOW_SEALING`, you can freeze the mapping so consumers can safely `mmap` it without worrying about shrinks or truncations.

Useful for the storage engine's transient work: building an index in memory before atomically renaming into place, staging a large response body that needs to be `sendfile`'d, or sharing a read-only snapshot with a child process.

## Kernel source

| Path | What |
| --- | --- |
| [`reference/linux/mm/memfd.c`](../../../reference/linux/mm/memfd.c) | `SYSCALL_DEFINE2(memfd_create, ...)` + seal ops. |
| [`reference/linux/include/uapi/linux/memfd.h`](../../../reference/linux/include/uapi/linux/memfd.h) | `MFD_*` flags. |
| [`reference/linux/include/uapi/linux/fcntl.h`](../../../reference/linux/include/uapi/linux/fcntl.h) | `F_ADD_SEALS`, `F_GET_SEALS`, `F_SEAL_*` constants. Sealing is a `fcntl(F_ADD_SEALS, ...)` operation on the memfd. |

## Man pages

`man 2 memfd_create`, `man 2 fcntl` (for the seal operations).

## Rust FFI via `libc`

```rust
use libc::{memfd_create, ftruncate, mmap, munmap};
use libc::{MFD_CLOEXEC, MFD_ALLOW_SEALING, MFD_HUGETLB, MFD_NOEXEC_SEAL};
use libc::{F_ADD_SEALS, F_GET_SEALS};
use libc::{F_SEAL_SEAL, F_SEAL_SHRINK, F_SEAL_GROW, F_SEAL_WRITE, F_SEAL_FUTURE_WRITE};
```

## Direct-syscall example

```rust
unsafe {
    // 1. Create the anonymous fd. The name is for /proc/self/fd listings; not a path.
    let name = std::ffi::CString::new("wo-index-build").unwrap();
    let fd = libc::memfd_create(name.as_ptr(), libc::MFD_CLOEXEC | libc::MFD_ALLOW_SEALING);
    if fd < 0 { return Err(io::Error::last_os_error()); }

    // 2. Size it, then write into it (or mmap and write directly).
    libc::ftruncate(fd, 4 * 1024 * 1024);    // 4 MiB
    let ptr = libc::mmap(
        std::ptr::null_mut(),
        4 * 1024 * 1024,
        libc::PROT_READ | libc::PROT_WRITE,
        libc::MAP_SHARED,
        fd,
        0,
    );
    // ... build an index into the mapping ...

    // 3. Seal it so consumers can mmap RO without races.
    //    SHRINK prevents ftruncate-down; GROW prevents ftruncate-up;
    //    WRITE prevents further writes; SEAL prevents more seals from being added.
    libc::fcntl(fd, libc::F_ADD_SEALS,
        libc::F_SEAL_SHRINK | libc::F_SEAL_GROW
        | libc::F_SEAL_WRITE | libc::F_SEAL_SEAL);

    // 4. Pass fd to consumers via SCM_RIGHTS or clone3(CLONE_FILES).
    //    They mmap it RO and treat the contents as immutable.

    // 5. Close the final fd: when the last consumer closes theirs, the kernel
    //    frees the memory. No filesystem cleanup.
    libc::munmap(ptr, 4 * 1024 * 1024);
    libc::close(fd);
}
```

## Key flags

### `memfd_create` flags

| Flag | Meaning |
| --- | --- |
| `MFD_CLOEXEC` | Close on exec. Always set. |
| `MFD_ALLOW_SEALING` | Permit later `F_ADD_SEALS` calls on this fd. Required if consumers `mmap` read-only and you want to promise immutability. |
| `MFD_HUGETLB` | Back with huge pages. Pair with `MFD_HUGE_2MB` or `MFD_HUGE_1GB`. Good for large index arenas. |
| `MFD_NOEXEC_SEAL` | Linux 6.3+: apply `F_SEAL_EXEC` automatically. Prevents the memfd from ever being mmap'd executable — a mild defence against exploitation if you ever accept untrusted data. |

### Seals (`fcntl(F_ADD_SEALS, ...)`)

| Seal | Meaning |
| --- | --- |
| `F_SEAL_SEAL` | No more seals can be added. Always the last one you apply. |
| `F_SEAL_SHRINK` | File size cannot decrease. Required before safe `mmap` by other processes. |
| `F_SEAL_GROW` | File size cannot increase. |
| `F_SEAL_WRITE` | No further writes permitted. Turns the memfd into a read-only shared region. |
| `F_SEAL_FUTURE_WRITE` | Linux 5.1+: prevents future writes, but keeps existing writable mappings functional. Softer than `F_SEAL_WRITE`. |
| `F_SEAL_EXEC` | Linux 6.3+: prevents mapping with `PROT_EXEC`. Security hardening. |

## Gotchas

- **Not on disk — the memory counts against `RLIMIT_MEMLOCK` / cgroup memory.** A 64 GB memfd is a 64 GB RAM commitment. Plan capacity.
- **Sealing is one-way.** Once `F_SEAL_WRITE` is on, the fd is read-only forever. The usual pattern: build → seal → share.
- **Existing writable mappings survive `F_SEAL_WRITE`.** The seal blocks *new* `mmap(PROT_WRITE)` and `write()`. Existing `MAP_SHARED` mappings still let you write. Use `F_SEAL_FUTURE_WRITE` if you want the existing writers to keep working while preventing new ones.
- **Sharing between processes.** The standard mechanisms: `SCM_RIGHTS` over a Unix socket, or `clone3(CLONE_FILES)` to inherit the fd table. Both preserve fd identity — the receiver sees the same memfd.
- **Not for durable data.** The memfd dies with its last fd. If you need the contents persisted, write them to a real file before closing.

## Used by

Phase 3 of the database series — index-build-then-swap (mentioned in [03-inmemory-engine.md § Recovery](../runtime/database/03-inmemory-engine.md#recovery) as "build a .seg index in memory before atomically swapping it to disk"). Also any future IPC story with worker processes (Phase 6 full-stack with multiple render workers, say).

## v1 port source

**None.** V1 doesn't use anonymous memory — all index work hits the filesystem directly.
