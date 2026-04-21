# 10 — `pidfd`

Process identifier as a file descriptor. `pidfd_open(pid)` returns an fd that becomes readable when the process exits — reaping is a `read`, not a `waitpid` race. Lets the supervisor track child processes on the same `epoll` loop that drives everything else, with no PID-reuse bugs (an fd can't be recycled to point at a different process).

Not on the runtime's critical path today; useful when the runtime grows a supervisor (spawning workers, running `wo build` subprocesses, managing a sharded engine's child processes). Worth knowing the shape now so Phase 9+ doesn't reinvent `waitpid`.

## Kernel source

| Path | What |
| --- | --- |
| [`reference/linux/kernel/pid.c`](../../../reference/linux/kernel/pid.c) | `SYSCALL_DEFINE2(pidfd_open, ...)`, `pidfd_create`, `pidfd_pid`. |
| [`reference/linux/kernel/signal.c`](../../../reference/linux/kernel/signal.c) | `SYSCALL_DEFINE4(pidfd_send_signal, ...)`. |
| [`reference/linux/kernel/fork.c`](../../../reference/linux/kernel/fork.c) | `clone3` — the only way to get a pidfd atomically with spawn. |
| [`reference/linux/include/uapi/linux/pidfd.h`](../../../reference/linux/include/uapi/linux/pidfd.h) | `PIDFD_*` flags. |

## Man pages

`man 2 pidfd_open`, `man 2 pidfd_send_signal`, `man 2 pidfd_getfd`, `man 2 clone3`.

## Rust FFI via `libc`

`libc` doesn't have direct wrappers for every pidfd syscall. Use `libc::syscall` with the numeric id:

```rust
use libc::{syscall, SYS_pidfd_open, SYS_pidfd_send_signal, SYS_pidfd_getfd};
use libc::{SYS_clone3, clone_args};       // clone3 also goes via syscall(SYS_clone3, ...)
use libc::{PIDFD_NONBLOCK, PIDFD_THREAD}; // Linux 5.10+
```

## Direct-syscall example

```rust
unsafe {
    // Open a pidfd for an already-running child (race-prone: the child could
    // have exited and the PID been reused before this call — fine for the
    // self-pid, risky for arbitrary children)
    let pidfd = libc::syscall(libc::SYS_pidfd_open, child_pid, 0);
    if pidfd < 0 { return Err(io::Error::last_os_error()); }

    // Register on epoll. EPOLLIN fires exactly once, when the process exits.
    let mut ev = libc::epoll_event {
        events: libc::EPOLLIN as u32,
        u64:    child_pid as u64,     // your correlation key
    };
    libc::epoll_ctl(epfd, libc::EPOLL_CTL_ADD, pidfd as i32, &mut ev);

    // On the readiness event, waitid collects the exit status
    let mut info: libc::siginfo_t = std::mem::zeroed();
    libc::waitid(libc::P_PIDFD, pidfd as u32, &mut info, libc::WEXITED);

    // Send a signal via the fd — no PID race
    libc::syscall(libc::SYS_pidfd_send_signal, pidfd, libc::SIGTERM, std::ptr::null::<libc::siginfo_t>(), 0);

    libc::close(pidfd as i32);
}
```

For race-free child spawn, use `clone3(CLONE_PIDFD)`:

```rust
let mut pidfd: i32 = -1;
let args = libc::clone_args {
    flags:    libc::CLONE_PIDFD as u64,
    pidfd:    &mut pidfd as *mut _ as u64,
    // ... stack, tls, etc.
    ..std::mem::zeroed()
};
let child = libc::syscall(libc::SYS_clone3, &args, std::mem::size_of::<libc::clone_args>());
```

## Key flags

| Flag | Meaning |
| --- | --- |
| `PIDFD_NONBLOCK` | Non-blocking reads; combine with epoll. Linux 5.10+. |
| `PIDFD_THREAD` | Open a pidfd for a TID, not just a PID. Rarely needed. |
| `CLONE_PIDFD` | Passed to `clone3` — kernel stores the new pidfd at `args.pidfd`. The atomic way to get a pidfd without a race window. |

## Gotchas

- **Kernel version matters.** `pidfd_open` is 5.3+; `PIDFD_NONBLOCK` is 5.10+; `pidfd_getfd` (steal an fd from another process) is 5.6+. Check your target range.
- **PID reuse race on manual `pidfd_open`.** If the child exited and something else was spawned with the same PID between `fork` and `pidfd_open`, you hold a pidfd for the wrong process. `clone3(CLONE_PIDFD)` eliminates the window; for arbitrary external processes, `pidfd_open` is best-effort.
- **`epoll` fires once per exit.** The pidfd stays readable forever after, which is sometimes useful (always-ready means always-wake-me), sometimes annoying (you must `EPOLL_CTL_DEL` or the loop spins).
- **`pidfd_send_signal` refuses to signal the init process** (`pid == 1`). Not a concern unless running as PID 1 in a container.
- **Permissions.** You can only pidfd-open a child of yours, or a process in the same session, or with `CAP_KILL`.

## Used by

Not used in phases 02–08. Future supervisor work: the `wo build` subprocess, a hypothetical `wo dev` hot-reload supervisor, or a sharded engine's child monitoring — all would register child pidfds on the existing event loop.

## v1 port source

**None.** V1 doesn't spawn processes.
