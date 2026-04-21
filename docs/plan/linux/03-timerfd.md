# 03 — `timerfd`

Timers as file descriptors. Set an expiry with `timerfd_settime`, `read` the fd to retrieve the number of expirations, `epoll` notifies the loop when the timer fires. Drives everything in the runtime that needs a deadline without a separate timer thread: keepalives, debounce windows, checkpoint intervals.

## Kernel source

| Path | What |
| --- | --- |
| [`reference/linux/fs/timerfd.c`](../../../reference/linux/fs/timerfd.c) | All three syscalls (`timerfd_create`, `timerfd_settime`, `timerfd_gettime`). |
| [`reference/linux/include/uapi/linux/timerfd.h`](../../../reference/linux/include/uapi/linux/timerfd.h) | `TFD_*` flags. |

## Man pages

`man 2 timerfd_create`, `man 2 timerfd_settime`, `man 2 timerfd_gettime`.

## Rust FFI via `libc`

```rust
use libc::{timerfd_create, timerfd_settime, timerfd_gettime};
use libc::{itimerspec, timespec};
use libc::{TFD_CLOEXEC, TFD_NONBLOCK, TFD_TIMER_ABSTIME};
use libc::{CLOCK_MONOTONIC, CLOCK_REALTIME};
```

## Direct-syscall example

```rust
unsafe {
    let fd = libc::timerfd_create(
        libc::CLOCK_MONOTONIC,
        libc::TFD_CLOEXEC | libc::TFD_NONBLOCK,
    );
    if fd < 0 { return Err(io::Error::last_os_error()); }

    // one-shot: fire in 150ms, no interval
    let spec = libc::itimerspec {
        it_value:    libc::timespec { tv_sec: 0, tv_nsec: 150_000_000 },
        it_interval: libc::timespec { tv_sec: 0, tv_nsec: 0 },
    };
    // periodic: replace it_interval with the period, e.g. { tv_sec: 1, tv_nsec: 0 }

    if libc::timerfd_settime(fd, 0, &spec, std::ptr::null_mut()) < 0 {
        return Err(io::Error::last_os_error());
    }

    // register fd on the epoll loop; on EPOLLIN:
    let mut expirations: u64 = 0;
    libc::read(fd, &mut expirations as *mut _ as *mut _, 8);
    // expirations > 0 means the timer fired. Usually 1 for a oneshot,
    // could be >1 for a periodic timer whose consumer missed ticks.
}
```

## Key flags

| Flag | Meaning |
| --- | --- |
| `CLOCK_MONOTONIC` | Steady clock; immune to wall-clock jumps. **Default choice for runtime timers.** |
| `CLOCK_REALTIME` | Wall clock; jumps when NTP corrects or user sets time. Avoid unless you need calendar semantics. |
| `TFD_CLOEXEC` | Close on exec. Always set. |
| `TFD_NONBLOCK` | `read` doesn't block when expirations == 0. Required when on `epoll`. |
| `TFD_TIMER_ABSTIME` | Interpret `it_value` as absolute (not relative). Passed to `timerfd_settime`'s `flags`, not at create. |

## Gotchas

- **Reads are always 8 bytes.** The value is an expiration count.
- **Disarming is `timerfd_settime(fd, 0, &{0,0,0,0}, NULL)`** — a zero spec cancels pending fires.
- **Re-arming a periodic timer** replaces the spec atomically; no "pause then resume" surface.
- **Resolution is nanosecond-granular but scheduling can slip.** Don't use for sub-millisecond accuracy — use a busy loop or hardware timer for that.

## Used by

[`02-event-loop-epoll.md`](../02-event-loop-epoll.md) — housekeeping timers. [`07-inotify-content-watcher.md`](../07-inotify-content-watcher.md) — 150 ms debounce window after an inotify burst. Future subscription phase — keepalive pings to long-lived connections.

## v1 port source

[`reference/crates/wo-event/src/timerfd.rs`](../../../reference/crates/wo-event/src/timerfd.rs) (91 LOC) — `TimerFd { oneshot(dur), periodic(dur), disarm, read_expirations }`.
