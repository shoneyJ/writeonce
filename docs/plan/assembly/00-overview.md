# 00 — The role of assembly in a runtime

Why does a runtime ship hand-written assembly at all? Three reasons — each one a place where a higher-level language literally cannot express the operation it needs, so the compiler is bypassed and machine instructions are written directly. Go's [`src/runtime/`](../../../reference/go/src/runtime/) is the canonical example; this doc names the three reasons and points at the Go files that embody each.

## 1 — Operations that violate the language's own calling convention

The biggest category. The language's calling convention — how arguments are passed, who saves which registers, how the stack grows — is the contract every compiled function obeys. A few runtime operations *have* to break it because they ARE the mechanism by which control flow enters and exits that contract.

**Goroutine stack switching.** When Go's scheduler switches from one goroutine to another, it's literally rewriting the stack pointer mid-function — jumping from one goroutine's stack to another's. The language compiler can't emit this safely because every function assumes its stack is the one it got called on. See [`reference/go/src/runtime/asm_amd64.s`](../../../reference/go/src/runtime/asm_amd64.s) for `TEXT runtime·gogo(SB)`, `TEXT runtime·mcall(SB)`, `TEXT runtime·systemstack(SB)` — all unavoidable.

**Signal-handler entry.** When a signal arrives, the kernel drops the process onto an alternate stack with preserved registers. Returning to normal code means restoring everything the handler touched plus switching stacks back. Go's `runtime·sigtramp` in [`reference/go/src/runtime/sys_linux_amd64.s`](../../../reference/go/src/runtime/sys_linux_amd64.s) handles this.

**Cgo boundary crossing.** Calling C from Go means switching to the OS thread's "real" stack (C expects contiguous stacks; Go uses segmented). Going back means the inverse. Entirely asm-driven.

## 2 — Architecture-specific instructions the compiler doesn't emit

Atomics, memory barriers, and some hardware-accelerated primitives need specific instruction sequences. A compiler that sees `a = *b` can't know whether you wanted a relaxed load or an acquire fence without annotation — and the *right* instruction on x86 vs ARM vs RISC-V is different.

**Atomic CAS / load-acquire / store-release.** On x86 it's `LOCK CMPXCHG`; on ARM it's `LDXR` / `STXR` with a retry loop; on RISC-V it's `LR.W.AQ` / `SC.W.RL`. Go emits these from [`reference/go/src/runtime/atomic_amd64.s`](../../../reference/go/src/runtime/atomic_amd64.s) (and its per-arch siblings) because a portable compiler can't.

**Memory barriers.** `MFENCE`, `LFENCE`, `SFENCE` on x86; `DMB` / `DSB` / `ISB` on ARM. Used by Go's `publicationBarrier`, `procyield`, and friends. Per-arch asm files carry them.

**Optimised `memmove` / `memequal` / `memclr`.** The compiler knows how to emit `rep movsb`, but a runtime sometimes ships a *better* version than the compiler's — wider vector loads, prefetch hints, alignment-aware loops. Go ships its own in [`asm_amd64.s`](../../../reference/go/src/runtime/asm_amd64.s) using AVX/SSE paths.

## 3 — Syscall trampolines

Every raw syscall to the kernel is an asm stub. The kernel expects arguments in specific registers (on x86_64: `rdi`, `rsi`, `rdx`, `r10`, `r8`, `r9`, with the syscall number in `rax`), a `syscall` instruction, and return-value unpacking from `rax` (including `-errno` convention). A high-level language's calling convention doesn't match that layout — you need a thin asm wrapper per syscall.

See [`reference/go/src/runtime/sys_linux_amd64.s`](../../../reference/go/src/runtime/sys_linux_amd64.s) — 43 `TEXT` functions, one per syscall family: `runtime·write`, `runtime·read`, `runtime·futex`, `runtime·clone`, `runtime·rt_sigaction`, `runtime·rt_sigprocmask`, `runtime·rt_sigreturn`, `runtime·sched_yield`, `runtime·mmap`, `runtime·munmap`, `runtime·madvise`, `runtime·epollcreate1`, `runtime·epollctl`, `runtime·epollwait`, etc.

Go does these in asm because it cannot rely on libc — Go's scheduler needs to enter/exit syscalls at exactly controlled points (`runtime·entersyscall`, `runtime·exitsyscall`) so the M (OS thread) can be parked or reused without losing the goroutine. Going through `libc::write` would sidestep the scheduler's accounting.

## How writeonce differs

**Rust + libc covers all three categories** for the specific workload the `rt` crate serves. No custom scheduler means no stack switching. `std::sync::atomic::*` emits the right arch-specific instructions per target. `libc::syscall(SYS_*, ...)` hits the kernel through glibc's own trampolines — we don't need our own because we don't need fine-grained control over scheduler park/unpark (there's no scheduler to park). `signalfd` (see [`../linux/04-signalfd.md`](../linux/04-signalfd.md)) makes signal-handler asm unnecessary.

The next document, [`01-go-runtime-asm.md`](./01-go-runtime-asm.md), catalogues Go's asm in concrete detail. The one after that, [`02-writeonce-stance.md`](./02-writeonce-stance.md), spells out the policy: **no custom assembly in `crates/rt`** — and lists the three edge cases where a future profiling run might force the decision.

## Reading order

1. **This doc** — the abstract "why asm exists in runtimes."
2. [`01-go-runtime-asm.md`](./01-go-runtime-asm.md) — concrete Go inventory with reference paths.
3. [`02-writeonce-stance.md`](./02-writeonce-stance.md) — the writeonce policy + escape hatches.
