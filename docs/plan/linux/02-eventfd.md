# 02 — `eventfd`

Counter as a file descriptor. `write(fd, &n, 8)` adds `n` to the counter; `read(fd, &buf, 8)` drains it to zero (or decrements by one in semaphore mode). Paired with `epoll`, it's the cheapest way to wake the event loop from another flow — cross-thread signalling, scheduled work, shutdown requests.

## Kernel source

| Path | What |
| --- | --- |
| [`reference/linux/fs/eventfd.c`](../../../reference/linux/fs/eventfd.c) | `SYSCALL_DEFINE2(eventfd, ...)`, `struct eventfd_ctx`, read/write handlers. |
| [`reference/linux/include/uapi/linux/eventfd.h`](../../../reference/linux/include/uapi/linux/eventfd.h) | `EFD_*` flags. |

## Man pages

`man 2 eventfd`.

## Rust FFI via `libc`

```rust
use libc::{eventfd, EFD_CLOEXEC, EFD_NONBLOCK, EFD_SEMAPHORE};
// read / write go through plain libc::read / libc::write

extern "C" {
    // eventfd is already wrapped in libc
}
```

## Direct-syscall example

```rust
unsafe {
    let fd = libc::eventfd(0, libc::EFD_CLOEXEC | libc::EFD_NONBLOCK);
    if fd < 0 { return Err(io::Error::last_os_error()); }

    // wake the loop from anywhere
    let one: u64 = 1;
    libc::write(fd, &one as *const _ as *const _, 8);

    // inside the loop, on EPOLLIN:
    let mut buf: u64 = 0;
    let n = libc::read(fd, &mut buf as *mut _ as *mut _, 8);
    // buf now holds the accumulated count (or 1 if EFD_SEMAPHORE)
}
```

## Key flags

| Flag | Meaning |
| --- | --- |
| `EFD_CLOEXEC` | Close on exec. Always set. |
| `EFD_NONBLOCK` | `read` returns `EAGAIN` when counter is zero instead of blocking. Required when the fd is on an `epoll` loop. |
| `EFD_SEMAPHORE` | Each `read` decrements by one (otherwise reads drain the whole counter). Useful as a bounded work queue. |

## Gotchas

- **Always 8-byte `read` / `write`.** Short reads/writes return `EINVAL` — the counter is `u64`, full word or nothing.
- **Writing `u64::MAX`** returns `EINVAL`; the counter can't hold more than `u64::MAX - 1`.
- **Multiple writers are OK**; the kernel serialises. But reads race — use `EFD_SEMAPHORE` if you want one consumer per write.
- **Not async-signal-safe.** Don't `write(fd, ...)` from a signal handler; use `signalfd` instead (see [04-signalfd.md](./04-signalfd.md)).

## Used by

[`02-event-loop-epoll.md`](../02-event-loop-epoll.md) — wake the loop for shutdown or internal work. Future `sub` crate ([`09-native-subscriptions`], not yet planned) uses it to signal that a subscriber queue has drained.

## v1 port source

[`reference/crates/wo-event/src/eventfd.rs`](../../../reference/crates/wo-event/src/eventfd.rs) (66 LOC) — `EventFd { new, write, read, as_raw_fd }`.
