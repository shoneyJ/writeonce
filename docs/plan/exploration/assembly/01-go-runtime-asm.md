# 01 — Go's runtime assembly, catalogued

The Go runtime ships ~72 `TEXT` functions in `asm_amd64.s` alone, ~43 in `sys_linux_amd64.s`, and per-architecture variants of both for `386`, `arm`, `arm64`, `loong64`, `mips(64)x`, `ppc64x`, `riscv64`, `s390x`, `wasm`. This doc inventories them by purpose so a reader can map each Go asm concern to the writeonce equivalent (spoiler: usually "Rust stdlib does it"). Follow-on reading: [`02-writeonce-stance.md`](./02-writeonce-stance.md).

All paths are inside [`reference/go/src/runtime/`](../../../reference/go/src/runtime/).

## Scheduler & stack switching — `asm_<arch>.s`

One file per arch, everything that has to break Go's calling convention. The x86_64 version lives at [`asm_amd64.s`](../../../reference/go/src/runtime/asm_amd64.s).

| Go symbol | What |
| --- | --- |
| `runtime·gogo(SB)` | Jump to a goroutine's saved program counter on its stack. The machine-level act of resuming a suspended goroutine. |
| `runtime·mcall(SB)` | Call a function on the `g0` stack (the OS-thread's real stack). Used by anything that may block indefinitely — the calling goroutine gets parked. |
| `runtime·systemstack(SB)` | Transient jump to `g0` for a single function (GC operations, scheduler code) and back. |
| `runtime·morestack(SB)` | Stack-growth trampoline. Preamble-injected by the compiler when a function's stack usage exceeds the current segment; calls back into Go to allocate more. |
| `runtime·asmcgocall(SB)` | Enter C code. Switches from Go stack to the OS thread's C stack, marshals arguments, handles re-entry if C calls back into Go. |
| `runtime·cgocallback(SB)` | Re-enter Go from C. The inverse of the above. |
| `runtime·memmove(SB)` | Optimised memcpy; falls into SSE/AVX paths for large regions. Competes with (and often beats) the C library's version on specific inputs. |
| `runtime·memequal(SB)` | Fixed-length memory compare; used by map key comparisons and interface equality. |
| `runtime·memclrNoHeapPointers(SB)` | Fast zero-fill for regions the GC doesn't need to scan. Autovectorised; avoids Go-level write-barrier overhead. |
| `runtime·jmpdefer(SB)` | `defer` unwinding. Manually adjusts the frame pointer to jump into a deferred function as if it had been called at a different site. |
| `runtime·asyncPreempt(SB)` | Preemption entry point (Go 1.14+). A signal handler rewrites the target goroutine's PC to point here; on resume it parks and runs the scheduler. |

**Why it's asm:** every function above manipulates state (stack pointer, program counter, register contents) that Go's language semantics don't let you express.

## Atomics & barriers — `internal/runtime/atomic/atomic_<arch>.s`

Lives at [`internal/runtime/atomic/atomic_amd64.s`](../../../reference/go/src/internal/runtime/atomic/atomic_amd64.s) (and arch variants). Wrappers around arch-specific instructions:

| Go symbol | x86 instruction | Purpose |
| --- | --- | --- |
| `·Load` / `·Loadp` / `·Load64` | `MOV` | Acquire load. On x86, plain `MOV` is already acquire-ordered; on ARM the same operation needs `LDR` + `DMB ISH`. |
| `·Store` / `·Store64` / `·StoreRel` | `XCHG` or `MOV` + `MFENCE` | Release store. |
| `·Xchg` / `·Xchg64` | `XCHG` | Atomic swap. |
| `·Xadd` / `·Xadd64` | `LOCK XADD` | Atomic fetch-and-add. |
| `·Cas` / `·Cas64` / `·Casp` | `LOCK CMPXCHG` | Compare-and-swap. |
| `·And` / `·Or` / `·And8` / `·Or8` | `LOCK AND` / `LOCK OR` | Atomic bit manipulation. |

**Why it's asm:** the *instruction* per operation differs per architecture. Go abstracts them behind a consistent `sync/atomic` surface.

## Syscall trampolines — `sys_<os>_<arch>.s`

On Linux-x86_64 that's [`sys_linux_amd64.s`](../../../reference/go/src/runtime/sys_linux_amd64.s) — 43 `TEXT` functions. Each is a short wrapper: move args into the kernel's register layout, execute `SYSCALL`, convert `rax` into a Go return value + error.

| Go symbol | Linux syscall |
| --- | --- |
| `runtime·write` / `runtime·write1` | `write(2)` |
| `runtime·read` / `runtime·pread` | `read(2)` / `pread64(2)` |
| `runtime·closefd` | `close(2)` |
| `runtime·open` | `openat(2)` |
| `runtime·futex` | `futex(2)` |
| `runtime·clone` | `clone(2)` — M (OS thread) creation |
| `runtime·rt_sigaction` / `runtime·rt_sigprocmask` | Signal plumbing |
| `runtime·sigreturn` | Return from signal handler |
| `runtime·sched_yield` | Voluntary preemption |
| `runtime·mmap` / `runtime·munmap` / `runtime·madvise` | Memory operations |
| `runtime·epollcreate1` / `runtime·epollctl` / `runtime·epollwait` | The epoll trio |
| `runtime·exit` / `runtime·exitThread` | Process / thread termination |

**Why it's asm (in Go):** libc's syscall wrappers check for cancellation, TLS, errno state — overhead Go's scheduler can't afford between `entersyscall` and `exitsyscall`. Go needs precise control over when the goroutine is "in a syscall" so the M can be detached from the P.

**Note:** writeonce **does** use libc's syscall wrappers — we don't have a scheduler to detach, so the overhead is irrelevant. See the stance doc for the full rationale.

## Signal handling — `sigtramp` in `sys_<os>_<arch>.s`

`runtime·sigtramp` (same file as the syscalls) is the entry point the kernel jumps to when a signal fires. It saves the pre-signal register state, switches to the `g0` stack, calls into `runtime.sigtrampgo`, then restores on return. Needs asm because the ABI between kernel and user signal handler is rigid and arch-specific.

**Writeonce avoids this entirely** by using `signalfd` — signals become fd reads on the epoll loop, no handler ever runs. See [`../linux/04-signalfd.md`](../linux/04-signalfd.md).

## Cgo bridge — `cgo_<os>_<arch>.s`

Files like [`cgo/asm_amd64.s`](../../../reference/go/src/runtime/cgo/asm_amd64.s). Machine-code marshalling between Go's register convention and C's SysV AMD64 ABI. Needed because Go's calling convention uses stack slots differently from C's register passing.

**Writeonce doesn't cross language boundaries** — Rust is the only language in the binary; `libc` is already in Rust's register convention via `extern "C"`. No cgo bridge needed.

## Timers & monotime — `time_<os>_<arch>.s`

Small files providing the absolute-minimum latency paths for `nanotime()` and friends. On modern Linux they use `vDSO` entries (`__vdso_clock_gettime`) that bypass the syscall boundary.

**Writeonce uses `std::time::Instant::now()`** which internally invokes the same vDSO through glibc — already optimised, no added value in rolling our own.

## ASAN / MSAN / race detector — `asan_<arch>.s`, `race_<arch>.s`

Hookable entry points for sanitisers. Not relevant to writeonce.

## What's NOT in asm

Everything else in Go's runtime is plain Go: the scheduler's policy (`proc.go`), the garbage collector (`mgc.go` etc.), `netpoll` dispatch (`netpoll.go` — *Go code*; the platform-specific backends like `netpoll_epoll.go` are also pure Go that call into the asm `epollwait` trampoline). The asm is strictly the three categories in [`00-overview.md`](./00-overview.md): calling-convention-breaking operations, arch-specific instructions, and syscall trampolines.

The three categories writeonce **also** needs a solution for — but writeonce gets all three from Rust stdlib + libc. The next doc catalogues those mappings.
