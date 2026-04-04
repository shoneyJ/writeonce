# Why Rust Does Not Use Fibers or Garbage Collection

## The Question

Most runtime-heavy languages ship with two things: a garbage collector (Go, Java, Python, C#, Erlang) and fiber-like concurrency (Go goroutines, Erlang processes, Java virtual threads). Rust ships with neither. Why?

The answer is the same for both: **Rust pushes the cost to compile time so there is zero cost at runtime.**

## Garbage Collection

### What a GC Does

A garbage collector tracks which objects in memory are still reachable from the program. Periodically (or continuously), it scans the heap, finds objects nothing points to, and frees them.

```
Allocate object A
Allocate object B
A.ref = B           // B is reachable through A
drop(A)             // A is unreachable — GC will free A
                    // B is now also unreachable — GC will free B
```

### How It Works (Simplified)

**Mark-and-sweep** (Go, Java):

```
1. Pause the program (or run concurrently)
2. Start from "roots" (stack variables, globals)
3. Mark every object reachable from roots
4. Sweep: free every object NOT marked
```

**Reference counting** (Python, Swift, Objective-C):

```
1. Every object has a counter
2. When a reference is created: counter++
3. When a reference is dropped: counter--
4. When counter == 0: free immediately
```

### The Costs

| Cost | Mark-and-sweep GC | Reference counting |
|------|-------------------|-------------------|
| Pause time | Stop-the-world pauses (Go: ~1ms, Java: varies) | No pauses, but slower per-operation |
| Memory overhead | 2x heap needed (live objects + garbage until collected) | Counter per object (8 bytes) |
| CPU overhead | GC thread scanning heap (10-30% throughput loss) | Increment/decrement on every pointer operation |
| Predictability | Unpredictable latency spikes | Predictable but cycles leak (need cycle collector) |
| Cache impact | GC walks heap → cache pollution | Counters spread across memory → cache misses |

For a content platform that needs predictable low-latency responses, GC pauses are the enemy. Even Go's ~1ms pauses compound under load — if a GC pause hits during `epoll_wait`, every pending connection stalls.

### What Rust Does Instead: Ownership

Rust replaces garbage collection with a compile-time ownership system:

```rust
fn main() {
    let s = String::from("hello");   // s owns the string, allocated on heap
    let t = s;                        // ownership moves to t — s is invalid
    // println!("{}", s);             // compile error: s was moved
    println!("{}", t);                // ok
}   // t goes out of scope → String freed here. Deterministic. No GC.
```

The rules:

1. **Every value has exactly one owner**
2. **When the owner goes out of scope, the value is dropped (freed)**
3. **Ownership can be moved or borrowed, but never duplicated**

The compiler enforces these rules at compile time. At runtime, there is:
- No GC thread
- No mark phase
- No sweep phase
- No reference counters
- No heap scanning
- No pauses

Memory is freed at the exact point it is no longer needed — deterministically, at the closing brace.

### Lifetimes: The Compile-Time GC

References (borrows) have lifetimes — the compiler tracks how long each reference lives and ensures no reference outlives its data:

```rust
fn longest<'a>(x: &'a str, y: &'a str) -> &'a str {
    if x.len() > y.len() { x } else { y }
}
```

The `'a` lifetime annotation tells the compiler: "the returned reference lives as long as both inputs." If you try to return a reference to a local variable, the compiler rejects it — at compile time, not at runtime.

```rust
fn bad() -> &str {
    let s = String::from("hello");
    &s  // compile error: s is dropped at end of function, reference would dangle
}
```

This is what a GC does at runtime (detect unreachable memory). Rust does it at compile time (detect impossible references). Zero runtime cost.

### The Tradeoff

| Aspect | GC languages | Rust |
|--------|-------------|------|
| Developer effort | Low — just allocate, GC handles cleanup | Higher — must think about ownership and lifetimes |
| Compile time | Fast | Slower (borrow checker analysis) |
| Runtime cost | GC pauses, heap scanning, memory overhead | Zero — deterministic drop at scope exit |
| Latency | Unpredictable (GC can pause anytime) | Predictable — no hidden pauses |
| Memory usage | 2x+ (garbage accumulates between collections) | Tight — freed immediately when unused |

Rust trades developer convenience for runtime performance. For systems software (databases, runtimes, web servers), this is the right trade.

## Fibers

### Why Other Languages Use Fibers

Go has goroutines. Erlang has processes. Java 21 has virtual threads. These are all fibers — lightweight user-space threads that the runtime schedules cooperatively (or semi-preemptively in Go's case).

They exist because these languages need to:
1. Handle millions of concurrent I/O tasks
2. Let developers write synchronous-looking code (`result = fetch(url)`) that blocks the fiber, not the OS thread
3. Manage scheduling without exposing the event loop

```go
// Go: goroutine blocks on I/O — runtime suspends it and runs another
go func() {
    resp, _ := http.Get("https://example.com")  // blocks this goroutine, not the thread
    fmt.Println(resp.Status)
}()
```

### Why Rust Does Not Use Fibers

**1. Fibers require a runtime that allocates stacks.**

Each fiber needs its own stack (Go starts at 2-8 KB, grows dynamically). This means:
- A heap allocation per fiber
- Stack overflow checks on every function call
- A runtime that manages stack growth and shrinking
- Memory overhead proportional to number of concurrent tasks

Rust's goal is zero-cost abstractions. Allocating stacks at runtime is a cost.

**2. Fibers are hard to optimize across FFI boundaries.**

Rust interoperates with C libraries extensively. Fibers with tiny stacks can't safely call into C code (which expects a full OS stack). Go solves this by switching to a system stack for cgo calls — adding complexity and overhead.

**3. Async/await achieves the same concurrency without stacks.**

Rust's async/await compiles each async function into a state machine — a regular struct stored inline, no heap allocation needed:

```rust
async fn fetch_article(title: &str) -> Article {
    let data = read_from_seg(title).await;    // suspend point 1
    let html = render_markdown(&data).await;  // suspend point 2
    Article { title, html }
}
```

The compiler transforms this into something like:

```rust
enum FetchArticle {
    Start { title: String },
    AfterRead { title: String, data: Vec<u8> },
    AfterRender { title: String, html: String },
    Done,
}
```

Each `.await` becomes a variant transition. The "stack" is just the live variables in the current variant — bytes, not kilobytes. No allocation, no stack, no runtime overhead.

### Fiber vs Async/Await: Memory Per Task

```
Go goroutine:       ~2,048 bytes minimum (stack)
Erlang process:     ~2,688 bytes minimum (stack + heap + mailbox)
Rust async task:    size_of::<FetchArticle>()  — often 32-128 bytes
```

For a million concurrent connections:
- Go: ~2 GB just for goroutine stacks
- Rust: ~128 MB for state machines (and often less, since the executor batches them)

### When Fibers Would Be Better

Fibers have one advantage: **deeply nested call stacks that suspend at arbitrary points.** If a function 20 calls deep needs to yield, a fiber just swaps the stack pointer. With async/await, every function in the chain must be `async` and every call must be `.await`ed — the "async infection" problem.

```
Fibers:     yield anywhere in the call stack — transparent to callers
Async:      yield only at .await points — every caller must be async
```

For database engines with complex query execution plans that suspend mid-evaluation, fibers are compelling. For an HTTP server that suspends at I/O boundaries, async/await is strictly better.

## How This Applies to writeonce

writeonce uses neither fibers nor async/await. It uses a plain event loop:

```rust
loop {
    events = epoll_wait();
    for event in events {
        handle(event);  // runs to completion, no suspension
    }
}
```

This is the simplest model — no GC, no fibers, no async state machines. Each handler reads from the `.seg` file, renders a template, writes to the socket, and returns. Nothing suspends mid-handler.

The memory model:

| What | How it's managed |
|------|-----------------|
| Article data in .seg | Owned by `Store`, freed when `Store` drops |
| Template ASTs | Owned by `TemplateRegistry`, live for the process lifetime |
| HTTP connections | Owned by `HashMap<Token, Connection>`, freed on close/hangup |
| Subscription table | Owned by `SubscriptionManager`, entries removed on `EPOLLHUP` |

No garbage. No fibers. No async. Just ownership, scopes, and the kernel's event notification. The Rust compiler guarantees at compile time that every allocation is freed exactly once, at exactly the right time.

## Summary

```
GC languages (Go, Java):   runtime scans heap → frees unreachable objects
                            cost: pauses, memory overhead, CPU overhead

Reference counting (Python): counter per object → free at zero
                             cost: per-operation overhead, cycle leaks

Rust ownership:             compiler tracks ownership → free at scope exit
                            cost: zero at runtime, developer thinks harder

Fibers (Go, Erlang):       runtime manages stacks → swap on yield
                            cost: stack allocation, stack checks, runtime

Async/await (Rust):         compiler generates state machines → no stack
                            cost: zero allocation, async must propagate

Event loop (writeonce):     no tasks, no suspension → handlers run to completion
                            cost: nothing — simplest possible model
```

Rust's answer to both GC and fibers is the same: **make the compiler do the work so the runtime doesn't have to.**
