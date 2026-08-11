# The `.wob` format v1 — normative reference

> Copied verbatim from the normative section of
> [`docs/superpowers/plans/2026-08-01-wob-format-and-vm-core.md`](../../superpowers/plans/2026-08-01-wob-format-and-vm-core.md)
> (plan 1 of the approved spec
> [`2026-08-01-oop-compiler-vm-design.md`](../../superpowers/specs/2026-08-01-oop-compiler-vm-design.md)).
> The machine-readable twin is [`runtime/src/wob.h`](../../../runtime/src/wob.h) —
> constants there and prose here must never disagree. The test-side assembler
> `runtime/test/wob_build.c` is a second, independent encoding; builder/loader
> disagreements surface as test failures.

All integers little-endian; offsets are absolute file offsets.

**Header (44 bytes):** magic `"WOB1"`, version 1, then offset/count u32 pairs for the constant pool, class table, interface section, and method table, then a u32 entry-method index (all-ones = none).

**Constant pool** — sequential entries: one tag byte; tag 0 = i64 follows; tag 1 = text (u32 length + bytes, no NUL).

**Class table** — per class: name constant index, flags u32 (bit0 = instances are `@gc`), field count, then one kind byte per field padded to a 4-byte boundary. Field kinds: 0 SCALAR, 1 OWNED, 2 GCREF, 3 TEXT, 4 MULTI, 5 MAP. Runtime object layout: 16-byte header then one 8-byte slot per field, in declaration order.

**Interface section** — per interface: name constant index, method count. Global *slot ids* are assigned sequentially across interfaces in declaration order. Then a vtable row count and rows: class id, interface id, one method index per interface method.

**Method table** — per method: name constant index, class id (all-ones = free fn), arg count u8, register count u8, reserved u16, code length in bytes (multiple of 4), the u32 instructions, a line table (count + ascending pc→line pairs), and a drop table (count + ascending entries of pc, owned-register bitmask u64, gc-register bitmask u64). Drop-table lookup = last entry with pc ≤ current pc; no entry means nothing live.

**Instructions** — fixed 32-bit, Lua-style fields: opcode byte, A byte, then either B and C bytes or a 16-bit Bx (signed jumps encode as Bx − 32768).

| op | name | semantics (in words) |
| --- | --- | --- |
| 0 | NOP | nothing |
| 1 | LOADK A Bx | register A = constant Bx (int inline; text = pointer to interned const string) |
| 2 | MOVE A B | copy register; for owned values this IS the move — compiler guarantees the source is dead |
| 3–7 | ADD/SUB/MUL/DIV/NEG | i64 arithmetic, two's-complement wrapping (no signed-overflow UB); DIV traps on zero divisor and on INT64_MIN ÷ −1 |
| 8 | CONCAT A B C | new owned text from two texts |
| 9–12 | EQ/LT/LE/EQS | i64 compares and text-content equality, result 0/1 |
| 13–14 | JMP / JZ | relative jump (JZ when register A is zero) |
| 15 | CALL A Bx | call method Bx; callee's register window starts at caller base + A (register-window overlap, Lua-style); args sit at A, A+1, …; return value lands back in slot A |
| 16 | ICALL A Bx | interface call by global slot id Bx; receiver in A; vtable lookup by the receiver's class |
| 17–18 | RET A / RET0 | return value from register A (or zero), pop frame |
| 19 | NEW A Bx | new zeroed instance of class Bx |
| 20–21 | GETF / SETF | field read/write with runtime null/native/bounds checks (trap T_BOUNDS); overwriting a non-scalar field does NOT auto-drop the old value — the compiler emits the drop |
| 22 | DROP A | recursively drop the owned value in A per its class drop plan, null the register |
| 23–26 | BORROW_S/BORROW_X/RELEASE_S/RELEASE_X | borrow-word ops on the object in A; violation traps T_BORROW |
| 27–28 | RC_INC / RC_DEC | refcount ops on the `@gc` object in A |
| 29 | BUILTIN A B C | register A = builtin C applied to args starting at register B (fixed arity per builtin; `multi_new`/`map_new` carry kind immediates in B instead) |
| 30 | DB_STUB | trap T_DB "engine not linked" (spec: SQL-layer statements in milestone 1) |
| 31 | TRAP Bx | explicit trap with code Bx |

**Builtins:** now (ms), print (text), print_int, words (whitespace token count), multi_new/multi_push/multi_get/count/latest, map_new/map_set/map_get/map_has.

**Trap codes:** DIV0, BORROW, STACK, OOM, DB, BOUNDS, KEY, EXPLICIT.

## Single-binary trailer (`woc build`, plan 3 Task 6)

This section is **not part of the `.wob` format above** — `.wob` v1 is unchanged.
It documents the wrapper a *deployable executable* carries: `woc build <dir> -o
app` makes `app` by copying the `wovm` runtime binary and appending the
compiled `.wob` image plus a small fixed-size trailer. `wovm`'s own startup
(`runtime/src/main.c`) looks for this trailer in its own executable
(`/proc/self/exe`) before falling back to the classic `wovm file.wob` argv
contract, so the result runs standalone with no separate `.wob` file. Writer:
`compiler/bin/main.ml`. Reader: `runtime/src/main.c`'s `load_self_embedded`.
Append-based only, deliberately — no ELF section manipulation.

**Layout** — the trailer is the fixed **last 20 bytes** of the file, all
integers little-endian, found by seeking from the end (no scanning):

```
byte offset from EOF   size   field
  -20                   8     payload_off  -- absolute file offset where the embedded .wob image starts
  -12                   8     payload_len  -- length in bytes of the embedded .wob image
   -4                   4     magic        -- 0x31544257 ("WBT1" read as LE u32, mirrors WOB_MAGIC's "WOB1")

[ wovm runtime bytes (payload_off bytes) ][ .wob image (payload_len bytes) ][ trailer: payload_off | payload_len | magic ]
^ byte 0                                  ^ byte payload_off                ^ byte payload_off+payload_len == file_size-20
                                                                                                              file_size ^
```

**Reader algorithm** (`load_self_embedded`): open `/proc/self/exe`; if the
file is shorter than 20 bytes, or its last 4 bytes don't equal the magic,
there is no trailer — fall back to the argv `.wob` path unchanged. If the
magic matches, `payload_off` and `payload_len` are validated to account for
*every* trailing byte exactly (`payload_off + payload_len == file_size -
20`, checked via a bounds-safe subtraction so a corrupt/huge value can't
wrap the arithmetic and slip past); any mismatch is reported as a clear
"corrupt trailer" error (exit 2) rather than a crash or silent
misbehavior. On success, the executable is mmap'd and `wo_load_buf` parses
the embedded region exactly as `wo_load_file` parses a standalone `.wob`
today — argv is never consulted.

**Runtime location (writer side):** `--runtime <path>` wins when given;
otherwise the default is `runtime/wovm` resolved relative to the current
working directory (the same repo-root-relative assumption every other
`just`/build-tooling entry point in this repo already makes). A missing
runtime binary is a build-time error naming the recipe: `make -C runtime
wovm`. `woc build` never invokes or inspects the runtime binary beyond
reading its bytes — it does not need to be executable *as run by woc*, only
as run by whoever runs the produced artifact.

**Edge cases decided for `woc build`** (each implemented deliberately, not
left to fall out accidentally):

- **Output path already exists:** overwritten, but atomically — the new
  binary is assembled in a temp file (`<out>.woc-build.tmp`, freshly
  created with mode `0755` each time so a stale temp file's permissions
  can never leak through) next to `-o`, then renamed over it. A failed
  build (bad compile, missing runtime, disk-full mid-write) never
  clobbers a previously-working binary with a partial one.
- **A directory with no `main`:** unlike `--emit` (where a `.wob` with no
  entry method is a legitimate, already-specified artifact), `build`'s
  entire purpose is something runnable, so a clean compile with no
  zero-argument free fn named `main` is a **build-time error, no output
  written** — not deferred to `wovm`'s own "module has no entry method"
  message at run time. Detected by reading the compiled image's own
  entry field (`WOB_OFF_ENTRY`, offset 40) rather than plumbing a new
  return value through the emitter.
- **`--runtime` itself already carries a trailer** (rebuilding from a
  previously-built single binary): its embedded payload is *stripped*
  before copying — the writer recognizes its own trailer on the input
  runtime binary the same way the C reader does, and keeps only the
  pristine runtime prefix (`payload_off` bytes). This makes `woc build
  ... --runtime already-built-app -o new-app` produce a binary
  byte-identical in size to building fresh from `runtime/wovm` directly,
  instead of chaining stale payloads and bloating on every rebuild. Any
  input that doesn't unambiguously look like our own trailer (wrong
  magic, or offsets that don't exactly account for every trailing byte)
  is left untouched and copied as-is — the safe default when it's not
  certain.
