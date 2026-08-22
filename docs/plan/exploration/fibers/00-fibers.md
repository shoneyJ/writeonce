# Fibers — the concurrency model, its terminology, and why writeonce chose what it chose

> Exploration/reference note (no status banner by board convention).
> The normative decisions live in the arc spec
> ([`2026-08-20-shard-fiber-arc-design.md`](../../../superpowers/specs/2026-08-20-shard-fiber-arc-design.md))
> and iterations [8](../../../stories/language-runtime-database/done/08-shard-actor-runtime.md) /
> [11](../../../stories/language-runtime-database/done/11-fibers.md); this
> page explains the WHY at doctrine depth. Written 2026-08-20, when this
> file was also the target of a dangling reference from iteration 11 —
> it exists now.

> **Demonstrated live** by [`docs/examples/fibers`](../../../examples/fibers/README.md)
> (`just fibers`, 8 checks): byte-exact budget interleave, a parked
> sleeper blocking nobody, on the io_uring AND epoll backends.

## What a green thread is, and why anyone bothers

A green thread (fiber, virtual thread, coroutine — the tradition picks
the name) is a thread scheduled by the LANGUAGE RUNTIME, not the kernel.
The kernel sees a handful of OS threads; the runtime multiplexes
thousands-to-millions of logical threads on top.

The economics: an OS thread costs a fixed stack reservation (typically
1–8 MiB of address space) and every context switch goes through the
kernel scheduler (CFS is O(log N), and task counts in the hundreds
already hurt). A green thread can start around 2 KiB and switch in tens
of nanoseconds — a register save and a pointer swap, no syscall. That is
the difference between a few thousand concurrent connections and a few
million.

## The terminology, untangled

- **Green thread** — originally Java pre-1.3's model, which was M:1
  (everything on ONE OS thread — no parallelism at all). Now used
  loosely for any user-space thread.
- **Fiber** — usually implies COOPERATIVE: it runs until it explicitly
  yields.
- **Coroutine** — the broadest term; covers stackful and stackless both.
- **Goroutine** — Go's M:N implementation, preemptively scheduled.
- **Virtual thread** — Java 21's, also M:N.

The load-bearing distinction between "fiber" and "goroutine" is NOT the
stack — it is the EVICTION POLICY. A classic fiber owns the CPU until it
yields; one selfish loop starves everything behind it. A goroutine can
be interrupted WITHOUT ITS CONSENT (since Go 1.14, via a signal that
stops it at a safe instruction).

M:1 vs M:N matters concretely here: one process embedding its own
storage would leave every core but one idle under M:1. writeonce is M:N
— stage 2 of the arc pins one kernel thread per core (shards), and
fibers multiplex above them.

## writeonce's model: cooperative mechanics, accounting-based eviction

The BEAM shape. Mechanically cooperative — no signals, no interrupts,
no async preemption machinery — but every loop back-edge decrements a
REDUCTION BUDGET (`WO_REDUCTIONS`, default 4000), and at zero the fiber
is re-queued whether it likes it or not.

What that buys, all three at once:

- goroutine-grade starvation-freedom (the corpus pins budget-1 as EXACT
  round-robin across fibers — `runtime/test/test_fiber.c`);
- fiber-grade DETERMINISM: same program, same schedule, byte-identical
  output — signal-based preemption points are nondeterministic by
  nature, Go's included;
- zero interruption machinery: an interpreter's dispatch loop is always
  at a safe point, so the "where can I safely stop this thread"
  problem Go solves with signal handlers and pc maps costs nothing.

One implementation lesson worth keeping (it cost a livelock): the
budget check runs at loop back-edges AFTER the jump lands, so the saved
resume pc is the loop head. A pre-instruction save at budget 1
re-executes the jump straight into the same decrement, forever.

## Should the grammar have `async`? No — permanently rejected

`async` is only NECESSARY in stackless designs, where the compiler must
know statically which functions can suspend so it can transform each
into a state-machine struct. That requirement is function COLORING: an
async function can only be called from another async function, so the
keyword propagates virally up every call chain, and the ecosystem forks
into sync and async flavors of everything. In writeonce it would color
the query API, then every handler that touches data — the exact code
that most wants to read as straight-line logic.

With stackful fibers, a blocking read ten frames deep just parks. The
suspension point is a RUNTIME fact, not a TYPE-SYSTEM fact. Same source
text in program mode and server mode; no keyword; no coloring. This is
iteration 11's "async/await — permanently rejected surface, not
deferred", with the reasoning attached.

## Stackful vs stackless: the real fork, and the recommendation

**Stackful** (goroutines, Java virtual threads, classic fibers): each
logical thread owns a real, growable stack; it can suspend from ANY
depth. Costs: growth-by-copy must find and rewrite every pointer into
the old stack (Go's machinery), and FFI is the classic killer — C
callees assume a large contiguous stack and know nothing about growth,
so every foreign call needs a stack switch or a guaranteed segment.

**Stackless** (Rust futures, JS promises, C# async, Python coroutines):
the compiler rewrites each async function into a fixed-size state
object. Cheaper, runtime-optional — and it buys the coloring above,
plus suspension only at explicit await points.

**Recommendation, and what is already built: STACKFUL — because both of
its classic costs vanish in this stack.**

1. **No FFI, by doctrine.** The main argument against small growable
   stacks is C interop. writeonce has no C to call (the reject row is
   load-bearing). The cost is simply absent.
2. **The "stack" is register-indexed, not address-based.** A
   `wo_fiber`'s state is the interpreter's register window + frame
   array — frames reference registers by INDEX, so a context is
   relocatable by construction. Growth is allocate-larger + memcpy with
   ZERO pointer fixups; Go's hardest stackful problem does not exist
   here. Contexts are ~42 KiB fixed today; start-small (~4 KiB)
   growable contexts are the recorded improvement that takes fiber
   counts from thousands toward millions (arc spec, "Fiber context
   growth").

## The two problems every design must solve, and where they land here

- **Blocking syscalls.** A green thread calling fsync blocks the OS
  thread carrying it and strands every fiber behind it — and an
  embedded database fsyncs on every commit, milliseconds each, an
  eternity for a scheduler. writeonce's answer is io_uring, twice: the
  arc's per-shard ring parks blocking `net`/`time` builtins (T4,
  readiness ops first, then reads/writes as ring ops), and iteration
  23 rides the SAME ring for WAL WRITE+FSYNC group-commit so the
  DB-owner shard keeps executing while durability drains. epoll
  survives only as the portability fallback behind a startup probe
  (`WO_IO=uring|epoll`) — seccomp'd containers routinely deny io_uring,
  and the binary must run everywhere. The reference cards live in
  [`../linux/07-io_uring.md`](../linux/07-io_uring.md).
- **Preemption.** Solved by the reduction budget above. And to the
  standing question "does preemption depend on FFI?" — NO, inversely:
  preemption holes come from frames the runtime cannot interrupt, which
  in Go means foreign C frames (its signal preemption skips them). No
  FFI means no foreign frames; the only non-preemptible regions are our
  OWN builtins — C we control, bounded, and the blocking ones become
  parked ring ops. FFI would have CREATED the dependency; its absence
  is why the budget is airtight.

## Precedent survey (what was adopted, what was rejected)

- **BEAM (Erlang/Elixir): adopted** — reduction-budget preemption,
  actor mailboxes, one-message-at-a-time delivery, isolated failure
  (a fiber's uncaught trap kills that fiber alone).
- **Go: half-adopted** — M:N over pinned threads yes; stack-copy
  pointer rewriting unnecessary here (register-indexed contexts);
  signal-based async preemption rejected (nondeterministic, and the
  budget makes it redundant).
- **Rust/Tokio, JS, C#, Python: rejected** — stackless coloring is the
  cost this language exists to not pay.
- **Java Loom: matches** — park-under-a-blocking-API is exactly the
  stdlib posture (`net.read` blocks in program mode, parks under
  fibers, same signature).
