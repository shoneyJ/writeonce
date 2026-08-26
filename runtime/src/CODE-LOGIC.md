# `runtime/src` — how the VM is put together

Written 2026-08-14, when the runtime grew the systems stdlib and json; the
file table was brought back in line with `src/` on 2026-08-26 (`park.c` and
`crypto.c` had arrived with iterations 11/35 and 34 and were missing). Read
this before changing a file here; the normative contracts are
[`docs/plan/oop-vm/00-wob-format.md`](../../docs/plan/oop-vm/00-wob-format.md)
(the `.wob` format, opcodes, builtin ids) and
[`08-builtin-surface.md`](../../docs/plan/oop-vm/08-builtin-surface.md) (what
each builtin means in source terms). `wob.h` is the machine-readable twin of
the first: constants there and prose there must never disagree.

## The files, in dependency order

| file | what it owns |
| --- | --- |
| `wob.h` | every format constant: header offsets, field kinds, opcodes, builtin ids, trap codes, the 16-byte object header, the class descriptor |
| `obj.h/.c` | the per-shard arena, object allocation (traced instances link onto the traced list, born white — black mid-cycle), the per-class may-gcref fixpoint, `wo_str` (header + length + inline bytes, no NUL) |
| `cont.h/.c` | `multi` and `map` as native classes: struct heads in the arena, backing arrays malloc'd, map lookup a linear scan over parallel key/value arrays |
| `gc.h/.c` | the kind-directed dispatcher (`wo_drop_kind`/`wo_drop_obj`) for owned values, and the incremental tri-color mark-sweep for traced (inferred-gc) objects: per-shard traced list, snapshot-at-beginning roots, Yuasa deletion barrier (the `wo_drop_kind` GCREF case + `SETF`), budgeted mark and sweep slices (iteration 7b — RC and Bacon–Rajan are gone) |
| `borrow.h/.c` | the borrow word: shared counts and the exclusive sentinel |
| `loader.h/.c` | parse and **validate** an image; the validation contract in its header comment is exactly what the interpreter may then assume |
| `vm.h/.c` | the register interpreter: window-overlap calls, dual-flavor dispatch, traps, unwinding, catch frames |
| `builtin.h/.c` | the pure builtins: print, containers, text |
| `sysio.c` | the OS half: `fs`, `time`, `env`, `net`, `proc` |
| `json.c` | `json.encode` / `json.decode`, driven by class metadata |
| `park.c/.h` | the park plane (iteration 11 + 35, added after this doc was first written): the raw io_uring ABI mirrored from uapi with no liburing, the epoll fallback, fiber parking and unparking, and the per-call deadlines the `_dl` net members lower to. This is where a blocking builtin becomes "the shard runs someone else" |
| `crypto.c/.h` | SHA-1, SHA-256, HMAC-SHA256 (iteration 34, builtin ids 85–87): hand-rolled per the no-dependency doctrine, accepted against FIPS 180 / RFC 2202 / RFC 4231 vectors in `test/test_crypto.c` |
| `main.c` | the CLI: find an image (argument or embedded trailer), build argv, call the entry, map its result to an exit code; post-exit gc pump (a rootless cycle frees everything unreachable, in budgeted slices) |

`builtin.c`'s `wo_builtin` is the single entry point the interpreter calls; it
forwards ids at or above `WO_B_SYS_FIRST` to `sysio.c` and the json pair to
`json.c`. Splitting by translation unit keeps the kernel-touching code and the
format-walking code out of the hot builtin switch.

## Two invariants worth stating plainly

**The loader is the only validator.** Everything the interpreter skips
checking — opcode ranges, register operands, jump targets, builtin arities,
window sizes, table ordering — is checked once at load. The exceptions are
deliberate and documented: `GETF`/`SETF` field indexes and receiver shapes stay
runtime checks, because registers are untyped (the spec's residual-check
doctrine). If you add an opcode or a builtin, its validation goes in
`loader.c`'s switch and its arity in `b_arity`, or the interpreter is running
unvalidated bytes.

**Traps never leak.** A trap unwinds frames innermost-outward, and in each one
the drop-table entry governing that frame's current instruction says which
registers hold owned or counted values. The governing instruction is the
trapping pc for the innermost frame and the CALL (saved pc − 1) for every outer
one. Registers are nulled as they are released, so window overlap cannot
double-free.

## try/catch (the catch stack)

`TRY A sBx` pushes `{depth, handler pc, error register}`; `ENDTRY` pops it. On a
trap with a catch frame live, `vm_trap`:

1. fills `vm->caught` (the same structured error the uncaught surface prints),
2. unwinds every frame **above** the catching one, exactly as an uncaught trap
   would,
3. releases what the try region owned **in** the catching frame — the
   difference between the drop entry at the trapping instruction and the entry
   at the handler pc, which is why the compiler must record an entry at the
   handler,
4. points that frame at the handler and returns 0, so `TRAPF` reloads and keeps
   interpreting.

A frame that returns pops the catch frames it registered (`DROP_CATCHES`), so a
`return` out of a try region cannot leave a handler aimed at a dead window.
With `ncatch == 0` every trap behaves byte-for-byte as it did before the
feature existed — that is the property to preserve when touching this code.

## Records the VM fills but does not know

Three builtins return a *record*: `fs.stat`, `time.local`, `proc.run`, plus
`err_fill` for a catch arm. The VM cannot name a source type, so the compiler
passes the record's **class id** as the call's last argument and the builtin
fills fields by index. The field order is therefore a contract, written beside
each case in `sysio.c` and mirrored in `compiler/src/types.ml`'s predeclared
records. Change one side and the other silently writes to the wrong slot.

## Class metadata and json (`.wob` v2)

The class table carries, per field, its name constant, the class it refers to
(or a json-raw marker) and a container field's element kinds. That is what lets
`json.c` be one implementation for every shape instead of per-type generated
code:

- **encode** takes the top-level value's *static* kind from the compiler,
  because a register alone cannot say whether it holds an i64 or a pointer.
  Everything nested comes from object headers (which carry `class_id`) and the
  class table.
- **decode** parses and binds straight into the target class: keys matched
  against field names, a nested object built as that field's class, an array as
  a `multi` of that field's element kind, unknown keys skipped, absent keys left
  as the zero word (nil). Malformed input yields nil rather than trapping —
  that is what makes `json.decode(t) as T` a checked decode.

Two limits are inherent to the kind byte and are documented, not bugs to
discover: a `Bool` field encodes as `0`/`1`, and a fractional JSON number
decodes by truncation.

## Program mode

`main.c` accepts an entry taking no arguments or exactly one `multi Text`. The
list holds the program's **own** arguments — not the program name, and not the
image path a `wovm image.wob args...` invocation carries — so `args[0]` is the
first real argument. The entry's return value is the process exit code (low
byte); a trap is exit 1 with the fixed `trap N in METHOD at line L: MESSAGE`
line on stderr, which the conformance harness parses.

## Where to look when something breaks

- A wild pointer inside a builtin usually means the *compiler* put the wrong
  thing in a register: check the method's disassembly (`woc --dump-bc`) before
  suspecting the C.
- `make -C runtime wovm-asan` builds the sanitized binary; the unit suites
  (`just wovm-test`) run every `test/test_*.c` under ASan+UBSan in both
  dispatch flavors, so a fallback-only bug cannot hide.
- `runtime/test/wob_build.c` is an independent image assembler. A
  builder/loader disagreement shows up as a unit-test failure, which is the
  point of having two encoders.

## Fibers and actors (the 8+11 arc, stage 1 — 2026-08-20)

A `wo_fiber` is the interpreter state `wo_vm` used to hold inline (register
window, frame stack, catch stack, caught error); the vm keeps the module,
the runtime, the current-fiber pointer, and a FIFO run queue. The reduction
budget (`WO_REDUCTIONS`, default 4000) is checked ONLY at loop back-edges
and AFTER the jump lands — a pre-instruction save at budget 1 re-executes
the jump into the same decrement and livelocks (test_fiber pins budget 1
as exact round-robin). Main returning ends the program: every other fiber
unwinds through the drop maps (`fib_reap_all`); a spawned fiber's uncaught
trap kills that fiber alone.

An actor (`wo_actor`) is runtime-owned state + a receive method index + a
growable FIFO mailbox + at most ONE delivery fiber (one message at a time);
delivery re-queues per message so an actor never monopolizes the shard.
The runtime owns each message: it is dropped after its receive call
returns, and actor state / queued messages / the in-flight message are GC
roots scanned beside the fiber frames. spawn = BUILTIN 68 (instance +
receive's method index, compile-time constant); send = BUILTIN 69 (the
message is excluded from the emitter's fresh-arg drops — ownership moved).

## Float and Bytes (iteration 19 — `.wob` v5)

The registers did not change shape: a Float IS the register's 64 bits read as
an f64, converted only by `wo_f64`/`wo_bits` in `wob.h` (memcpy, so
strict-aliasing-clean and free at -O1). Nothing else in the runtime knows the
difference, which is why the change is opcodes and kind bytes rather than a
layout.

- **Two failure worlds.** `WOP_DIV` traps DIV0; `WOP_FDIV` never traps. That
  asymmetry is the contract, not an oversight — IEEE quiet semantics mean Inf
  and NaN flow instead of raising, so a compute-bound handler cannot be killed
  by data. `WOP_FNEG` flips the sign bit rather than subtracting from zero,
  which is the only way `-0.0` is reachable.
- **IEEE compares are not the index's order.** `FEQ`/`FLT`/`FLE` are IEEE
  (`NaN == NaN` is 0, `0.0 == -0.0` is 1). Indexes and `order by` need a total
  order instead, so `wo_float_cmp` (`wob.h`) sorts NaN last and treats the two
  zeros as equal, and `table.c`'s `idx_float_key` canonicalizes an index
  column's bits to match. Skip that canonicalization and a `unique` Float
  column accepts both `-0.0` and `0.0`, and a probe for one misses a row stored
  as the other — the bug this pairing exists to prevent.
- **A `?Float`'s nil is a reserved quiet NaN** (`WO_NIL_FLOAT`), not the zero
  word (`+0.0`) and not `WO_NIL_SCALAR` (whose bits are `-2.0`). Arithmetic
  produces the platform's canonical quiet NaN, so a computed NaN never reads as
  absence. Both json paths that write a nil word — the omitted-key prefill in
  `jparse_object` and the explicit `null` in `jparse_value` — must know this;
  either one alone leaves a `null` price reading back as zero.
- **Bytes is `wo_str` with a different `class_id`.** Same struct, same
  allocator, same free (`gc.c` handles both ids), so lifetime handling can
  never diverge. `WO_B_TEXT_COPY` preserves the id, which is what lets every
  existing copy-on-ownership-boundary serve both carriers; copying a Bytes as a
  Text would launder it into the wrong world, and the distinct id exists
  precisely to stop that.
- **One float renderer, three callers.** `wo_float_text` backs
  `float_to_text`, string interpolation, and `json.encode`. Shortest digits
  that reparse to the same BITS (bits, not `==`: `-0.0 == 0.0` is true, so a
  value comparison would let `0` stand in for `-0.0`), then fixed notation
  preferred over exponential in `1e-6 … 1e21` — pure "shortest" renders a
  price of 900.0 as `9e+02`.
- **The durability path never renders.** `wal.c` writes a Float as its raw
  word and a Bytes as the same length-prefixed blob a Text uses, so replay is
  bit-exact for NaN, ±Inf, and `-0.0`. `test_wal`'s `test_float_bytes_replay`
  asserts on bits for exactly that reason.

## The Int bitwise set (iteration 36 — `.wob` v6)

- **One shared case-body text serves both dispatch flavors** — the new
  CASE blocks sit in the Int neighborhood after LE, so `-DWO_ISO_C`
  cannot rot (same discipline as every opcode before them).
- **SHL shifts the unsigned register word** (wrapping, like ADD — a
  signed left-shift overflow would be UB); **SHR casts to int64_t
  first**, so it is ARITHMETIC — gcc/clang define signed `>>` as
  sign-extending, and those are the only compilers this runtime targets.
- **The count check is a trap, not a mask.** x86 masks the count mod 64,
  which would make `x << 64 == x` silently; Go saturates to 0/-1, spec
  surface for generic-width code this VM does not have. WO_T_SHIFT (12)
  follows the DIV0 precedent instead: named, catchable, honest. It can
  only fire on a count computed at run time — woc rejects literal
  out-of-range counts as WO-E223.
- **The loader validates the five opcodes as plain three-register forms**
  — the count is a register, not an immediate, so there is nothing to
  range-check at load time.

## The transparent DB actor (arc stage 3, 2026-08-21)

- **The database is an actor on shard 0.** A worker shard's DB builtin
  never touches an engine (its `rt.db` is NULL, asserted at serve entry):
  `wo_db_rpc` (vm.c) marshals the statement, ships it in a kind-3 envelope
  to the primary's inbox, and parks the fiber; the primary executes it
  serialized inside its inbox drain (`wo_vm_adopt` case 3, `wo_db_exec_req`)
  and ships the same request back as a kind-4 reply, which unparks the
  fiber; the builtin RE-EXECUTES and consumes the answer.
- **VM heaps never cross shards.** The requester ENCODES its argument
  values into engine slots on its own thread (`wo_db_val_encode`) — an
  owner-side read of a requester's Text would race that shard's collector
  writing header mark bits. Replies come back as plain ids (scan/probe), a
  deep-cloned engine value (get-field, decoded into the requester's arena),
  or a bare id (insert). Traps and messages are byte-identical to the local
  path; the ack crosses shards only after the owner's WAL commit.
- **The reply park holds no plane wait.** `WO_PARK_INBOX` (park_fd -2)
  joins the parked list only; the wake is `wo_io_unpark` from the envelope
  drain. Deadline scans key on park_fd == -1 EXACTLY — a -2 must never be
  read as a deadline. A busy shard adopts its inbox once per reduction
  slice, so a computing primary bounds a worker's DB latency to one slice.
- **Ring params are per-vm (`wo_vm.io_params`) — never share them.** They
  were one file static; a worker's lazy `wo_vm_init` memset+refilled it
  while another shard read ring offsets out of it, submits landed at
  garbage offsets, and parked fibers lost their wakes (~1/20 hangs at
  default cores, found by stage 3's cross-shard traffic). A short
  `io_uring_enter` submit is a failure, never a success — that check is
  what turns any relapse into a loud WO_T_IO instead of a silent hang.
- Proof: `just db-actor` (docs/examples/db-actor — multi-shard set ×3,
  both forced backends, single-shard byte-exact, WO_DATA replay pair);
  ASan/TSan clean on the RPC path.

## Net deadlines + the deadline tick (iteration 35)

- **`_dl` builtins (91–95) are per-call**: the fiber carries the absolute
  deadline (`dl_active`/`dl_at`) across the park protocol's re-execution;
  a timeout is the EXPECTED nil/false result, never a trap. `ms <= 0` is
  the pre-35 behavior bit for bit.
- **One op per fd-park stays the law.** Deadlines ride ONE per-shard
  TIMEOUT ("tick", sentinel user_data) armed for the nearest fd-park
  deadline; the post-CQE sweep wakes expired parks and POLL_REMOVE
  tombstones their poll. epoll needs no ops — its deadline scan grew the
  fd-park case. Full design + rejected alternatives:
  docs/superpowers/specs/2026-08-23-net-seams-park-design.md.
- **Fibers pool, never free mid-run** (`vm->fib_pool`): the loser of a
  readiness-vs-deadline race can complete one wait late, and its
  user_data must never point at freed memory. Worst case anywhere is a
  spurious wake, absorbed by re-execution. Pool dies with the vm;
  steady-state size = peak live fibers.
- **`listen_unix` sets O_NONBLOCK on the listener itself** — accept4's
  SOCK_NONBLOCK flags the ACCEPTED socket only; a blocking listener
  would block the whole shard (found by the seam probe, both backends).
