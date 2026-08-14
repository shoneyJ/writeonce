# `runtime/src` — how the VM is put together

Written 2026-08-14, when the runtime grew the systems stdlib and json. Read
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
| `obj.h/.c` | the per-shard arena, object allocation, `wo_str` (header + length + inline bytes, no NUL) |
| `cont.h/.c` | `multi` and `map` as native classes: struct heads in the arena, backing arrays malloc'd, map lookup a linear scan over parallel key/value arrays |
| `gc.h/.c` | the kind-directed dispatcher (`wo_drop_kind`/`wo_drop_obj`), refcounting for `@gc`, and the budgeted Bacon–Rajan cycle collector |
| `borrow.h/.c` | the borrow word: shared counts and the exclusive sentinel |
| `loader.h/.c` | parse and **validate** an image; the validation contract in its header comment is exactly what the interpreter may then assume |
| `vm.h/.c` | the register interpreter: window-overlap calls, dual-flavor dispatch, traps, unwinding, catch frames |
| `builtin.h/.c` | the pure builtins: print, containers, text |
| `sysio.c` | the OS half: `fs`, `time`, `env`, `net`, `proc` |
| `json.c` | `json.encode` / `json.decode`, driven by class metadata |
| `main.c` | the CLI: find an image (argument or embedded trailer), build argv, call the entry, map its result to an exit code |

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
