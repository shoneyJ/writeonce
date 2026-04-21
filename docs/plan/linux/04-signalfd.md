# 04 — `signalfd`

Unix signals as file descriptors. `signalfd(fd, mask)` installs a mask on the process and returns an fd that becomes readable when any masked signal arrives. Replaces signal handlers entirely — no `sigaction`, no re-entrancy minefield, no interrupted syscalls. The whole `wo` binary's shutdown path is one `signalfd` on the event loop.

## Kernel source

| Path | What |
| --- | --- |
| [`reference/linux/fs/signalfd.c`](../../../reference/linux/fs/signalfd.c) | `SYSCALL_DEFINE4(signalfd4, ...)` + `signalfd_dequeue`. |
| [`reference/linux/include/uapi/linux/signalfd.h`](../../../reference/linux/include/uapi/linux/signalfd.h) | `struct signalfd_siginfo`, `SFD_*` flags. |
| [`reference/linux/kernel/signal.c`](../../../reference/linux/kernel/signal.c) | Background: `sigprocmask`, pending-signal dequeue. |

## Man pages

`man 2 signalfd`, `man 7 signal`.

## Rust FFI via `libc`

```rust
use libc::{signalfd, signalfd_siginfo, sigset_t};
use libc::{sigemptyset, sigaddset, sigprocmask};
use libc::{SFD_CLOEXEC, SFD_NONBLOCK, SIG_BLOCK, SIG_UNBLOCK};
use libc::{SIGINT, SIGTERM, SIGHUP, SIGQUIT};
```

## Direct-syscall example

```rust
unsafe {
    // 1. Block the signals in the thread so the kernel delivers them via the fd instead
    let mut mask: sigset_t = std::mem::zeroed();
    libc::sigemptyset(&mut mask);
    libc::sigaddset(&mut mask, libc::SIGINT);
    libc::sigaddset(&mut mask, libc::SIGTERM);
    if libc::sigprocmask(libc::SIG_BLOCK, &mask, std::ptr::null_mut()) < 0 {
        return Err(io::Error::last_os_error());
    }

    // 2. Create the fd
    let fd = libc::signalfd(-1, &mask, libc::SFD_CLOEXEC | libc::SFD_NONBLOCK);
    if fd < 0 { return Err(io::Error::last_os_error()); }

    // 3. Register fd on epoll. On EPOLLIN:
    let mut info: signalfd_siginfo = std::mem::zeroed();
    let n = libc::read(fd, &mut info as *mut _ as *mut _, std::mem::size_of::<signalfd_siginfo>());
    if n as usize == std::mem::size_of::<signalfd_siginfo>() {
        match info.ssi_signo as i32 {
            libc::SIGINT | libc::SIGTERM => begin_graceful_shutdown(),
            _ => {}
        }
    }
}
```

## Key flags

| Flag | Meaning |
| --- | --- |
| `SFD_CLOEXEC` | Close on exec. Always set. |
| `SFD_NONBLOCK` | Non-blocking reads. Required when on `epoll`. |
| `SIG_BLOCK` | Passed to `sigprocmask` to add the set to the current mask. The corresponding `SIG_UNBLOCK` / `SIG_SETMASK` are available if you need them. |

## Gotchas

- **Blocking the signal with `sigprocmask` is mandatory.** Otherwise the default handler runs and kills the process before the fd ever becomes readable. Block once at boot, never unblock.
- **Per-thread mask.** `sigprocmask` is per-thread. In a single-threaded runtime that's fine; if you ever spawn threads, use `pthread_sigmask` on each so the main loop gets the signals.
- **`signalfd_siginfo` is big** (128 bytes). Read into an aligned buffer; partial reads return `EINVAL`.
- **Doesn't catch `SIGKILL` or `SIGSTOP`.** Nothing does. Those bypass everything.
- **Child exit notifications (`SIGCHLD`)** work via signalfd but `pidfd` (see [10-pidfd.md](./10-pidfd.md)) is usually the better fit for clean child supervision.

## Used by

[`04-cutover-remove-tokio-axum.md`](../04-cutover-remove-tokio-axum.md) — replaces `tokio::signal::ctrl_c()` for graceful shutdown. Every subsequent phase inherits this pattern.

## v1 port source

[`reference/crates/wo-event/src/signalfd.rs`](../../../reference/crates/wo-event/src/signalfd.rs) (62 LOC) — `SignalFd::new(&[SIGINT, SIGTERM]) -> SignalFd` with a safe `read_signo()` helper.
