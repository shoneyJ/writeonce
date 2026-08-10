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
