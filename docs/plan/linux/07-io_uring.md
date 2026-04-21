# 07 — `io_uring`

Ring-buffer based async I/O (Linux 5.1+, mature 5.11+). Two lock-free SPSC rings shared between userspace and kernel: submissions (SQEs) go in one, completions (CQEs) come out of the other. Batched, zero-syscall submission (with SQPOLL), zero-copy where the underlying op allows. Successor to `epoll` + `libaio` for the storage engine's WAL fsync path and — eventually — the HTTP server's accept/recv/send path.

**Not on the runtime's critical path in phases 02–08.** Phase 02 uses `epoll`. `io_uring` comes in during [Phase 3 — In-Memory Engine](../runtime/database/03-inmemory-engine.md) for the WAL's group-commit fsync loop. This card is the reference for that phase.

## Kernel source

| Path | What |
| --- | --- |
| [`reference/linux/io_uring/`](../../../reference/linux/io_uring/) | Whole subsystem. Start with `io_uring.c` (ring setup + submission/completion) and `fs.c` (fsync op). |
| [`reference/linux/io_uring/io_uring.c`](../../../reference/linux/io_uring/io_uring.c) | `SYSCALL_DEFINE2(io_uring_setup, ...)`, `SYSCALL_DEFINE6(io_uring_enter, ...)`, `SYSCALL_DEFINE4(io_uring_register, ...)`. |
| [`reference/linux/include/uapi/linux/io_uring.h`](../../../reference/linux/include/uapi/linux/io_uring.h) | `struct io_uring_sqe`, `io_uring_cqe`, `io_uring_params`, every `IORING_*` flag. |

## Man pages

`man 7 io_uring` (overview + entire ring model), `man 2 io_uring_setup`, `man 2 io_uring_enter`, `man 2 io_uring_register`. Also the `liburing` manual — Axboe's C library — useful for the per-op surface even if we don't link it.

## Rust FFI via `libc`

`libc` currently exposes the **constants** and the raw **syscall numbers** (`SYS_io_uring_setup`, `SYS_io_uring_enter`, `SYS_io_uring_register`), not wrapper functions. Invoke via `libc::syscall`:

```rust
use libc::{syscall, SYS_io_uring_setup, SYS_io_uring_enter, SYS_io_uring_register};
use libc::{mmap, munmap, MAP_SHARED, MAP_POPULATE, PROT_READ, PROT_WRITE};
// struct layouts from include/uapi/linux/io_uring.h — must mirror exactly
```

## Direct-syscall example (minimum viable ring)

```rust
// 1. setup — size is the number of SQEs; kernel rounds to power of 2
let mut params: io_uring_params = std::mem::zeroed();
// params.flags |= IORING_SETUP_SQPOLL; // kernel polls SQ — zero-syscall submit
let ring_fd = libc::syscall(SYS_io_uring_setup, 256u32, &mut params as *mut _) as i32;

// 2. mmap the three regions the kernel allocated
let sq_ring = libc::mmap(
    std::ptr::null_mut(),
    params.sq_off.array as usize + params.sq_entries as usize * 4,
    PROT_READ | PROT_WRITE,
    MAP_SHARED | MAP_POPULATE,
    ring_fd,
    IORING_OFF_SQ_RING,
);
let cq_ring = libc::mmap(..., IORING_OFF_CQ_RING);
let sqes    = libc::mmap(..., IORING_OFF_SQES);

// 3. submit an fsync — fill an SQE and bump the SQ tail
let idx = *sq_tail & ring_mask;
let sqe = &mut *(sqes as *mut io_uring_sqe).add(idx as usize);
sqe.opcode  = IORING_OP_FSYNC as u8;
sqe.fd      = wal_fd;
sqe.user_data = commit_lsn;          // your correlation key
*sq_tail = sq_tail.wrapping_add(1);

// 4. enter — tell the kernel to process N SQEs, optionally wait for completions
libc::syscall(SYS_io_uring_enter, ring_fd, 1u32, 1u32, IORING_ENTER_GETEVENTS, 0, 0);

// 5. reap a CQE
let idx = *cq_head & ring_mask;
let cqe = &*(cq_ring.add(params.cq_off.cqes as usize) as *const io_uring_cqe).add(idx as usize);
let lsn = cqe.user_data;
let err = cqe.res;        // < 0 is -errno
*cq_head = cq_head.wrapping_add(1);
```

Full working code is ~200 LOC including error handling — see `liburing` source for the canonical shape.

## Key flags + ops

| | |
| --- | --- |
| `IORING_SETUP_SQPOLL` | Kernel thread polls the SQ — userspace writes SQEs with no syscall. One pinned kernel thread per ring. Needs `CAP_SYS_NICE` before 5.11. |
| `IORING_SETUP_IOPOLL` | Busy-poll completions from the NVMe device (no interrupts). Lower latency, higher CPU. Requires `O_DIRECT`. |
| `IORING_SETUP_SINGLE_ISSUER` | Optimisation when only one thread submits (Linux 6.0+). **Always set** in the single-threaded runtime. |
| `IORING_REGISTER_FILES` | Pre-register a set of fds with the ring — skips per-op fd-table lookup. Use it for the WAL fd. |
| `IORING_REGISTER_BUFFERS` | Pre-register userspace pages — skips per-op page pinning. Use for the WAL ring buffer. |
| `IOSQE_IO_LINK` | Chain SQEs — the second doesn't start until the first completes. Essential for WAL: `WRITE` linked to `FSYNC`. |
| `IORING_OP_WRITE`, `IORING_OP_FSYNC`, `IORING_OP_READ`, `IORING_OP_ACCEPT`, `IORING_OP_SEND`, `IORING_OP_RECV` | The ops that replace the phase-02 `epoll` + `read`/`write` dance. |

## Gotchas

- **Ring memory layout is ABI.** The kernel writes via the mmap'd regions; the `params.sq_off.*` / `cq_off.*` fields tell you the exact byte offsets. Hard-coding offsets breaks across kernel versions.
- **`user_data` is the correlation key.** The kernel echoes it back on the CQE untouched. Use it to thread whatever identifier you need (LSN, request id, subscriber id).
- **No ordering between unlinked SQEs.** Independent writes can complete in any order. Use `IOSQE_IO_LINK` for ordering (write-then-fsync) or per-fd serialization (one fd at a time).
- **CQE `res` is `-errno` on failure**, not `-1` + `errno`. Sign-extend it as `i32`, negate for the error code.
- **Always check `sq_ring_mask` from `params.sq_off.ring_mask`** before indexing. Never assume size 256.
- **`io_uring` has had CVE fights.** Some hosting providers and container runtimes disable it (`io_uring_disabled=2`). Detect at runtime and fall back to `epoll` — the phase-02 event loop stays useful forever as a compatibility path.

## Used by

Phase 3 of the database series — see [`docs/runtime/database/03-inmemory-engine.md`](../runtime/database/03-inmemory-engine.md). Specifically the WAL fsync path: link `WRITE` → `FSYNC` SQEs, submit many per tick, reap completions to ack committed transactions. Also the natural upgrade target for the HTTP server once Phase 4 adds the native wire protocol.

## v1 port source

**None.** The v1 crates predate `io_uring` and use `epoll` + blocking `fsync` on a WAL-writer thread. This crate will be new code in `crates/wal/` when Phase 3 activates.
