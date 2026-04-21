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

What are fibers? #
Fibers are a lightweight thread of execution similar to OS threads. However, unlike OS threads, they’re cooperatively scheduled as opposed to preemptively scheduled. What this means in plain English is that fibers yield themselves to allow another fiber to run. You may have used something similar to this in your programming language of choice where it’s typically called a coroutine, there’s no real distinction between coroutines and fibers other than that coroutines are usually a language-level construct, while fibers tend to be a systems-level concept.

Other names for fibers you may have heard before include:

green threads
user-space threads
coroutines
tasklets
microthreads
There are very few and minor differences between fibers and the above list. For the purposes of this document, we should consider them equivalent as the distinctions don’t quite matter.

Scheduling #
At any given moment the OS is running multiple processes all with their own OS threads. All of those OS threads need to be making forward progress. There’s two classes of thought when it comes to how you solve this problem.

Cooperative scheduling
Preemptive scheduling
It’s important to note that while you may observe that all processes and OS threads are running in parallel, scheduling is really providing the illusion of that. Not all threads are running in parallel, the scheduler is just switching between them quickly enough that it appears everything is running in parallel. That is they’re concurrent. Threads start, run, and complete in an interleaved fashion.

It is possible for multiple OS threads to be running in parallel with symmetric multiprocessing (SMP) where they’re mapped to multiple hardware threads, but only as many hardware threads as the CPU physically has.

Premptive scheduling #
Most people familiar with threads know that you don’t have to yield to other threads to allow them to run. This is because most operating systems (OS) schedule threads preemptively.

The points at which the OS may decide to preempt a thread include:

IO
sleeps
waits (seen in locking primitives)
interrupts (hardware events mostly)
The first three in particular are often expressed by an application as a system call. These system calls cause the CPU to cease executing the current code and execute the OS’s code registered for that system call. This allows the OS to service the request then resume execution of your application’s calling thread, or another thread entierly.

This is possible because the OS will decide at one of the points listed above to save all the relevant state of that thread then resume some other thread, the idea being that when this thread can run again, the OS can reinstate that thread and continue executing it like nothing ever happened. These transition points where the OS switches a thread are called context switches.

There’s a cost associated with this context switching and all modern operating systems have made great deals of effort to reduce this cost as much as possible. Unfortunately, that overhead begins to show itself when you have a lot of threads. In addition, recent cache side channel attacks like: Spectre, Meltdown, Spoiler, Foreshadow, and Microarchitectural Data Sampling on modern processors has led to a series of both user-space and kernel-space mitigation strategies, some of which increased the overhead of context switches significantly.

You can read more about context switching overhead in this paper.

Cooperative scheduling #
This idea of fibers yielding to each other is what is known as cooperative scheduling. Fibers effectively move the idea of context switching from kernel-space to user-space and then make those switches a fundamental part of computation, that is, they’re a deliberate and explicitly done thing, by the fibers themselves. The benefit of this is that a lot of the previously mentioned overhead can be entierly eliminated while still permitting an excess count of threads of execution, just in the form of these fibers now.

The problem with multi-threading #
There’s many problems related to multi-threading, most obviously that it’s difficult to get right. Most proponents of fibers make false claims about how this problem goes away when you use fibers because you don’t have parallel threads of execution. Instead, you have these cooperatively scheduled fibers which yield to each other. This means it’s not possible to race data, dead lock, live lock, etc. While this statement is true when you look at fibers as a N:1 proposition, the story is entierly different when you introduce M:N.

N:1 and what it means #
Most documentation, libraries, and tutorials on fibers are almost exclusively based around using a single thread given to you by the OS, then sharing it among multiple fibers that cooperatively yield and run all your asynchronous code. This is called N:1 (“N to one”). N fibers to 1 thread, and it’s the most prevalent form of fibers. This is how Lua coroutines work, how Javascript’s and Python’s async/await work, and it’s not what you’re interested in doing if you actually want to take advantage of hardware threads. What you’re interested in is M:N, (“M to N”) M fibers to N threads.

M:N and what it means #
The idea behind M:N is to take the model given to us by N:1 and map it to multiple actual OS threads. Just like we’re familiar to the concept of thread pools where we execute tasks, here we have a pool of threads where we execute fibers and those fibers get to yield more of themselves on that thread.

I should stress that M:N fibers have all the usual problems of multi-threading. You still need to syncronize access to resources shared between multiple fibers because there’s still multiple threads.

The problem with thread pools #
A lot of you may be wondering how this is different from traditional task based parallelism choices seen in many game engines and applications. The model where you have a fixed-size pool of threads you queue tasks on to be executed at some point in the future by one of those threads.

Locality of reference #
The first problem is locality of reference. The data-oriented / cache-aware programmers reading this will have to mind my overloading of that phrase because what I’m really talking about is the resources that a job needs access to are usually local. The job isn’t going to be executed immediately, but rather when the thread pool has a chance to. Any resource that job needs access to, needs to be available for the job at some point in the future. This means local values need their lifetime’s extended for an undefined amount of time.

There’s many ways to solve this lifetime problem, except they all have overhead. Consider this trivial example where I want to asynchronously upload a local file to a webserver.

## Async Runtime for the `.wo` Database

The writeonce event loop (`wo-event` / `wo-rt`, described in [async.md](./async.md)) is a **single-threaded epoll loop with callbacks**. It works because the content workload is microseconds per request and single-writer. The `.wo` database (described in the [database series](./database/02-wo-language.md)) inverts every one of those assumptions:

| writeonce (blog) | `.wo` database |
| --- | --- |
| Single-threaded — one handler at a time | Thousands of queries in flight concurrently |
| epoll — kernel wakes you on fd readiness | io_uring — userland submits I/O, kernel completes asynchronously |
| Callbacks — run to completion, return to loop | Tasks — query execution spans multiple I/O waits (WAL write, network send) |
| No blocking — handlers are microsecond-fast | Graph traversals and join plans can be milliseconds of CPU |
| Single writer — no contention | MVCC — many readers and writers touching shared data structures |
| Read-heavy | Write-heavy on hot paths (checkout, inventory) |

A new runtime is needed. This section designs it, building on the fiber/async/event-loop theory above.

### What the Runtime Must Schedule

Every subsystem in the `.wo` engine produces work with a different shape:

| Subsystem | Work shape | Blocking? | Parallelizable? |
| --- | --- | --- | --- |
| **Client accept** | Wait for incoming TCP connection | I/O-bound | No — one listener fd |
| **Request decode** | Parse binary frame or GraphQL | CPU, microseconds | Yes — per connection |
| **Query planning** | Compile `.wo` to execution plan | CPU, microseconds | Yes — per query |
| **Query execution** | Traverse B+ tree / LSM / graph | CPU, microseconds to milliseconds | Yes — per query |
| **WAL append** | Serialize + io_uring write + fsync | I/O-bound (NVMe) | Batched — group commit |
| **MVCC bookkeeping** | Version chain prepend, snapshot management | Atomic CAS, nanoseconds | Yes — per record |
| **Subscription matching** | Evaluate deltas against registered predicates | CPU, microseconds | Yes — per subscription |
| **Client push (DELTA)** | io_uring send to client socket | I/O-bound | Yes — per connection |
| **Checkpoint** | Snapshot RAM arenas to SSD | I/O-bound, background | Single background task |
| **Vacuum** | Prune old MVCC versions | CPU, background | Parallelizable by range |

The runtime’s job is to keep all of these progressing concurrently — mixing CPU-bound query execution with I/O-bound disk and network operations — without blocking any subsystem on another.

### Four Candidate Architectures

#### 1. Thread-Per-Core / Shared-Nothing (Seastar / ScyllaDB)

Each CPU core runs its own independent event loop. No shared memory between cores. Communication is message-passing over lock-free queues.

```
Core 0                Core 1                Core 2                Core 3
┌──────────┐          ┌──────────┐          ┌──────────┐          ┌──────────┐
│ io_uring  │          │ io_uring  │          │ io_uring  │          │ io_uring  │
│ ring      │          │ ring      │          │ ring      │          │ ring      │
│           │          │           │          │           │          │           │
│ local     │ ←msg→    │ local     │ ←msg→    │ local     │ ←msg→    │ local     │
│ shard of  │          │ shard of  │          │ shard of  │          │ shard of  │
│ data      │          │ data      │          │ data      │          │ data      │
└──────────┘          └──────────┘          └──────────┘          └──────────┘
```

**How it works**: data is hash-partitioned across cores. A query for `product.id = 42` routes to the core that owns shard `hash(42) % num_cores`. That core executes the query entirely locally — no locks, no contention.

**Pros**: zero contention on the hot path; each core runs flat-out; NUMA-friendly; no lock overhead.

**Cons**: cross-shard queries (joins, graph traversals spanning partitions) require inter-core messaging; range scans hit all shards; transaction coordination across shards is complex; programming model is unfamiliar.

**Used by**: ScyllaDB/Seastar (C++), Redpanda (C++), Glommio (Rust).

#### 2. Work-Stealing Async Runtime (tokio / Rust `async/await`)

M:N scheduling of `async` tasks across a thread pool. Tasks (futures) yield at `.await` points; the scheduler steals work from busy threads.

```
┌─────────────────────────────────────────┐
│          tokio runtime (M:N)            │
│  ┌────────┐  ┌────────┐  ┌────────┐    │
│  │ worker │  │ worker │  │ worker │    │
│  │ thread │  │ thread │  │ thread │    │
│  │        │  │        │  │        │    │
│  │ task   │  │ task   │  │ task   │    │
│  │ task   │  │ task   │←steal──┐ │    │
│  │ task   │  │        │  │     task    │
│  └────────┘  └────────┘  └────────┘    │
│              shared task queues          │
└─────────────────────────────────────────┘
```

**How it works**: each query becomes an `async fn` that `.await`s I/O (network reads, WAL writes). The executor multiplexes thousands of tasks across a small thread pool. Shared state is accessed through `Arc<RwLock<T>>` or lock-free structures.

**Pros**: familiar Rust async model; excellent library ecosystem; handles mixed I/O and CPU work; tokio has 7+ years of production hardening.

**Cons**: `async/await` infects the entire codebase (everything must be async); shared state needs locks or lock-free structures; work-stealing adds scheduling overhead; harder to reason about NUMA locality.

**Used by**: SurrealDB (Rust + tokio), TiKV (Rust + tokio), Materialize (Rust + tokio).

#### 3. M:N Fiber Runtime (Go / Erlang)

User-space fibers with their own stacks, scheduled cooperatively across OS threads. Each query is a fiber; yield points are implicit at I/O boundaries.

```
┌────────────────────────────────────────┐
│           Go-style runtime             │
│                                        │
│  goroutine  goroutine  goroutine       │
│  goroutine  goroutine  goroutine       │
│  goroutine  goroutine  goroutine       │
│       ↓         ↓         ↓            │
│  ┌────────┐ ┌────────┐ ┌────────┐     │
│  │ OS thr │ │ OS thr │ │ OS thr │     │
│  └────────┘ └────────┘ └────────┘     │
│       work-stealing scheduler          │
└────────────────────────────────────────┘
```

**How it works**: each query spawns a fiber. The fiber runs synchronously from its own perspective — blocking calls (network read, disk write) are transparently converted to yields by the runtime. The scheduler multiplexes fibers across OS threads.

**Pros**: synchronous programming model (no `async`/`.await` annotations); lightweight (2–8 KB per fiber vs. the state-machine size of a Rust future); Go and Erlang prove this works at massive scale; preemption possible (Go preempts goroutines at function calls since Go 1.14).

**Cons**: stack allocation per fiber (minor); GC pressure if using Go (major for database latency); less ecosystem in Rust (no production-quality M:N fiber runtime); context switch is ~10–100 ns vs. ~1 ns for a Rust future poll.

**Used by**: CockroachDB (Go), Dgraph (Go), Erlang/OTP databases, may_minihttp (Rust).

#### 4. Deterministic io_uring Loop + Thread Pool (TigerBeetle)

A single main thread drives an io_uring instance for all I/O. CPU-bound work (query execution) is dispatched to a thread pool. Results return to the main loop via the io_uring completion queue.

```
┌────────────────────────────────────────────┐
│           Main thread (deterministic)      │
│                                            │
│  io_uring ring:                            │
│    - ACCEPT  (new connections)             │
│    - RECV    (query frames)                │
│    - WRITE   (WAL append)                  │
│    - FSYNC   (WAL durable)                 │
│    - SEND    (results / deltas)            │
│                                            │
│  On CQE(RECV, query_bytes):                │
│    dispatch to thread pool                 │
│                                            │
│  On CQE(thread_pool_result):               │
│    submit SEND(client_fd, result_bytes)     │
└────────────────────────────────────────────┘
         │                    ▲
         ▼                    │
┌────────────────────┐   ┌─────────┐
│   Worker threads   │   │ eventfd │
│   (query exec)     │──→│ signal  │
│                    │   │ back    │
└────────────────────┘   └─────────┘
```

**How it works**: all I/O flows through one io_uring instance on the main thread. When a query arrives (CQE for RECV), the main thread dispatches it to a worker. The worker executes the query (CPU-bound, touching in-memory data structures), then signals completion back to the main loop via eventfd. The main loop submits the SEND SQE to return the result.

**Pros**: fully deterministic — the main loop processes events in a fixed order, which enables replay-based testing and debugging; io_uring handles both disk and network in one scheduler; minimal coordination (workers are fire-and-forget); easiest to reason about.

**Cons**: single main thread is a potential bottleneck for very high connection rates; worker pool dispatch adds one context switch per query; all state mutation in the main loop limits write throughput to one core.

**Used by**: TigerBeetle (Zig), LMAX Disruptor (Java, similar philosophy).

### Decision: Which Model for `.wo`

> **Canonical model: single-threaded event loop.** The rest of this section catalogues a hybrid multi-threaded architecture that was the original proposal; it now lives here as the **scale-out reference** for when one core isn't enough. The Phase 2 spec ([02-wo-language.md § Concurrency Model](./database/02-wo-language.md#concurrency-model)) pins the initial runtime to a single userland thread driving io_uring directly (option 4 below, deterministic io_uring loop), without a worker pool, lock-free shared state, or a dedicated WAL-writer thread. Group commit still happens — the loop drains pending commits into one fsync SQE per tick. Scale past one core by **sharding** independent engine processes, not by reintroducing the multi-threaded hybrid here.

The multi-threaded architecture below remains useful as (a) a reference for the sharded model's internal coordination when cross-shard 2PC is added, and (b) the fallback if a workload profile ever justifies abandoning the single-threaded invariant. Keep reading if you want the full trade-off map; skip to the next section if you only care about what ships.

The architecture maps to the database subsystems (multi-threaded variant — not the current target):

| Subsystem | Best fit | Why |
| --- | --- | --- |
| **I/O scheduling** (accept, recv, send, WAL, fsync) | io_uring on a dedicated I/O thread | One submission queue; batched syscalls; handles disk + network |
| **Query execution** (plan, traverse, filter) | Worker thread pool | CPU-bound; parallelizable per query; no I/O in the hot path (data is in RAM) |
| **WAL writer** | Single dedicated thread | Ordered writes; group commit batching; sequential fsync |
| **Subscription matching** | Worker thread pool (same as query execution) | CPU-bound predicate evaluation; parallelizable per commit |
| **Client push** | io_uring SEND from I/O thread | I/O-bound; batched with other sends |
| **Checkpoint / vacuum** | Background threads | Long-running, low-priority, can yield to production traffic |
| **Transaction coordination** | Lock-free shared state (AtomicU64 for LSN, CAS for version chains) | Must be accessible from any worker |

This multi-threaded variant is a **hybrid** — closest to **option 4 (TigerBeetle) extended with a worker pool and lock-free shared state**. The actual Phase 2 design stops at plain option 4 (main-thread-only) — no worker pool, no cross-thread lock-free structures:

```
┌───────────────────────────────────────────────────────────┐
│                    .wo Runtime                             │
│                                                           │
│  ┌───────────────────────────────────────────────────┐    │
│  │               I/O Thread                          │    │
│  │  io_uring ring:                                   │    │
│  │    ACCEPT → new session                           │    │
│  │    RECV   → dispatch query to worker pool         │    │
│  │    SEND   → results / subscription deltas         │    │
│  │    WRITE  → WAL (via WAL writer thread)           │    │
│  │    FSYNC  → WAL barrier                           │    │
│  │    TIMEOUT → keepalive / session expiry            │    │
│  └───────────────────────────────────────────────────┘    │
│          │ dispatch               ▲ results               │
│          ▼                        │                       │
│  ┌───────────────────────────────────────────────────┐    │
│  │            Worker Pool (N = num_cores - 2)        │    │
│  │                                                   │    │
│  │  worker 0: parse → plan → execute → match subs    │    │
│  │  worker 1: parse → plan → execute → match subs    │    │
│  │  worker 2: parse → plan → execute → match subs    │    │
│  │  ...                                              │    │
│  │                                                   │    │
│  │  Shared (lock-free):                              │    │
│  │    - B+ tree (optimistic lock coupling)           │    │
│  │    - LSM memtable (crossbeam-skiplist)            │    │
│  │    - Graph adjacency (dashmap)                    │    │
│  │    - MVCC version chains (atomic CAS)             │    │
│  │    - Subscription registry (sharded RwLock)       │    │
│  └───────────────────────────────────────────────────┘    │
│          │ WAL records            ▲ fsync ack             │
│          ▼                        │                       │
│  ┌───────────────────────────────────────────────────┐    │
│  │            WAL Writer Thread                      │    │
│  │                                                   │    │
│  │  collect WAL records from worker batch queue       │    │
│  │  serialize → io_uring WRITE + FSYNC (linked SQE)  │    │
│  │  on CQE: signal workers that commit is durable    │    │
│  │                                                   │    │
│  │  Group commit: batch N commits into one fsync     │    │
│  └───────────────────────────────────────────────────┘    │
│                                                           │
│  ┌────────────────────┐  ┌────────────────────┐           │
│  │  Checkpoint thread │  │  Vacuum thread     │           │
│  │  (background)      │  │  (background)      │           │
│  └────────────────────┘  └────────────────────┘           │
└───────────────────────────────────────────────────────────┘
```

### Thread Roles — Fixed, Not Dynamic

Each thread has a single role for its lifetime. No work-stealing across roles. This gives predictability and avoids cache-pollution:

| Thread | Count | Role | I/O model |
| --- | --- | --- | --- |
| **I/O thread** | 1 | Accept, recv, send via io_uring; dispatch queries; push subscription deltas | io_uring SQ/CQ poll |
| **Worker threads** | `num_cores - 3` | Parse, plan, execute queries; match subscriptions; stage MVCC mutations | Pure CPU; no I/O; no syscalls |
| **WAL writer** | 1 | Collect committed WAL records; batch; io_uring write + fsync; signal durability | Dedicated io_uring ring for WAL fd |
| **Checkpoint** | 1 | Periodic arena snapshot to SSD | io_uring or plain `pwrite` |
| **Vacuum** | 1 | Prune MVCC version chains; reclaim memtable space | CPU-bound scan |

Total: `num_cores` threads, one per core, pinned via `pthread_setaffinity_np`. No oversubscription, no context switches between roles.

### Query Lifecycle Through the Runtime

A checkout query flows through the runtime like this:

```
1. Client sends EXECUTE frame over TCP
   → io_uring CQE(RECV) on I/O thread

2. I/O thread deserializes frame, identifies session + prepared plan
   → pushes (session, plan, params) onto worker dispatch queue

3. Worker thread picks up the query
   → Acquires MVCC snapshot (AtomicU64 read — nanoseconds)
   → Executes plan against in-memory structures:
       UPDATE products SET inventory.on_hand = ... WHERE id = 42
       INSERT INTO orders ...
       MATCH ... CREATE ... 
   → Stages mutations in a local write-set (no global visibility yet)
   → Evaluates constraints
   → Pushes WAL record to WAL writer’s batch queue

4. WAL writer collects this record + records from other workers
   → Serializes batch
   → Submits io_uring WRITE + linked FSYNC to NVMe
   → On CQE(FSYNC): marks all records in the batch as durable
   → Signals each worker’s commit-complete channel

5. Worker receives durability signal
   → Publishes mutations to global MVCC (atomic pointer swaps)
   → Evaluates subscription registry against the delta
   → Pushes DELTA frames to the I/O thread’s send queue

6. I/O thread submits io_uring SEND for each DELTA + the RESULT frame
   → Client receives commit acknowledgment
   → Subscribers receive live inventory update

Total wall time: ~100–200 μs (dominated by step 4: NVMe fsync)
```

### Why Not Pure Async (tokio)?

Tokio is production-proven and SurrealDB uses it. But for this database, the hybrid is better:

| Concern | tokio | Hybrid (this design) |
| --- | --- | --- |
| **io_uring integration** | `tokio-uring` exists but is experimental; tokio’s core is epoll-based | io_uring is the primary I/O model; no epoll fallback |
| **Determinism** | Work-stealing introduces non-deterministic scheduling | Fixed thread roles; deterministic dispatch; replay-testable |
| **GC / allocator pressure** | Futures allocate on the heap; many small allocations per query | Workers use arena allocators; pre-allocated per-query scratch space |
| **Cache locality** | Tasks migrate between cores via work-stealing | Threads are pinned; data stays cache-hot per core |
| **Debugging** | Async stack traces are notoriously hard to read | Each thread has a clear role; stack traces are synchronous |
| **Dependency weight** | tokio + tower + hyper + … | libc + io_uring syscalls |

The trade-off: less library reuse, more manual plumbing. For a database where every microsecond on the commit path matters, that trade-off is correct.

### Why Not Pure Fibers (Go)?

Go’s goroutine scheduler is excellent and CockroachDB proves databases can be built on it. But:

| Concern | Go goroutines | Hybrid (this design) |
| --- | --- | --- |
| **GC pauses** | Stop-the-world pauses during checkout fsync batches lose money | No GC — Rust or C++ with arena allocators |
| **io_uring** | Go has no native io_uring support; falls back to epoll + thread pool for disk I/O | io_uring is first-class |
| **Memory control** | Cannot pin arenas, control huge pages, or use `mlockall` idiomatically | Full control via `libc` bindings |
| **Lock-free structures** | Possible but `sync/atomic` is more limited than Rust’s `crossbeam` | `crossbeam`, `dashmap`, `arc-swap` — mature ecosystem |

If the database were written in Go, goroutines would be the right model. Since the [Phase 2 language analysis](./database/02-wo-language.md) argues for Rust or C++, fibers are not the natural fit.

### Synchronization Between Threads

Workers touch shared data structures. The synchronization budget is strict — any contention on the commit path adds latency to every checkout:

| Shared structure | Accessed by | Primitive | Contention |
| --- | --- | --- | --- |
| **LSN counter** | All workers + WAL writer | `AtomicU64::fetch_add` | One atomic increment per commit — cheapest possible |
| **MVCC version chains** | All workers (read + write) | Atomic CAS to prepend | Per-record; independent records don’t contend |
| **B+ tree internal nodes** | All workers (read) | Optimistic lock coupling (version + retry) | Read-dominant; writes hold latches for microseconds |
| **Worker dispatch queue** | I/O thread (push) + workers (pop) | Lock-free MPSC queue (`crossbeam-channel`) | One producer, N consumers — no contention |
| **WAL batch queue** | Workers (push) + WAL writer (drain) | Lock-free MPMC queue | Drained in bulk every fsync batch (~100 μs) |
| **Subscription registry** | Workers (match) + I/O thread (register/unregister) | Sharded `RwLock` | Readers (match on commit) never block each other |
| **Send queue** | Workers (push DELTA) + I/O thread (drain to io_uring) | Lock-free MPSC per connection | One consumer per connection fd |

No mutex on the commit path. The only serialization point is the LSN counter — a single `fetch_add`.

### Handling Compute-Heavy Queries

Graph traversals and analytical queries can consume milliseconds of CPU. In a fiber model, a long-running fiber starves others (cooperative scheduling). In this hybrid:

- Workers are pre-emptible at the OS level (kernel threads, not fibers).
- Each worker runs one query at a time to completion. If a query takes 5 ms, that worker is busy for 5 ms — but the other N-1 workers continue serving other queries.
- If the pool is saturated, the I/O thread applies back-pressure: it stops reading from client sockets (io_uring RECV is not re-submitted), TCP flow control kicks in, and clients experience latency — which is the correct behavior under overload.
- Optional: a per-query CPU budget (checked at loop iteration points in graph traversal) that yields the worker back to the pool and resumes the query later. This is partial preemption — fiber-like semantics within a thread pool.

### Lifecycle of a Subscription

Subscriptions are long-lived — they span many commits. The runtime handles them without dedicated threads or fibers:

```
1. Client sends SUBSCRIBE frame
   → I/O thread registers (predicate, client_fd) in subscription registry

2. A commit happens on a worker thread
   → Worker evaluates subscription registry against the commit’s delta
   → For each match: serialize DELTA frame, push to that client’s send queue

3. I/O thread drains send queues
   → Submits io_uring SEND for each queued DELTA

4. Client disconnects (CQE reports EPOLLHUP-equivalent)
   → I/O thread removes all subscriptions for that fd
```

No thread or fiber per subscription. No polling. The cost of a subscription is: one entry in the registry (a few hundred bytes) + O(1) evaluation per commit (if keyed) or O(subs) per commit (if arbitrary predicate). Thousands of active subscriptions add microseconds to each commit, not threads.

### Comparison With Real Database Runtimes

| Database | Language | Runtime model | I/O | Query scheduling |
| --- | --- | --- | --- | --- |
| **Postgres** | C | Process-per-connection | epoll + blocking I/O on worker processes | One process per query |
| **MySQL** | C++ | Thread-per-connection or thread pool | epoll | One thread per query |
| **SurrealDB** | Rust | tokio (M:N async) | epoll (tokio) + Rayon for CPU | async tasks + Rayon parallel iterators |
| **ScyllaDB** | C++ | Thread-per-core (Seastar) | io_uring / epoll / SPDK | Futures on per-core reactor |
| **TigerBeetle** | Zig | Single-threaded io_uring loop | io_uring | Deterministic, single-threaded |
| **CockroachDB** | Go | M:N goroutines | epoll (Go netpoller) | Goroutine per query |
| **DuckDB** | C++ | Thread pool | Blocking I/O | Morsel-driven parallelism |
| **`.wo`** (this design) | Rust/C++ | **Hybrid: io_uring I/O thread + pinned worker pool + dedicated WAL thread** | io_uring | Worker per query, lock-free shared state |

### What the Runtime Does NOT Do

Keeping the scope honest:

- **No work-stealing**. Workers are pinned, queries are assigned round-robin or by shard affinity. Work-stealing adds scheduling complexity for marginal throughput gains when the data is in RAM and queries are sub-millisecond.
- **No async/await in the engine codebase**. Workers run synchronous code against in-memory structures. The only async code is the I/O thread’s io_uring event loop.
- **No fiber stacks**. No `swapcontext`, no stack allocation per query. Each worker has one OS stack, runs one query at a time.
- **No M:N scheduling**. N queries map to N worker invocations, but each invocation is a plain function call on a fixed thread — not a scheduled task on a shared executor.

The runtime is deliberately simpler than tokio, Go’s scheduler, or Seastar. The bet is that with all data in RAM, query execution is fast enough that a fixed-size thread pool with lock-free shared state is sufficient — and far easier to debug, profile, and reason about.

### Reference Implementations

- **Seastar** — <https://github.com/scylladb/seastar>. The thread-per-core framework. Read `seastar/core/reactor.cc` for the io_uring event loop and `seastar/core/smp.cc` for inter-core messaging.
- **Glommio** — <https://github.com/DataDog/glommio>. Rust thread-per-core runtime built on io_uring. Closest Rust analogue to Seastar.
- **tokio** — <https://github.com/tokio-rs/tokio>. Work-stealing async runtime. Read `tokio/src/runtime/scheduler/` for the work-stealing logic.
- **TigerBeetle** — <https://github.com/tigerbeetle/tigerbeetle>. Deterministic single-threaded io_uring loop. Read `src/io.zig` for the I/O ring and `src/state_machine.zig` for deterministic processing.
- **crossbeam** — <https://github.com/crossbeam-rs/crossbeam>. Lock-free data structures for Rust: channels, skiplist, deque, epoch-based GC.
- **LMAX Disruptor** — <https://github.com/LMAX-Exchange/disruptor>. Lock-free ring buffer for inter-thread communication. The intellectual ancestor of the WAL batch queue.
- **Readings**:
  - *The Seastar Tutorial* — thread-per-core explained from first principles.
  - *Fibers under the magnifying glass* (Vyukov) — M:N scheduling in practice.
  - *io_uring and networking in 2023* (Axboe) — io_uring for both storage and networking.
  - *LMAX Architecture* (Fowler) — single-writer, mechanical sympathy, lock-free coordination.

### Where This Fits in the Database Series

This runtime design is the missing piece between [Phase 2 (ACID engine)](./database/02-wo-language.md) and [Phase 3 (in-memory storage)](./database/03-inmemory-engine.md). Phase 2 described *what* the engine does (ACID transactions across three paradigms). Phase 3 described *where* data lives (RAM, with io_uring WAL). This section describes *how* work is scheduled — the thread architecture that connects client I/O, query execution, WAL durability, and subscription push into a single coherent runtime.
