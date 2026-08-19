# The `.wob` format v2 — normative reference

> Copied verbatim from the normative section of
> [`docs/superpowers/plans/2026-08-01-wob-format-and-vm-core.md`](../../superpowers/plans/2026-08-01-wob-format-and-vm-core.md)
> (plan 1 of the approved spec
> [`2026-08-01-oop-compiler-vm-design.md`](../../superpowers/specs/2026-08-01-oop-compiler-vm-design.md)).
> The machine-readable twin is [`runtime/src/wob.h`](../../../runtime/src/wob.h) —
> constants there and prose here must never disagree. The test-side assembler
> `runtime/test/wob_build.c` is a second, independent encoding; builder/loader
> disagreements surface as test failures.

All integers little-endian; offsets are absolute file offsets.

**Header (44 bytes):** magic `"WOB1"`, version 4, then offset/count u32 pairs for the constant pool, class table, interface section, and method table, then a u32 entry-method index (all-ones = none).

**Constant pool** — sequential entries: one tag byte; tag 0 = i64 follows; tag 1 = text (u32 length + bytes, no NUL).

**Class table** — per class: name constant index, flags u32 (bit0 = instances are `@gc`), field count, then one kind byte per field padded to a 4-byte boundary, then **three u32 arrays of per-field metadata** (v2), one entry per field each, in declaration order:

1. `field_names[i]` — constant index of the field's name, or all-ones for "not recorded" (what a hand-built test image writes).
2. `field_class[i]` — the class id the field refers to: its own class for an OWNED/GCREF field, its *element's* class for a container of records; `0xFFFFFFFE` marks a `json.Value` field, whose Text holds a raw JSON slice; all-ones for none.
3. `field_elem[i]` — a container field's element kinds: a MULTI's element kind, or a MAP's key kind in the low nibble and value kind in the next; 0 otherwise.

Field kinds: 0 SCALAR, 1 OWNED, 2 GCREF, 3 TEXT, 4 MULTI, 5 MAP. Runtime object layout: 16-byte header then one 8-byte slot per field, in declaration order.

The metadata exists for exactly one reason: `json.encode`/`json.decode` are runtime services driven by class metadata (`runtime/src/json.c`) rather than per-type generated code, so the names a JSON object needs and the shapes a decode has to build must live in the image. Every other part of the runtime ignores it.

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
| 27–28 | *reserved* | were RC_INC/RC_DEC; retired with reference counting in v4 (iteration 7b) — the loader rejects them like any unknown opcode |
| 29 | BUILTIN A B C | register A = builtin C applied to args starting at register B (fixed arity per builtin; `multi_new`/`map_new` carry kind immediates in B instead) |
| 30 | DB_STUB | trap T_DB "engine not linked" (spec: SQL-layer statements in milestone 1) |
| 31 | TRAP Bx | explicit trap with code Bx |
| 32 | TRY A sBx | push a catch frame for this frame and window: handler at pc + sBx, error record register A (haxe-parity Task 5) |
| 33 | ENDTRY | pop the innermost catch frame — the try region completed without trapping |

**try/catch (Task 5).** A trap raised while a catch frame is live unwinds every frame *above* the catching one exactly as an uncaught trap does (drop maps run, registers null), then releases what the try region owned in the catching frame — the difference between the drop entry at the trapping instruction and the one at the handler pc — and resumes at the handler instead of leaving the VM. A frame that returns takes its still-open catch frames with it, so a `return` out of a try region cannot leave a handler pointing at a dead window. With no catch frame live, a trap behaves byte-for-byte as it did before v2. The catch arm's error record is an ordinary compiler-allocated object filled by the `err_fill` builtin (field order: 0 code, 1 line, 2 method, 3 msg).

**Builtins:** now (ms), print (text), print_int, words (whitespace token count), multi_new/multi_push/multi_get/count/latest, map_new/map_set/map_get/map_has, int_to_text (haxe-parity Task 2), variant_tag (haxe-parity Task 4 — see "Enum payload variants" below), err_fill (Task 5's catch record), then the systems stdlib:

- **text/containers** — len, byte_at, print_err, starts_with, ends_with, index_of, last_index_of, substr, trim, to_lower, char_of, parse_int, split, split_ws, join, slice, pop, shift, sort, reverse, remove, key_at, val_at, multi_set. Ids 16–39; `runtime/src/builtin.c`.
- **the OS half** — fs.exists/list/stat/read_all/read_at/append, time.sleep/local/iso, env.get/stopping, net.listen/accept/read/write/close, proc.run. Ids 40–56; `runtime/src/sysio.c`. A member that returns a record takes that record's **class id as its last argument**, so the VM allocates what it fills without knowing any source type name.
- **json** — encode (value + the value's static kind), decode (text + the class id to build). Ids 57–58; `runtime/src/json.c`. Decode yields the zero word on malformed input rather than trapping, which is what makes `json.decode(t) as T` a checked decode.
- **59 `map_get_opt`** (`m[k]`'s optional read), **60 `text_copy`** (Text's ownership-boundary copy — Task 1 of the executable plan).
- **database** — **61 `db_insert`** (iteration 9, Task 3): window is R[B] = class id, R[B+1..] = one slot per **declared** field in declaration order; result R[A] = the new row's id. The loader validates the class-id slot statically (variable window: the field slots are validated at runtime by the engine against the class table). Engine failure traps `WO_T_DB`; a failed WAL commit traps `WO_T_IO` after un-applying the row. `database/src/db.c`.

**`?T` and nil.** A heap-shaped optional (`?Text`, `?Rec`, `?multi`, `?map`, `?@gc`) stores what `T` stores and spells nil as the **zero word** — every per-kind drop plan already ignores a zero slot, so `?T`'s field kind is `T`'s. A **nullable scalar** (`?Int`, `?Bool`, `?Timestamp`, `?Id`) cannot: `0` is a perfectly good `Int`, and real programs store it in a `?Int`. Its nil is therefore `WO_NIL_SCALAR` = −2^62 (not `INT64_MIN`: the compiler's own integers are 63-bit, so that value is not expressible on the emitting side). Such a field is marked `WOB_FIELD_NIL_SCALAR` in `field_class[i]`, which is how the runtime knows to write that word where it must produce absence itself — today only `json.decode` leaving a key absent, and `parse_int` on unparseable input.

`EQS` accepts a nil operand for the same reason: two `?Text` values compare with it, and the answer is "both absent is equal, one absent is not". A non-nil operand must still be a real Text.

**Trap codes:** DIV0, BORROW, STACK, OOM, DB, BOUNDS, KEY, EXPLICIT, IO (a syscall the source cannot prevent said no — errno's message rides in the error record).

## Enum payload variants (haxe-parity compiler Task 4)

`.wob` v1 is unchanged — no new section, no new header field, no version
bump. A union with at least one payload variant (`type Status = Pending |
Failed(reason: Text)`) compiles to **one ordinary class-table entry per
variant**, named `"<Union>.<Variant>"` in the constant pool (source
identifiers can never contain a dot, so the composite name cannot collide
with a declared class — the same convention the method table already uses
for `"Class.method"`). A variant's payload fields are the entry's fields,
declaration order, ordinary kind bytes — so a variant object is dropped,
masked, and cycle-scanned exactly like any other instance, including
recursive payload frees, with zero collector changes.

**The variant tag IS the class-table index**, carried by the object
header's existing `class_id` field — nothing new is stored and `NEW`
needs no change. The one VM addition is builtin **14 `variant_tag`**:
register A = the header `class_id` of the object in register B, so a
`switch` over a payload union reads the tag once and compares it against
`LOADK`-ed class-id constants — no per-arm allocation. It traps
`T_BOUNDS` on a null receiver or a native (`WO_CLS_*`) class id, the same
defense `ICALL` keeps; a non-pointer register stays the compiler's to
prevent (untyped registers, the residual-check doctrine). `variant_tag`
is compiler-internal: it is not a source-callable name and does not
appear in [`08-builtin-surface.md`](08-builtin-surface.md).

An **all-bare union** (`type CronResult = Ok | ErrorFinal | Miss`) never
reaches this file's format at all: its values are plain integer ordinals
(0, 1, 2 … in declaration order) in `WO_K_SCALAR` positions, compared
with `EQ` — no class entries, no heap objects, no `variant_tag`.

**Payload move-out** (Task 4 fix rounds 1–2): a `switch` arm that yields
its own payload binding as the switch's value (`case Boxed(b): b;`) MOVES
the payload out of the variant object — **pointer-kind fields only**
(OWNED/GCREF/TEXT/MULTI/MAP). The convention needs no format or collector
change: the compiler emits a `SETF` writing zero into the moved field
right after the value lands in its new owner's register, and the shell's
ordinary recursive drop plan — which already skips zero slots for every
kind (`runtime/src/gc.c wo_drop_kind`) — thereby frees the shell only.
Escaping a **SCALAR** field (Int/Bool/Timestamp/Id/`ref`, a bare-union
tag) is a plain COPY: no ownership moves and the field is left intact —
nulling it would corrupt the subject with a value indistinguishable from
a legitimate 0. A DISCARDED yield (statement-position switch) does not
null either: the shell keeps the payload and frees it as usual.
**Re-reading a moved-out payload is nil**: the field holds the zero word,
so a later `switch` over the same subject GETFs 0 into the binding and
any use of it traps `T_BOUNDS` ("null receiver") — memory-safe and
defined, the residual-check doctrine's direction; a later task may
promote this to a compile-time partial-move diagnostic (WO-E301 family).
One companion rule on the caller side: an **owned heap temporary** passed
as a borrow argument — a record/class constructor literal, a variant
construction, or an owned-returning call (`peek(Pay{})`,
`get(Boxed(Pay{}))`) — is copied to a stable register below the call
window and `DROP`ped by the caller once the call returns (`take`
arguments are the callee's to drop; places are their scope's; traced and
`Text` temporaries are excluded — the collector's and the Copy-aliasing
story's, respectively). Recursive drop is correct both ways, because a
payload the callee moved out left the field nulled.

**Typedef records** (`typedef Name = { ... }`) are ordinary class-table
entries too, with one compiler-side convention the loader never sees: two
records with the same shape (same ordered fields, same types, same
defaults) share a single entry — structural aliasing decided entirely at
emit time.

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
