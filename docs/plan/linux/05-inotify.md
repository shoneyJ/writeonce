# 05 — `inotify`

Filesystem event notifications as a file descriptor. `inotify_add_watch(dir, mask)` installs a watch; reading the fd returns variable-length `inotify_event` records each time a matching file creates, modifies, or disappears. Replaces the S3 + Lambda pipeline from the v1 architecture — one fd on the event loop IS the content-sync engine.

## Kernel source

| Path | What |
| --- | --- |
| [`reference/linux/fs/notify/inotify/inotify_user.c`](../../../reference/linux/fs/notify/inotify/inotify_user.c) | `SYSCALL_DEFINE1(inotify_init1, ...)`, `SYSCALL_DEFINE3(inotify_add_watch, ...)`, `SYSCALL_DEFINE2(inotify_rm_watch, ...)`. |
| [`reference/linux/fs/notify/inotify/inotify_fsnotify.c`](../../../reference/linux/fs/notify/inotify/inotify_fsnotify.c) | The fsnotify backend that feeds events into the fd. |
| [`reference/linux/include/uapi/linux/inotify.h`](../../../reference/linux/include/uapi/linux/inotify.h) | `struct inotify_event`, `IN_*` masks. |

## Man pages

`man 7 inotify` (overview + event semantics), `man 2 inotify_init1`, `man 2 inotify_add_watch`, `man 2 inotify_rm_watch`.

## Rust FFI via `libc`

```rust
use libc::{inotify_init1, inotify_add_watch, inotify_rm_watch, inotify_event};
use libc::{IN_CLOEXEC, IN_NONBLOCK};
use libc::{IN_MODIFY, IN_CREATE, IN_DELETE, IN_CLOSE_WRITE};
use libc::{IN_MOVED_FROM, IN_MOVED_TO, IN_ISDIR, IN_Q_OVERFLOW};
```

## Direct-syscall example

```rust
unsafe {
    let fd = libc::inotify_init1(libc::IN_CLOEXEC | libc::IN_NONBLOCK);
    if fd < 0 { return Err(io::Error::last_os_error()); }

    let dir = std::ffi::CString::new("docs/examples/blog/types").unwrap();
    let wd = libc::inotify_add_watch(
        fd,
        dir.as_ptr(),
        (libc::IN_MODIFY | libc::IN_CLOSE_WRITE
         | libc::IN_CREATE | libc::IN_DELETE
         | libc::IN_MOVED_FROM | libc::IN_MOVED_TO) as u32,
    );
    if wd < 0 { return Err(io::Error::last_os_error()); }

    // register fd on epoll. On EPOLLIN:
    let mut buf = [0u8; 4096];
    let n = libc::read(fd, buf.as_mut_ptr() as *mut _, buf.len());
    // parse the buffer: a packed sequence of inotify_event records,
    // each followed by a variable-length name field (event.len bytes)
    let mut offset = 0usize;
    while offset < n as usize {
        let ev = &*(buf.as_ptr().add(offset) as *const inotify_event);
        let name_len = ev.len as usize;
        let name_start = offset + std::mem::size_of::<inotify_event>();
        let name = std::str::from_utf8(&buf[name_start..name_start + name_len])
            .unwrap().trim_end_matches('\0');
        // dispatch based on ev.mask + ev.wd → directory → full path
        offset = name_start + name_len;
    }
}
```

## Key flags

| Flag | Meaning |
| --- | --- |
| `IN_CLOEXEC` / `IN_NONBLOCK` | Close on exec, non-blocking reads. Always set. |
| `IN_MODIFY` | File content written. Fires per-`write(2)` — noisy; prefer `IN_CLOSE_WRITE`. |
| `IN_CLOSE_WRITE` | File opened for writing was closed. **Usual choice** — one event per editor save. |
| `IN_CREATE` / `IN_DELETE` | Entry created / deleted inside a watched directory. |
| `IN_MOVED_FROM` / `IN_MOVED_TO` | The two halves of a rename. Paired by `cookie`. Editors often write tmp → rename → delete; you get both halves. |
| `IN_ISDIR` | Set on the event when the target is a directory. |
| `IN_Q_OVERFLOW` | Kernel event queue overflowed; `wd = -1`, rescan from scratch. **Must handle.** |

## Gotchas

- **Per-directory watches, not per-file.** Watching individual files wastes descriptors and misses `IN_CREATE`/`IN_DELETE` for new entries. Watch the directory; filter by event `name` in userspace.
- **Recursive watching is manual.** Walk the tree at init and add a watch per directory. React to `IN_CREATE | IN_ISDIR` by adding a watch for the new subdirectory — and to `IN_MOVED_TO | IN_ISDIR` too.
- **`fs.inotify.max_user_watches`** defaults to 8192 on most distros. Recursive watches over a big node_modules or target dir exhaust it fast. Filter aggressively before adding.
- **Editors burst events.** Tmp-file + rename + delete is 3–4 events per logical save. Debounce 100–200 ms with [`timerfd`](./03-timerfd.md).
- **Reading less than a full event is an `EINVAL`.** Use a buffer ≥ `sizeof(inotify_event) + NAME_MAX + 1` (≈ 4 KiB is a safe size).
- **`wd` is stable per-watch but reused after `rm_watch`.** Keep a `wd → path` map; remove from it on `IN_IGNORED`.

## Used by

[`07-inotify-content-watcher.md`](../07-inotify-content-watcher.md) — the Stage-3 hot-reload feature. Future `sub` crate — the register-macro subscription model in [`00-linux.md § Database Subscription`](./00-linux.md#database-subscription).

## v1 port source

[`reference/crates/wo-watch/src/lib.rs`](../../../reference/crates/wo-watch/src/lib.rs) (280 LOC) — already does recursive watch setup, event parsing, and path resolution via a `wd → PathBuf` map.
