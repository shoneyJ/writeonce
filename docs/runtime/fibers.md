# Runtime Fibers

Runtime fibers are lightweight, user-space threads managed by an application's runtime system rather than the OS kernel. They enable massive concurrency — millions per machine — because they don't carry the overhead of kernel thread stacks and scheduling. Unlike pre-emptive kernel threads, fibers use cooperative multitasking: they yield control voluntarily at known suspension points.

## Context Switching

A context switch is saving the state of one execution unit and restoring another so it can continue running. The cost of this switch is what separates kernel threads from fibers.

### Kernel Thread Context Switch

When the OS switches between threads:

1. Save all CPU registers (general purpose, floating point, SIMD) to kernel memory
2. Save the thread's stack pointer
3. Flush the TLB (translation lookaside buffer) if switching processes
4. Update scheduler data structures
5. Restore the next thread's registers and stack pointer
6. Return to userspace

Cost: **1-10 microseconds**, involves a kernel trap (syscall boundary crossing), cache pollution from TLB flush.

### Fiber Context Switch

When a runtime switches between fibers:

1. Save a few registers (stack pointer, instruction pointer, callee-saved registers)
2. Swap the stack pointer to the next fiber's stack
3. Jump to the next fiber's saved instruction pointer

Cost: **~10-100 nanoseconds**, entirely in userspace, no kernel involvement, no TLB flush, cache stays warm.

```
Kernel thread switch:     ~1,000-10,000 ns   (kernel trap + TLB flush)
Fiber switch:             ~10-100 ns          (register swap in userspace)
                          100x cheaper
```

## Types of Multitasking

### Pre-emptive (Kernel Threads)

The OS scheduler interrupts threads at arbitrary points using timer interrupts. The thread does not choose when to yield — the kernel forces it.

```
Thread A: ████████──┐ (interrupted by OS)
                    │
Thread B:           └──████████──┐ (interrupted by OS)
                                 │
Thread A:                        └──████████
```

- Threads can be interrupted mid-instruction
- Requires locks/mutexes to protect shared state
- Fairness guaranteed by the scheduler
- Used by: pthreads, std::thread, OS processes

### Cooperative (Fibers / Green Threads)

Fibers explicitly yield at known points (I/O boundaries, channel sends, `.await` in Rust). The runtime only switches when the fiber says "I'm done for now."

```
Fiber A: ████████ yield ──┐
                          │
Fiber B:                  └── ████████ yield ──┐
                                               │
Fiber A:                                       └── ████████
```

- Fibers are never interrupted mid-computation
- No locks needed for single-threaded runtimes — only one fiber runs at a time
- Starvation possible if a fiber never yields (compute-heavy work blocks the loop)
- Used by: Go goroutines, Erlang processes, Lua coroutines, Rust async/await

### Comparison

| Property | Pre-emptive (Threads) | Cooperative (Fibers) |
|----------|----------------------|---------------------|
| Scheduling | OS kernel decides | Runtime decides at yield points |
| Context switch cost | ~1-10 us | ~10-100 ns |
| Stack size | 1-8 MB per thread (fixed) | Bytes to KB per fiber (growable) |
| Max concurrency | ~10,000 threads | ~1,000,000+ fibers |
| Synchronization | Locks, mutexes, atomics | Not needed in single-threaded runtime |
| Interruption | Any point (timer interrupt) | Only at yield points |
| Fairness | Guaranteed by scheduler | Must be designed (fiber must yield) |

## How Fibers Work Internally

A fiber needs three things:

1. **A stack** — a block of memory for local variables and call frames
2. **A saved context** — the register state at the point it yielded
3. **A function** — the code to run when resumed

### Minimal Fiber in C

```c
#include <ucontext.h>
#include <stdio.h>

static ucontext_t main_ctx, fiber_ctx;
static char fiber_stack[8192];

void fiber_fn() {
    printf("Fiber: running\n");
    // Yield back to main
    swapcontext(&fiber_ctx, &main_ctx);
    printf("Fiber: resumed\n");
    // Yield again
    swapcontext(&fiber_ctx, &main_ctx);
}

int main() {
    // Set up fiber context
    getcontext(&fiber_ctx);
    fiber_ctx.uc_stack.ss_sp = fiber_stack;
    fiber_ctx.uc_stack.ss_size = sizeof(fiber_stack);
    fiber_ctx.uc_link = &main_ctx;
    makecontext(&fiber_ctx, fiber_fn, 0);

    printf("Main: starting fiber\n");
    swapcontext(&main_ctx, &fiber_ctx);   // switch to fiber

    printf("Main: fiber yielded\n");
    swapcontext(&main_ctx, &fiber_ctx);   // resume fiber

    printf("Main: fiber yielded again\n");
    swapcontext(&main_ctx, &fiber_ctx);   // resume — fiber finishes

    printf("Main: done\n");
    return 0;
}
```

Output:
```
Main: starting fiber
Fiber: running
Main: fiber yielded
Fiber: resumed
Main: fiber yielded again
Main: done
```

`swapcontext` saves the current register state to one context struct and loads another — that's the entire fiber switch. No kernel involved.

### Minimal Fiber in Rust (unsafe)

Rust's async/await compiles to state machines, not stack-swapping fibers. But you can build raw fibers with inline assembly:

```rust
use std::arch::asm;

struct Fiber {
    stack: Vec<u8>,
    sp: *mut u8,  // saved stack pointer
}

impl Fiber {
    fn new(func: fn()) -> Self {
        let mut stack = vec![0u8; 8192];
        let sp = unsafe {
            let top = stack.as_mut_ptr().add(stack.len());
            let aligned = (top as usize & !0xF) as *mut u8;
            // Push the function pointer as the return address
            let sp = aligned.sub(8);
            *(sp as *mut fn()) = func;
            sp
        };
        Fiber { stack, sp }
    }

    unsafe fn switch_to(&mut self, from: &mut *mut u8) {
        // Save callee-saved registers and swap stack pointers
        asm!(
            "push rbx",
            "push rbp",
            "push r12",
            "push r13",
            "push r14",
            "push r15",
            "mov [{from}], rsp",     // save current sp
            "mov rsp, [{to}]",       // load fiber sp
            "pop r15",
            "pop r14",
            "pop r13",
            "pop r12",
            "pop rbp",
            "pop rbx",
            from = in(reg) from,
            to = in(reg) &self.sp,
        );
    }
}
```

This is what runtimes like Go and Erlang do internally — allocate a small stack, save/restore a handful of registers, and jump. The cost is a few nanoseconds.

## Fibers vs Rust async/await

Rust chose a different approach than fibers for its async model:

| Property | Fibers (Go, Erlang) | Rust async/await |
|----------|-------------------|-----------------|
| Implementation | Stack swapping at runtime | Compiler generates state machines |
| Stack | Each fiber has its own stack | No extra stack — state stored in Future struct |
| Memory per task | ~2-8 KB minimum (stack) | Bytes — only the live variables at yield points |
| Yield mechanism | `swapcontext` / assembly | `.await` compiles to `Poll::Pending` |
| Overhead | Stack allocation + register swap | Zero-cost — state machine is a regular struct |
| Debuggability | Separate stacks in debugger | State machine is harder to trace |
| Preemption | Runtime can preempt (Go does this) | Never preempted — cooperative only |

Rust's approach is called **stackless coroutines** — no extra stack per task. The compiler transforms each `async fn` into a state machine enum where each variant holds the local variables alive across an `.await` point.

## Relation to writeonce

The writeonce runtime (`wo-event`, `wo-rt`) uses neither fibers nor Rust async/await. It uses a **single-threaded event loop with callbacks** — the simplest model:

```
loop {
    events = epoll_wait()
    for event in events {
        match event.token {
            WATCHER => handle_file_change(),
            LISTENER => handle_accept(),
            HTTP_CONN => handle_request(),
            ...
        }
    }
}
```

This is the same model as nginx, Redis, and Node.js (before libuv's thread pool). It works because:

- Each handler runs to completion quickly (microseconds)
- No handler blocks — all I/O is non-blocking
- No concurrent access to shared state — one thing runs at a time
- Blocking work (index rebuild) offloads to a thread pool and signals back via eventfd

If writeonce ever needed millions of concurrent long-lived tasks (not just connections), fibers would be the next step. But for a content platform serving articles, the event loop is sufficient — and far simpler to reason about.

## Summary

```
Kernel threads:   OS-managed, pre-emptive, expensive switch, ~10K max
Fibers:           Runtime-managed, cooperative, cheap switch, ~1M+ max
Async/await:      Compiler-managed, cooperative, zero-cost, ~1M+ max
Event loop:       No tasks at all — just fd readiness + callbacks

Complexity:       event loop < fibers < async/await < threads
Concurrency:      event loop = fibers = async/await >> threads
```

The right choice depends on the workload. For writeonce — an event loop. For a database with millions of queries in flight — fibers or async. For CPU-bound parallel work — kernel threads.
