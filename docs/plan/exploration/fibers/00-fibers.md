# Fibers — green threads on the wovm shard scheduler

> Research note (2026-08-08) feeding story iteration 11. Expands
> [the blue-green vision §3](../blue-green-vm/00-vision.md) with the
> kernel's-eye evidence and the precedent survey. Prose only; the spec and
> plan follow the brainstorming → writing-plans path when the iteration
> starts.

## Why the kernel cannot do this for us

Reference: [Threads and the OS kernel's view](https://learn.padho.ai/wiki/threads-and-the-os-kernels-view).
The facts that matter, condensed:

- A Linux thread IS a `task_struct`: own TID, own kernel stack (8–16 KiB),
  own scheduling slot; `clone()` flags decide what is shared. There is no
  cheaper kernel thread to ask for.
- Costs per thread: ~8 MiB stack VMA, ~9 KiB `task_struct`,
  ~20 µs creation, 1–3 µs per context switch. Measured against a userspace
  runtime spawn (~58 ns): **~342× creation cost, ~70× memory**.
- CFS keys a red-black tree by `vruntime` — O(log N) per pick. At tens of
  thousands of runnable tasks the scheduling slice approaches the
  context-switch cost and the kernel scheduler becomes the bottleneck
  before application code does.
- M:N threading died *in the kernel* (NPTL, 2003) and was reborn in
  userspace (goroutines, BEAM processes, Tokio tasks, Loom virtual
  threads) — because only the runtime knows its own blocking points and
  can keep per-task state tiny.

Conclusion the industry already reached and we adopt: **one kernel task
per core** (plan 09 / iteration 8's pinned shard — already doctrine),
**userspace tasks above it**. The kernel schedules cores; the runtime
schedules work.

## Precedent survey — who has green threads, and how

| Runtime | Task state | Preemption | I/O integration | Lesson for wovm |
| --- | --- | --- | --- | --- |
| **Erlang/BEAM** | interpreter state per process, private heap | **reduction budget** (~2k reductions, checked at calls) | park on scheduler, poll set | the closest shape: interpreter = scheduler; deterministic, signal-free preemption |
| **Go** | native stack, 8 KiB grown by copy | async preemption via signals (since 1.14) + safepoints | netpoller parks goroutines | stack copying needs precise pointer maps — heavy machinery; signals are what the vision explicitly avoids |
| **Java Loom** | continuation frames on heap, unmounted from carrier | cooperative at yield points | blocking calls in the JDK park the virtual thread | "same blocking API, runtime parks underneath" — exactly our stdlib posture |
| **Tokio/Rust** | stackless state machines (`async fn`) | cooperative at `.await` | reactor + waker | rejected surface: writeonce has **no async/await keyword** (systems-track spec Part 2); function coloring is the disease |
| **Lua** | coroutine = own Lua stack (interpreter state) | none (pure cooperative) | up to the host | proof that interpreter-state fibers are nearly free; but no preemption = one hot loop starves the shard |
| **boost.context / libco** | native stack + hand-rolled register switch | none | none | what we do NOT need: wovm executes bytecode, so no native stack switching, no asm, no guard pages |

## The wovm design (vision §3, confirmed by this survey)

A fiber is **the execution state the VM already isolates**: register-window
stack + frame stack + pc. Making that per-fiber instead of per-VM turns the
interpreter into a scheduler almost for free — the BEAM/Lua insight, minus
Lua's starvation problem:

- **Preemption by reduction budget** — the dispatch loop decrements a
  counter per instruction (or per call/back-edge); at zero the fiber parks
  and the next runnable one is picked. No signals, no safepoint asm, no
  stack copying; deterministic and debuggable. (Erlang has run this design
  in telecom production for three decades.)
- **Park on I/O** — a blocking stdlib builtin on a server shard hands its
  fd to the shard's epoll/io_uring loop and parks the fiber; the completion
  resumes it. Same typed builtins, blocking look, no coloring — the
  systems-track "one API, two execution disciplines" doctrine gains its
  third discipline: program mode blocks the thread, server shards park the
  fiber, the source text is identical.
- **Ownership fits** — a fiber owns its objects like a shard owns its heap;
  cross-fiber sends move ownership exactly like iteration 8's cross-shard
  sends (same rule, cheaper path: same heap, no copy). `@gc` references
  stay shard-local either way, so the per-shard cycle collector (staged to
  iteration 8) needs fiber stacks as additional roots — the one real
  collector interaction to spec.
- **Cost target** — fiber creation is an arena allocation of a small
  context (hundreds of bytes, the ~58 ns class), park/resume is a pointer
  swap in the dispatch loop; thousands of fibers per shard where the
  kernel tops out at hundreds of threads per core.

## What must be specified before implementation (open questions)

1. Surface: `spawn` returns what — a fiber handle, an actor address, or
   nothing (fire-and-forget)? Iteration 8's `spawn` and mailbox surface
   should be the same word; fibers refine its granularity.
2. Reduction budget size and where it is checked (per instruction vs per
   call/back-edge) — measure both in the interpreter before choosing.
3. Parked-fiber lifetime: what drops a fiber blocked forever (shard
   shutdown, blue-green drain)? Unwinding a parked fiber must run its drop
   maps — same machinery as trap unwind (OOP spec §6).
4. Fairness: run queue is FIFO per shard in v1; priorities/timers are a
   later capability (the recipe-box rule — no framework policy in the
   runtime).
5. Program mode: stays fiber-free in v1 (one thread, blocking legal) or
   gains the same scheduler? Default: fiber-free — log-watcher needs none.

## Doctrine check

Kernel primitives only (epoll/io_uring already ours; no new syscalls
needed) · no signals · no async/await keyword · ownership moves, never
shares · per-shard everything (heap, GC, now run queue) · plain
diagnostics (a starved-fiber warning names the hot function via the line
table). No principle bends.
