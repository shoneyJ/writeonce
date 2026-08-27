# `runtime/` — `wovm`, the writeonce bytecode VM

The C11 register VM that loads and runs `.wob` images. Sibling of the OCaml
`woc` compiler ([`compiler/README.md`](../compiler/README.md)): `woc` emits the
image, `wovm` executes it, and `woc build` appends an image to a copy of this
binary to produce one self-contained executable. libc only, direct syscalls, no
libraries.

The embedded database engine ([`database/`](../database/src/CODE-LOGIC.md)) is
statically linked into every `wovm` and every test binary — one binary, no
separate database process.

Reasoning under the code: [`src/CODE-LOGIC.md`](src/CODE-LOGIC.md). Normative
contracts: [`docs/plan/oop-vm/00-wob-format.md`](../docs/plan/oop-vm/00-wob-format.md)
(format, opcodes, builtin ids — `src/wob.h` is its machine-readable twin) and
[`08-builtin-surface.md`](../docs/plan/oop-vm/08-builtin-surface.md) (what each
builtin means in source terms).

> `wo-rt.c` in this directory is **not** part of that toolchain. It is the
> retired io_uring event-loop reference prototype the runtime's design was read
> off, kept for reading. See the "Historical" section at the bottom of this page
> — nothing in the shipped build compiles it.

## Build, test

```bash
just wovm-build     # -> runtime/wovm (the release binary)
just wovm-test      # unit suites, both dispatch flavors, + CLI smoke, ASan+UBSan

# in runtime/ directly:
make wovm           # the release binary
make test           # unit suites, computed-goto dispatch
make test-iso       # the same suites under -DWO_ISO_C (plain switch)
make wovm-asan      # a separate sanitized binary, for corpus fixtures needing a leak/UB proof
make wovm-tsan      # thread-sanitized, for the fiber/actor demos
```

Across both halves of the toolchain: `just oop-e2e` (the `woc` + `wovm`
conformance corpus) and `just oop-accept` (the full milestone gate — compile-time
budget, corpus under ASan, single-binary smoke, both unit suites, one command).

## Shipped features

- **Register interpreter** — fixed 32-bit instructions, Lua-style window-overlap
  calls (callee r0 = caller slot A), dual dispatch: computed goto under GNU C,
  `switch` under `-DWO_ISO_C`. Both flavors are gated so neither rots.
- **Owned objects with a runtime borrow word** — shared-reader count /
  exclusive sentinel in every 16-byte header; violations trap `T_BORROW`. The
  compiler elides provable sites; the VM enforces the residual ones.
- **Inferred GC, incremental tri-color mark-sweep** — GC-ness is a compiler
  inference, never an annotation (`@gc` is rejected outright, WO-E104). Per-shard
  traced list, snapshot-at-beginning roots, Yuasa deletion barrier, budgeted mark
  and sweep slices — no stop-the-world by construction. Reference counting and
  the Bacon–Rajan trial-deletion collector that shipped in iteration 2 were both
  **removed** by iteration 7b. `wovm` pumps the collector to quiescence after the
  entry returns (`WO_GC_BUDGET` steps per call, default 64; `WO_GC_TRACE=1`
  prints one stderr line per step).
- **Deterministic drops** — kind-directed drop plans (scalar/owned/gcref/text/
  bytes/float/multi/map), recursive over class fields and container elements.
- **Trap unwinding that never leaks** — per-method drop tables (pc → owned/gc
  register masks); a trap walks every frame and frees what was live; structured
  error `{code, line, method, msg}` via line tables, catchable with `try`/`catch`.
- **Validating loader** — bounds-checked parse, aligned copies, const-string
  interning, full static validation (opcodes, registers, indexes, jump targets,
  terminators, builtin arity, call windows, sorted vtables). What the loader
  accepts, the interpreter trusts — no UB on any input.
- **Structural interfaces** — `ICALL` binary-searches sorted (class, slot,
  method) vtable triples by receiver class.
- **Fibers and shard actors** — reduction-budget preemption, pinned per-core
  shards, ownership-move message sends, bounded mailboxes (`WO_MAILBOX`, default
  cap 1024) with a catchable `WO_T_ACTOR` trap on overflow, `call` parking the
  caller for a typed scalar reply, and actor death that traps callers rather than
  hanging them. Blocking stdlib calls park the fiber; the shard runs someone else.
- **The systems stdlib and the engine** — six module namespaces (`fs`, `time`,
  `env`, `net`, `proc`, `json`) plus the free builtins: text and containers, the
  `Float`/`Bytes` bridges, base64, and the digests `sha1`/`sha256`/`hmac_sha256`.
  `WO_B_MAX` is 95. Database builtins reach the linked engine directly; the
  legacy `DB_STUB` opcode survives only for images emitted against no engine.
- **CLI contract** — `wovm app.wob`: exit 0 = ran; exit 1 = trap, one stderr
  line `trap CODE in METHOD at line N: MESSAGE`; exit 2 = usage/load failure.
  `wovm --version` prints `wovm <VERSION>`. `WO_HEAP_MB` overrides the 64 MiB
  arena; `WO_DATA` opts into durability; `WO_SHARDS` sets the shard count. Run
  with no `.wob` argument, `wovm` checks its own trailer for an appended image
  (`woc build`'s single-binary output) — a recognized-but-corrupt trailer fails
  clearly on exit 2, never a crash.

Interpreter ceilings, all in `src/wob.h`: 64 registers per frame
(`WO_MAX_REGS`), a 4096-slot value stack (`WO_STACK_SLOTS`), 256 frames
(`WO_MAX_FRAMES`), 64 shards (`WO_MAX_SHARDS`). The image format is at
`WOB_VERSION 6` (iteration 36's bitwise opcodes 42–46 moved it last).

## File map

| File | What it is |
| --- | --- |
| `src/wob.h` | the `.wob` contract: opcodes, field kinds, trap codes, builtin ids, limits, the 16-byte object header |
| `src/obj.h/.c` | arena allocator (16-byte size-class free lists ≤ 1024 B, malloc above), `wo_rt` context, object creation, `wo_str` |
| `src/borrow.h/.c` | the borrow word: acquire shared/exclusive, unconditional releases |
| `src/cont.h/.c` | native containers: growable `multi`, **linear-scan** `map` (content-compared text keys) — deliberate KISS, and O(n) per lookup |
| `src/gc.h/.c` | the kind-directed drop dispatcher plus the incremental tri-color mark-sweep for traced objects |
| `src/loader.h/.c` | `.wob` parse + full static validation + the mmap file path |
| `src/vm.h/.c` | the interpreter: dispatch, frames, traps, drop-map unwinding, `ICALL`, shard/engine startup |
| `src/builtin.h/.c` | the builtin dispatcher — the single entry point the interpreter calls, forwarding to `sysio.c` and `json.c` |
| `src/sysio.c` | the OS half: `fs`, `time`, `env`, `net`, `proc` |
| `src/json.c` | `json.encode` / `json.decode`, driven by class metadata |
| `src/crypto.c/.h` | SHA-1, SHA-256, HMAC-SHA256 (iteration 34), vector-verified |
| `src/park.c/.h` | the park plane: raw io_uring ABI (no liburing) and the epoll fallback, fiber parking, per-call deadlines |
| `src/main.c` | the `wovm` CLI: find an image (argument or embedded trailer), build argv, call the entry, map its result to an exit code, pump the collector |
| `test/t.h` | 20-line assert harness (no framework) |
| `test/wob_build.h/.c` | in-memory `.wob` assembler — a second, independent encoding of the format, so builder/loader disagreements fail tests |
| `test/test_*.c` | 18 suites, one binary each, ASan+UBSan: arena, borrow, builtin, cont, crypto, cycle, fiber, icall, loader, mailbox, obj, objops, rc, table, unwind, vm, wal, wobbuild |
| `test/mkwob.c` | fixture generator for the CLI smoke |
| `test/cli_smoke.sh` | end-to-end exit-code/stderr-shape check |

Note where io_uring is and is not: `src/park.c` drives it for the fiber and
network plane, while the WAL commit path in `database/src/wal.c` is still a plain
per-commit `fdatasync`. Moving the WAL onto the rings is iteration 23.

## Debugging

VS Code: `.vscode/launch.json` ships four configs (needs the *C/C++* extension,
`ms-vscode.cpptools`):

1. **wovm: debug hello.wob** — rebuilds `wovm` at `-O0 -g`, regenerates
   fixtures, breaks anywhere in the VM.
2. **wovm: debug a .wob file** — same, prompts for the image path.
3. **wovm: debug unit test (ISO dispatch)** — pick one of the ASan test
   binaries (already `-g`), step through it.
4. **wo-rt: debug server (1 shard)** — the retired event-loop reference at
   `-O0 -g`, `WO_THREADS=1` so one shard owns everything.

How to work on the VM under a debugger:

- **Step the ISO flavor, not the computed-goto one.** The goto interpreter jumps
  label-to-label and single-stepping is disorienting. Test binaries have an ISO
  twin (`build/iso_test_*`, plain `switch`) where `next`/`step` behave normally.
  For `wovm` itself, build
  `make -B wovm CFLAGS='-O0 -g -std=c11 -DWO_ISO_C'`.
- **`break vm_trap`** — one breakpoint catches every trap at the moment of
  failure, with the trapping frame intact (`vm->frames[vm->depth-1]`, `pc`
  already rewound to the faulting instruction). `vm_unwind` is the next frame
  down if you're chasing a leak-on-trap.
- **Other load-bearing breakpoints:** `wo_load_buf` (validation rejects),
  `recv_check` (residual field checks), `wo_builtin` (all builtins), `wo_gc_step`
  (mark-sweep slices).
- **ASan under gdb:** `ASAN_OPTIONS=abort_on_error=1` makes the first report
  SIGABRT so the debugger stops on it with the full stack; without gdb the report
  alone usually names the exact free you missed.
- **CLI knobs:** `WO_HEAP_MB=1 ./wovm app.wob` forces early `T_OOM` paths; exit
  codes 0/1/2 are stable for scripting.
- **gdb without VS Code:** `gdb --args ./wovm build/hello.wob`, or
  `gdb ./build/iso_test_unwind`.

---

## Historical: `wo-rt.c`

A single-file C server that was the **runtime-layer reference prototype** —
built to find out which kernel primitives a writeonce runtime should stand on,
by writing them with no abstraction in the way. Its design conclusions are what
`src/park.c` and the shard model implement. It is not part of the toolchain, no
gate builds it, and it shares no code with `wovm`.

It reached phase F of its own plan
([`docs/plan/exploration/c-runtime/00-plan.md`](../docs/plan/exploration/c-runtime/00-plan.md),
phases A→F all shipped): `WO_THREADS` pinned threads, each owning a raw io_uring
ring (`io_uring_setup` + mmap'd SQ/CQ rings + `io_uring_enter`, no liburing), its
own `SO_REUSEPORT` listener with multishot accept, its own keep-alive
connections, and its own slice of one mlock'd mmap arena — shared-nothing, no
locks, one `io_uring_enter` per loop tick in steady state. Writes followed the
dual-write order: RAM apply, framed WAL record to a per-shard `fallocate`'d log,
one group-commit `fdatasync` per tick, **HTTP ack only after the fsync
completes**. Boot replayed each shard's snapshot + WAL tail in parallel before
any accept armed; `./wo-rt wal-check <file>` validated a log offline.

Build and poke it, if you want to read it running:

```bash
make -C runtime wo-rt     # cc -O2 -Wall -Wextra -std=c11 -pthread, no libraries
./runtime/wo-rt           # 127.0.0.1:8085 (WO_PORT=9000 WO_THREADS=4 to override)
```

Its measured numbers, kept as the historical record they are — bench client
`bench/bench.c` (keep-alive, only 2xx counted), Go reference in `bench/goref/`,
20-core Linux 6.14, tmpfs data dir. These are **not** `wovm` numbers; the
shipped VM's measurements live in `bench/baseline.json` and
[`docs/plan/perf-targets.md`](../docs/plan/perf-targets.md).

| Benchmark | **wo-rt-c** (8 shards, durable WAL, io_uring group commit) | **Go `net/http`** (go1.25.1, 20 cores, no durability) |
| --- | --- | --- |
| `GET /healthz` | **859,033 req/s** · p50 71 µs · p99 159 µs | 336,444 req/s · p50 70 µs · p99 1,277 µs |
| `GET /` (JSON) | **671,312 req/s** · p99 180 µs | — |
| `POST` write (tmpfs) | **618,343 commits/s** — fsync-acked · p99 194 µs | 320,516 req/s — RAM only, no WAL · p99 1,581 µs |
| 10,000 idle conns | 0 errors | 0 errors |

Honest caveats as recorded then: `net/http` does full general-purpose HTTP and
this parser was minimal; tmpfs makes fsync nearly free, so the durable column
flatters itself. ACID probes: three `kill -9` rounds mid-bench at ~2M commits all
showed WAL records ≥ acked; 300 concurrent commits → 300 distinct ids; torn-tail
records dropped whole by CRC. The crash-under-load test found two real bugs the
lighter phase-D test missed — an ack-before-fsync race and an fd-reuse ABA hazard
in ack parking — which is what the phase existed for.

Deliberate simplifications it never outgrew: single-shot RECV re-armed per
request, one outstanding SQE per connection, fixed-size buffers, naive `"title"`
extraction instead of a JSON parser, no `timerfd`. Requires kernel ≥ 5.19
(multishot accept).

Design docs it fed:
[`c-runtime/01-architecture.md`](../docs/plan/exploration/c-runtime/01-architecture.md)
(the runtime traced through one memory address, plus seven improvement
proposals),
[`c-runtime/02-single-binary.md`](../docs/plan/exploration/c-runtime/02-single-binary.md)
(how a single binary runs on this runtime),
[`blue-green-vm/00-vision.md`](../docs/plan/exploration/blue-green-vm/00-vision.md)
(port-free transports, fibers, embedded source, two-VM hot swap), and the
kernel-primitive cards under
[`exploration/linux/`](../docs/plan/exploration/linux/00-linux.md).
