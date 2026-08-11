# `wo-rt-c` — the writeonce runtime environment, in C

A single-file C implementation of the **runtime layer** the writeonce language runs on — now at **phase E: a durable RAM database**. Writes follow the dual-write order — RAM apply, framed WAL record (`len|crc32|payload|COMMIT`) to a per-shard `fallocate`'d log, one group-commit `fdatasync` per loop tick, **HTTP ack only after the fsync completion**. Boot performs the **first load, hard drive → RAM**: each shard replays its snapshot + WAL tail into its arena slice in parallel before any accept arms; clean shutdown snapshots each slice and truncates the WAL; a `meta` file pins the shard count so a mismatched `WO_THREADS` refuses to boot. `./wo-rt wal-check <file>` validates a log offline. `WO_THREADS` pinned threads (default = online cores), each owning its own raw io_uring ring (`io_uring_setup` + mmap'd SQ/CQ rings + `io_uring_enter` — **no liburing**), its own `SO_REUSEPORT` listener (multishot accept), its own keep-alive connections, and its own slice of the one mlock'd mmap arena — shared-nothing, no locks. Steady state is one `io_uring_enter` syscall per loop tick. Thread 0 owns the `signalfd`; shutdown broadcasts through per-thread `eventfd`s, both watched via `POLL_ADD` SQEs. **Zero dependencies beyond libc + kernel uapi headers.** The kernel is the runtime.

This is the runtime-layer sibling of [`prototypes/wo-db/`](../prototypes/wo-db/) (the C++ query-layer prototype): a reference card showing, with no abstraction in the way, exactly which kernel primitives the production Rust runtime (`crates/rt/`) drives through `libc`. Same role, different layer.

```
prototypes/wo-db/     C++   what the LANGUAGE executes   (parser, engine, transactions)
runtime/              C     what the RUNTIME stands on   (epoll, signalfd, sockets)
crates/rt/            Rust  the product — both layers, libc only
```

> **Moved from `prototypes/wo-rt-c/` to root-level `runtime/`** (monorepo layout, per the OOP compiler + VM design spec). `wo-rt.c` is untouched — it stays the event-loop reference described below. The **`wovm` bytecode VM now lives under `runtime/src/`** — see [The wovm bytecode VM](#the-wovm-bytecode-vm-runtimesrc) for what shipped and [Debugging](#debugging) for how to step through it.

## Build, run, poke

```bash
make                 # cc -O2 -Wall -Wextra -std=c11 -pthread — no libraries
./wo-rt              # 127.0.0.1:8085   (WO_PORT=9000 WO_THREADS=4 ./wo-rt to override)

curl localhost:8085/   # {"runtime":"wo-rt-c","loop":"epoll-et","threads":4,
                       #  "shard":2,"shard_requests":[68,36,44,53]}
curl -X POST localhost:8085/api/notes -d '{"title":"hello"}'
                       # {"id":2,"title":"hello","shard":1}   ← ids interleave per shard
curl localhost:8085/api/notes        # the connection's shard only — shared-nothing
# ctrl-C → signalfd on shard 0 → eventfd broadcast → all shards join
```

Each connection hashes to one shard for life (`SO_REUSEPORT` 4-tuple): a list may land on a different shard than the create that preceded it. That is the architecture, not a bug — cross-shard reads are a later phase / design decision (see the [architecture doc's improvements](../docs/plan/exploration/c-runtime/01-architecture.md)).

Or from the repo root: `just rt-c-demo`.

## Module map

Every block in `wo-rt.c` corresponds one-to-one to a module of the Rust runtime, which in turn mirrors Go's netpoller — the same lineage the docs trace:

| `wo-rt.c` block | Rust (`crates/rt/src/`) | Go (`.dev/reference/go/src/runtime/`) | Kernel reference card |
| --- | --- | --- | --- |
| `main` event loop (`epoll_create1` / `epoll_wait`, `EPOLLET`) | `runtime/netpoll_epoll.rs` | `netpoll_epoll.go` | [`linux/01-epoll.md`](../docs/plan/exploration/linux/01-epoll.md) |
| `sig_setup` (`sigprocmask` + `signalfd`) | `runtime/signalfd.rs` | signal mask handling | [`linux/04-signalfd.md`](../docs/plan/exploration/linux/04-signalfd.md) |
| `listener_bind` (`SOCK_NONBLOCK`, `accept4`-to-EAGAIN) | `http/listener.rs` | `net.Listen` + accept loop | `socket(7)` |
| `conn_drive` (read-to-EAGAIN, one buffer per fd) | `http/connection.rs` | `conn.Read` loop | the edge-triggered contract |
| `notes[]` store | `engine.rs` (BTreeMaps) | — | [`03-inmemory-engine.md`](../docs/runtime/database/03-inmemory-engine.md) |

## What it demonstrates

- **One thread owns each shard outright.** Accept, parse, store, respond — no locks, no worker pool, no connection migration. Scaling past one core is more shards ([`09-concurrency-scaleout.md`](../docs/plan/09-concurrency-scaleout.md)), never shared mutable state. The single cross-thread touch is the relaxed-atomic stats counters on `/` — monotonic, never on the data path.
- **Edge-triggered discipline.** Every registration sets `EPOLLET`; every readiness event is drained to `EAGAIN` (the accept loop and the read loop both). Get this wrong and connections silently hang — the reason the Rust module documents the same contract at the top of `netpoll_epoll.rs`.
- **Signals as fd events.** `SIGINT`/`SIGTERM` are blocked, then read from a `signalfd` on the same epoll — no async-signal-unsafe handler, no self-pipe trick.
- **RAM is the read path.** `GET /api/notes` touches a C array. The production engine is the same idea with MVCC and a WAL behind it.

## Architecture and roadmap

Documentation lives under `docs/` (repo convention) — this README stays here as the directory's orientation page only:

- [`docs/plan/exploration/c-runtime/01-architecture.md`](../docs/plan/exploration/c-runtime/01-architecture.md) — the runtime defined by tracing **one memory address** through user space, kernel space, and hardware under a million concurrent connections, plus seven improvement proposals (seqlock reads, registered buffers, zero-copy send, SQPOLL, …).
- [`docs/plan/exploration/c-runtime/00-plan.md`](../docs/plan/exploration/c-runtime/00-plan.md) — the phase sequence: **A → B → C → D → E → F, all ✅ shipped.**
- [`docs/plan/exploration/blue-green-vm/00-vision.md`](../docs/plan/exploration/blue-green-vm/00-vision.md) — where the runtime goes next: port-free transports, fibers, source embedded in the binary, agent-managed source over MCP, and the Blue/Green two-VM hot-swap deployment model.

## Measured (phase F, 20-core Linux 6.14, tmpfs data dir, `just rt-c-bench`)

Same C bench client (`bench/bench.c`, keep-alive, only 2xx counted) against both servers:

| Benchmark | **wo-rt-c** (8 shards, durable WAL, io_uring + group commit) | **Go `net/http`** (go1.25.1, 20 cores, no durability) | **Rust `wo`** (release, 8 shards, durable WAL, io_uring group commit)¹ |
| --- | --- | --- | --- |
| `GET /healthz` | **859,033 req/s** · p50 71 µs · p99 159 µs | 336,444 req/s · p50 70 µs · p99 1,277 µs | 746,340 req/s · p50 73 µs · p99 186 µs |
| `GET /` (JSON) | **671,312 req/s** · p99 180 µs | — | 692,671 req/s · p99 167 µs |
| `POST` write (tmpfs) | **618,343 commits/s** — fsync-acked · p99 194 µs | 320,516 req/s — RAM only, no WAL · p99 1,581 µs | 330,285 commits/s — fsync-acked · p99 360 µs |
| `POST` write (real ext4/NVMe) | — | — | **27,014 commits/s group commit vs 5,765 per-commit (4.7×)** · p50 2.2 ms |
| 10,000 idle conns | 0 errors | 0 errors | 0 errors |

¹ All numbers measured on a clean box with the same client (earlier parasite-contaminated runs superseded). The Rust column's history is the architecture roadmap, measured: 09a global mutex (74.9k writes/s) → 09b sharded engine (+51%) → 09c per-shard WAL (~1% durability cost) → **keep-alive: reads ×3.4 to 770k/s, durable writes ×1.9 to 331k/s, p99 under 350 µs everywhere**. Rust now beats Go on both columns *while fsyncing every write*, and sits within ~10% of the C prototype on reads — converging exactly as the same-architecture argument predicted. The one remaining C advantage is **group commit on io_uring** (one batched fsync + one syscall per tick vs per-commit fsync over epoll), which is the next port. Bonus finding: with `/tmp` accidentally full, the C runtime **refused to ack non-durable writes under ENOSPC** — the durability guarantee holding in an unplanned failure mode.

wo-rt-c on 8 cores outpaces Go on 20 with ~8× tighter p99 (Go's GC shows there) — while fsyncing every write Go doesn't. Honest caveats: `net/http` does full general-purpose HTTP; our parser is minimal; .NET was not installed on the box. **ACID under load:** three crash rounds (`kill -9` mid-bench at ~2M commits) all showed WAL records ≥ acked; isolation probe: 300 concurrent commits → 300 distinct ids; torn-tail records drop whole by CRC.

The crash-under-load test **found and fixed two real bugs** the lighter phase-D test missed: an ack-before-fsync race (`conn_continue` armed the send in the same tick the commit was staged) and an fd-reuse ABA hazard in ack parking (fixed with per-connection generation stamps). That is what phase F is for.
- [`docs/plan/exploration/c-runtime/02-single-binary.md`](../docs/plan/exploration/c-runtime/02-single-binary.md) — the end goal: how the `wo build` **single binary** runs on this runtime environment — the runtime kernel is statically linked into every writeonce app (Go model, nothing to install), with the catalog/routes/bytecode payload consumed at boot.

## Deliberate simplifications

Single-shot RECV re-armed per request (multishot recv + buffer rings are a phase-F improvement), one outstanding SQE per connection, fixed-size buffers, naive `"title"` extraction instead of a JSON parser, no `timerfd`. Requires kernel ≥ 5.19 (multishot accept). This file is for reading; `crates/rt` is for running writeonce.

## The wovm bytecode VM (runtime/src/)

Milestone 1 of the OOP track (spec: [`docs/superpowers/specs/2026-08-01-oop-compiler-vm-design.md`](../docs/superpowers/specs/2026-08-01-oop-compiler-vm-design.md), plan 1: [`docs/superpowers/plans/2026-08-01-wob-format-and-vm-core.md`](../docs/superpowers/plans/2026-08-01-wob-format-and-vm-core.md)) — a register VM that executes `.wob` bytecode (format: [`docs/plan/oop-vm/00-wob-format.md`](../docs/plan/oop-vm/00-wob-format.md)) with the full milestone-1 memory model. C11, libc only, same doctrine as `wo-rt.c`. `wovm` doesn't produce `.wob` itself — that's the OCaml `woc` front end's job ([`compiler/README.md`](../compiler/README.md), plan 3); this directory is the VM, not the compiler.

**Shipped features:**

- **Register interpreter** — fixed 32-bit instructions, Lua-style window-overlap calls (callee r0 = caller slot A), dual dispatch: computed goto under GNU C, `switch` under `-DWO_ISO_C` (both flavors gated in CI so neither rots).
- **Owned objects with a runtime borrow word** — shared-reader count / exclusive sentinel in every 16-byte header; violations trap `T_BORROW`. The compiler elides provable sites; the VM enforces the residual ones (hybrid model, spec §4).
- **`@gc` reference counting + budgeted cycle collection** — rc at zero frees immediately; possible cycles buffer as candidates (Bacon–Rajan trial deletion), collected in budgeted epochs per shard — no stop-the-world by construction. The `wovm` CLI pumps the collector to quiescence after the entry method returns (`WO_GC_BUDGET` steps per call, default 64; `WO_GC_TRACE=1` prints one stderr line per step) — scheduler-paced stepping between requests is sub-project 2, not this milestone.
- **Deterministic drops** — kind-directed drop plans (scalar/owned/gcref/text/multi/map), recursive over class fields and container elements.
- **Trap unwinding that never leaks** — per-method drop tables (pc → owned/gc register masks); a trap walks every frame and frees what was live; structured error `{code, method, line, message}` via line tables.
- **Validating loader** — bounds-checked parse, aligned copies, const-string interning, full static validation (opcodes, registers, indexes, jump targets, terminators, builtin arity, call windows, sorted vtables); what the loader accepts, the interpreter trusts — no UB on any input.
- **Structural interfaces** — `ICALL` binary-searches sorted (class, slot, method) vtable triples by receiver class.
- **Native containers + builtins** — `multi`/`map` with element-kind tags; `now/print/print_int/words/multi_*/map_*`; `DB_STUB` traps "engine not linked" until the DB engine binds (plan 5).
- **CLI contract** — `wovm app.wob`: exit 0 = ran; exit 1 = trap, one stderr line `trap CODE in METHOD at line N: MESSAGE`; exit 2 = usage/load failure. `WO_HEAP_MB` overrides the 64 MiB arena. Run with no `.wob` argument, `wovm` also checks its own trailer for an appended image (`woc build`'s single-binary output, [`docs/plan/oop-vm/00-wob-format.md`](../docs/plan/oop-vm/00-wob-format.md)'s "single-binary trailer" section) — a recognized-but-corrupt trailer fails clearly on exit 2, never a crash.

**File map:**

| File | What it is |
| --- | --- |
| `src/wob.h` | the `.wob` v1 contract: opcodes, field kinds, trap codes, builtin ids, limits, the 16-byte object header (machine-readable twin of the format doc) |
| `src/obj.h/.c` | arena allocator (16-byte size-class free lists ≤ 1024 B, malloc above), `wo_rt` context, object creation, strings |
| `src/borrow.h/.c` | the borrow word: acquire shared/exclusive, unconditional releases |
| `src/cont.h/.c` | native containers: growable `multi`, linear-scan `map` (content-compared text keys) |
| `src/gc.h/.c` | kind-directed drop dispatcher, rc inc/dec, cycle-candidate buffer, budgeted Bacon–Rajan collector |
| `src/loader.h/.c` | `.wob` parse + full static validation + mmap file path |
| `src/vm.h/.c` | the interpreter: dispatch, frames, traps, drop-map unwinding, `ICALL` |
| `src/builtin.h/.c` | builtin dispatcher |
| `src/main.c` | the `wovm` CLI: arg parsing, self-embedded-trailer detection, the post-exit gc pump |
| `test/t.h` | 20-line assert harness (no framework) |
| `test/wob_build.h/.c` | in-memory `.wob` assembler — the second, independent encoding of the format; builder/loader disagreements fail tests |
| `test/test_*.c` | 13 suites, one binary each, ASan+UBSan |
| `test/mkwob.c` | fixture generator for the CLI smoke |
| `test/cli_smoke.sh` | end-to-end exit-code/stderr-shape check |

**Gates:** `just wovm-build` · `just wovm-test` (unit suites + ISO flavor + CLI smoke, all ASan-clean) · in `runtime/`: `make test`, `make test-iso`, `make wovm`, `make wovm-asan` (sanitized binary for corpus fixtures that need a leak/UB proof, e.g. `tests/corpus/gc/`). Across both halves: `just oop-e2e` (the `woc` + `wovm` conformance corpus, [`compiler/README.md`](../compiler/README.md)) and `just oop-accept` (milestone 1's full acceptance gate — spec success criteria + both unit suites, one command).

## Debugging

VS Code: `.vscode/launch.json` ships four configs (needs the *C/C++* extension, `ms-vscode.cpptools`):

1. **wovm: debug hello.wob** — rebuilds `wovm` at `-O0 -g`, regenerates fixtures, breaks anywhere in the VM.
2. **wovm: debug a .wob file** — same, prompts for the image path.
3. **wovm: debug unit test** — pick one of the 13 ASan test binaries (already `-g`), step through it.
4. **wo-rt: debug server** — the event-loop reference at `-O0 -g`, `WO_THREADS=1` so one shard owns everything.

How to work on the VM under a debugger:

- **Step the ISO flavor, not the computed-goto one.** The goto interpreter jumps label-to-label and single-stepping is disorienting. Test binaries have an ISO twin (`build/iso_test_*`, plain `switch`) where `next`/`step` behave normally. For `wovm` itself, build `make -B wovm CFLAGS='-O0 -g -std=c11 -DWO_ISO_C'`.
- **`break vm_trap`** — one breakpoint catches every trap at the moment of failure, with the trapping frame intact (`vm->frames[vm->depth-1]`, `pc` already rewound to the faulting instruction). `vm_unwind` is the next frame down if you're chasing a leak-on-trap.
- **Other load-bearing breakpoints:** `wo_load_buf` (validation rejects), `recv_check` (residual field checks), `wo_builtin` (all builtins), `wo_gc_step` (cycle collection epochs).
- **ASan under gdb:** `ASAN_OPTIONS=abort_on_error=1` makes the first report SIGABRT so the debugger stops on it with the full stack; without gdb the report alone usually names the exact free you missed.
- **CLI knobs:** `WO_HEAP_MB=1 ./wovm app.wob` forces early `T_OOM` paths; exit codes 0/1/2 are stable for scripting.
- **gdb without VS Code:** `gdb --args ./wovm build/hello.wob`, or `gdb ./build/iso_test_unwind`.
