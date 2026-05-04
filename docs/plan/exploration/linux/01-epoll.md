# 01 — `epoll`

Event-driven I/O multiplexing. One `epoll_fd` watches many fds for readiness; `epoll_wait` blocks the loop until at least one fires. Foundation of the whole runtime — every other primitive on this list is an fd that lands on an `epoll` loop.

## Kernel source

| Path | What |
| --- | --- |
| [`reference/linux/fs/eventpoll.c`](../../../reference/linux/fs/eventpoll.c) | All three syscalls (`epoll_create1`, `epoll_ctl`, `epoll_wait`) live here. Grep for `SYSCALL_DEFINE`. |
| [`reference/linux/include/uapi/linux/eventpoll.h`](../../../reference/linux/include/uapi/linux/eventpoll.h) | `struct epoll_event`, `EPOLL_*` flags, the userspace-facing ABI. |

## Man pages

`man 7 epoll` (overview), `man 2 epoll_create1`, `man 2 epoll_ctl`, `man 2 epoll_wait`.

## Rust FFI via `libc`

```rust
use libc::{epoll_create1, epoll_ctl, epoll_wait, epoll_event, EFD_CLOEXEC};
use libc::{EPOLL_CLOEXEC, EPOLL_CTL_ADD, EPOLL_CTL_MOD, EPOLL_CTL_DEL};
use libc::{EPOLLIN, EPOLLOUT, EPOLLET, EPOLLRDHUP, EPOLLHUP, EPOLLERR};

extern "C" {
    // all three already wrapped in libc — no SYS_* workaround needed
}
```

## Direct-syscall example

```rust
unsafe {
    let epfd = libc::epoll_create1(libc::EPOLL_CLOEXEC);
    if epfd < 0 { return Err(io::Error::last_os_error()); }

    let mut ev = libc::epoll_event {
        events: (libc::EPOLLIN | libc::EPOLLET) as u32,
        u64:    token_value,                        // your own dispatch key
    };
    if libc::epoll_ctl(epfd, libc::EPOLL_CTL_ADD, watched_fd, &mut ev) < 0 {
        return Err(io::Error::last_os_error());
    }

    let mut events: [libc::epoll_event; 64] = std::mem::zeroed();
    let n = libc::epoll_wait(epfd, events.as_mut_ptr(), 64, timeout_ms);
    for ev in &events[..n as usize] {
        let token = ev.u64;
        // dispatch based on token
    }
}
```

## Key flags

| Flag | Meaning |
| --- | --- |
| `EPOLL_CLOEXEC` | Close the `epoll_fd` on `exec` — always set it. |
| `EPOLLIN` / `EPOLLOUT` | Readable / writable readiness. |
| `EPOLLET` | **Edge-triggered.** Reads must drain until `EAGAIN`; no re-arm needed. Required for the single-threaded loop's correctness. |
| `EPOLLRDHUP` | Peer half-closed — distinguishes "client closed" from transient. |
| `EPOLLHUP` / `EPOLLERR` | Always implicitly set; you don't ask for them but you must handle. |

## Gotchas

- **Edge-triggered means drain.** A handler that reads once and returns leaks readiness; the next `epoll_wait` won't re-fire until more bytes arrive. Always `read()` in a loop until `EAGAIN`.
- **`timeout = -1` blocks forever**; `0` polls; positive milliseconds cap the wait.
- **`epoll_wait` can return fewer events than you asked.** Normal. Don't assume full batches.
- **Modifying a watched fd's interest requires `EPOLL_CTL_MOD`**, not delete-then-add — the atomic update avoids races with pending events.

## Used by

Every runtime phase that touches I/O: [`02-event-loop-epoll.md`](../02-event-loop-epoll.md), [`03-hand-rolled-http.md`](../03-hand-rolled-http.md), [`07-inotify-content-watcher.md`](../07-inotify-content-watcher.md), [`08-sendfile-static-assets.md`](../08-sendfile-static-assets.md).

## v1 port source

[`reference/crates/wo-event/src/epoll.rs`](../../../reference/crates/wo-event/src/epoll.rs) (183 LOC) — already wraps all three syscalls with a safe `EventLoop { register, deregister, wait_once }` facade.
