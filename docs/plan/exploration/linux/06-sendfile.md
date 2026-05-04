# 06 — `sendfile`

Zero-copy transfer from a file fd to a socket fd. The kernel splices pages directly from the page cache into the socket's send buffer — userspace never touches the bytes. One syscall, one copy (DMA → NIC), no userspace buffer.

## Kernel source

| Path | What |
| --- | --- |
| [`reference/linux/fs/read_write.c`](../../../reference/linux/fs/read_write.c) | `SYSCALL_DEFINE4(sendfile, ...)` and `SYSCALL_DEFINE4(sendfile64, ...)`. Modern glibc aliases the first to the second; the syscalls are distinguished by the offset type. |
| [`reference/linux/fs/splice.c`](../../../reference/linux/fs/splice.c) | Internally `sendfile` delegates to `splice_direct_to_actor`. Related — see [07-splice.md](./07-splice.md) if you ever need the more general fd-to-fd pipe path. |

## Man pages

`man 2 sendfile`.

## Rust FFI via `libc`

```rust
use libc::{sendfile, off_t};
// sendfile64 is the same syscall on 64-bit Linux; libc::sendfile already uses it
```

## Direct-syscall example

```rust
unsafe {
    // out_fd must be a socket; in_fd must be a regular file opened O_RDONLY.
    // Pass NULL for the offset pointer to advance the file's own file offset;
    // pass a &mut offset to keep the file position untouched and step through explicitly.
    let mut sent: isize = 0;
    let mut remaining = file_len;
    let mut offset: off_t = 0;

    while remaining > 0 {
        let n = libc::sendfile(sock_fd, file_fd, &mut offset as *mut _, remaining as usize);
        if n < 0 {
            let e = io::Error::last_os_error();
            match e.raw_os_error() {
                Some(libc::EAGAIN) | Some(libc::EWOULDBLOCK) => {
                    // re-arm EPOLLOUT on sock_fd and yield back to the loop;
                    // resume this call when the loop wakes us
                    return Err(e);
                }
                _ => return Err(e),
            }
        }
        sent += n;
        remaining -= n as u64;
        if n == 0 { break; } // peer closed
    }
}
```

## Key behaviour

| Detail | Notes |
| --- | --- |
| Input fd | Must support `mmap`-like access — regular files, shared memory, some block devices. **Not** sockets, pipes, or character devices. |
| Output fd | Must be a socket (kernel 2.6.33+ lifted the restriction to anything, but practically: sockets). |
| Max per-call | Kernel caps at ~2 GB per syscall regardless of what you request. Loop for bigger files. |
| `offset` pointer | If NULL, updates the input fd's internal offset (like `read` does). If non-NULL, updates only the pointed-to variable. **Always use non-NULL** when sharing the fd across concurrent readers. |
| Return | Bytes sent (possibly less than requested — partial send; re-arm `EPOLLOUT` and resume). |

## Gotchas

- **`EAGAIN` is the common partial-write case.** The socket's send buffer filled; register `EPOLLOUT`, drain the CQ when the loop fires, keep calling `sendfile` with the updated `offset`.
- **TLS and `sendfile` don't mix** (without kTLS). Encryption requires a userspace copy. If / when TLS lands, use kTLS (`setsockopt(TCP_ULP, "tls")`, kernel 4.13+, AES-GCM only in practice).
- **Chunked transfer encoding and `sendfile` also don't mix** — the chunk framing has to go around the payload. Send headers with `write`, then `sendfile` the body, then write the trailing zero-length chunk.
- **Compression can't happen on the wire** — the kernel doesn't gzip. Pre-compressed sibling files (`.gz`, `.br`) + `Content-Encoding` header is the idiom.
- **`fstat` before `sendfile`** to get the file length for `Content-Length` — spares the client from guessing when the body ends.

## Used by

[`08-sendfile-static-assets.md`](../08-sendfile-static-assets.md) — the `GET /static/...` handler. Future `##ui` SSR output bundles go through the same path.

## v1 port source

[`reference/crates/wo-serve/src/sendfile.rs`](../../../reference/crates/wo-serve/src/sendfile.rs) (109 LOC) — `send_file(sock, path) -> Result` wrapping the loop + `EAGAIN` handling.
