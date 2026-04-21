# 02 — The writeonce stance on assembly

**Policy: no custom assembly in `crates/rt`.** Use Rust stdlib + `libc` for every category Go's runtime solves with `.s` files. If a future profiling pass proves an asm optimisation is necessary, confine it to `crates/rt/src/runtime/asm/` with one file per use case and an `#[cfg(target_arch = "...")]` cover per file (the Go `asm_amd64.s` pattern, one-per-architecture).

## Why the policy works

Each Go asm category from [`01-go-runtime-asm.md`](./01-go-runtime-asm.md) maps to a Rust-stdlib equivalent that is already correct on every supported architecture:

| Go asm need | What writeonce uses | Why it covers the gap |
| --- | --- | --- |
| Scheduler stack switching (`gogo`, `mcall`, `systemstack`) | — nothing — | Single-threaded event loop (see Phase 2 [Concurrency Model](../runtime/database/02-wo-language.md#concurrency-model)). No goroutines, no stack switching, no `g0`. |
| Preemption (`asyncPreempt`) | — nothing — | No preemption. Handlers run to completion on the single thread. |
| Atomic operations (`Load`, `Store`, `Cas`, `Xadd`, ...) | [`std::sync::atomic`](https://doc.rust-lang.org/std/sync/atomic/) | The compiler emits the right instruction per target — `LOCK CMPXCHG` on x86, `LDXR/STXR` on ARM, `LR.W/SC.W` on RISC-V. Ordering is in the type signature (`Ordering::Acquire`, `Release`, `SeqCst`). |
| Memory barriers (`MFENCE` etc.) | [`std::sync::atomic::fence(Ordering)`](https://doc.rust-lang.org/std/sync/atomic/fn.fence.html) | One call, one fence, arch-neutral. |
| `memmove` / `memequal` / `memclr` | [`core::ptr::copy`](https://doc.rust-lang.org/core/ptr/fn.copy.html), `<[T]>::copy_from_slice`, `==`, `[T]::fill(0)` | LLVM emits the same vectorised code Go's asm does, often better because it knows alignment statically. |
| Syscall trampolines | `libc::write(...)`, `libc::read(...)`, `libc::syscall(libc::SYS_io_uring_enter, ...)` | No scheduler means no "park the M across this syscall" concern. Plain libc is the right abstraction. |
| Signal handler entry (`sigtramp`) | `signalfd` (see [`../linux/04-signalfd.md`](../linux/04-signalfd.md)) | Signals become fd reads on the epoll loop. No user-side handler runs; no trampoline needed. |
| Cgo boundary | — nothing — | Rust is the only language. `extern "C"` handles libc in the compiler's own ABI pass; no asm shim. |
| `nanotime` (vDSO) | [`std::time::Instant::now()`](https://doc.rust-lang.org/std/time/struct.Instant.html) | Already goes through the vDSO via glibc. No added value in rolling our own. |

Net result: the assembly layer that accounts for Go's runtime complexity collapses into Rust's standard library and a handful of `libc::*` calls. Zero hand-written `.s` files in the target architecture.

## Edge cases where asm might come up

Three situations where a future developer might reach for `std::arch::asm!` — and the first-line answer for each, which is always "try this Rust-stdlib path first."

### 1. `io_uring` ring atomics

The SQ and CQ rings need specific memory ordering around the `head`/`tail` indices: producer writes release-ordered, consumer reads acquire-ordered. The kernel and userland form a lockless SPSC queue across the mmap'd region.

- **Rust answer:** `AtomicU32` at the ring-index offsets with explicit `Ordering::Acquire` / `Ordering::Release`. `fence(Ordering::SeqCst)` where the kernel ABI demands a full barrier. This is how `io-uring` (the community crate) implements it — in safe Rust.
- **When to escalate:** never, realistically. Kernel + Rust stdlib agree on x86-TSO / ARM-AcRel semantics. An asm deviation would be a bug.

### 2. SIMD in the phase-05 JSON parser

`simd-json` and `serde-json-core` both show that JSON parsing benefits significantly from SIMD byte scans (whitespace skip, quote/escape detection).

- **Rust answer:** `core::arch::x86_64::*` intrinsics (`_mm256_cmpeq_epi8`, `_mm_movemask_epi8`) inside `#[cfg(target_feature = "avx2")]` blocks, with a scalar fallback. All safe-enough Rust — no `asm!` block needed. `std::simd` (nightly) is even cleaner when it stabilises.
- **When to escalate:** if a specific instruction sequence outperforms the compiler's codegen by a meaningful margin *under measurement*, wrap it in `asm!` inside a single `#[cfg(target_arch = "x86_64")]` helper. Today the compiler is almost always as good as handwritten for well-understood patterns.

### 3. WAL group-commit sub-µs timing

Phase 3's WAL loop batches commits and fsyncs them in one `io_uring` submission. If we ever need to shave nanoseconds off the per-commit path — spin-waiting on the `io_uring` CQE ring, say — the tightest loop might want specific instructions (`PAUSE` on x86, `WFE` on ARM).

- **Rust answer:** [`std::hint::spin_loop()`](https://doc.rust-lang.org/std/hint/fn.spin_loop.html). The compiler emits `PAUSE` / `WFE` per target. Use `core::hint::black_box` to prevent compiler over-optimisation of measurement.
- **When to escalate:** when benchmarks show a specific hotspot the compiler is provably wrong about. Not before.

## The escape hatch

If all three defences above are exhausted — benchmarked, documented, reviewed — assembly goes in:

```
crates/rt/src/runtime/asm/
├── mod.rs                # re-exports; arch gating
├── memcpy_avx2_x86_64.rs # #[cfg(all(target_arch = "x86_64", target_feature = "avx2"))]
└── spin_pause_x86_64.rs  # #[cfg(target_arch = "x86_64")]
```

Rules:

1. **One concern per file.** No `asm.rs` catch-all.
2. **Arch-gated at the file level.** `#[cfg(target_arch = "...")]` at the top of the file covering the whole module. A non-x86_64 build sees an empty module, not missing symbols.
3. **Scalar fallback in `mod.rs`.** Every asm function has a pure-Rust sibling gated with the negation. Hosts that fail the `target_feature` check get the fallback transparently.
4. **Comment every instruction with the intent.** `lock xadd` is opaque without "// atomic RMW for SQ head advance."
5. **Benchmark in the commit message.** Numbers before and after, the compiler version that produced the delta, and the specific CPU model.

No asm has been written under this policy yet. The expectation is it stays that way for the foreseeable future — the [`docs/plan/02-08`](../) sequence reaches feature parity with Go's runtime using exclusively the Rust-stdlib + libc path.

## Cross-references

- [`00-overview.md`](./00-overview.md) — why runtimes ever need asm at all (three categories).
- [`01-go-runtime-asm.md`](./01-go-runtime-asm.md) — Go's asm inventory, by file.
- [`../linux/04-signalfd.md`](../linux/04-signalfd.md) — the specific primitive that obviates Go's `sigtramp` asm.
- [`../02-event-loop-epoll.md`](../02-event-loop-epoll.md) — phase 02, where the `runtime/` module actually lands.
