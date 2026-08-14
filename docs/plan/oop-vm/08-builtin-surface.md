# Milestone-1 source surface the emitter lowers — normative reference

> What a `.wo` program may say and have `woc` produce bytecode for.
> The `.wob` format doc ([`00-wob-format.md`](00-wob-format.md)) names the
> BUILTIN *ids*; this names their **source spellings** and the handful of
> rules that have no other home. Landed with the emitter
> (`compiler/src/emit.ml`, plan 3 task 1). Diagnostics referenced here are
> catalogued in [`01-error-catalog.md`](01-error-catalog.md).
>
> Anything on this page is a contract for corpus fixtures and for every
> later sub-project's `.wo` code — not an emitter implementation detail.

## Builtins

Containers and runtime services are free functions, never methods. Each
maps to one `BUILTIN` id of the format doc.

| source | `.wob` builtin | arity | meaning |
| --- | --- | --- | --- |
| `now()` | `now` | 0 | wall-clock milliseconds (`Int`) |
| `print(t)` | `print` | 1 | a `Text`, newline-terminated |
| `print_int(n)` | `print_int` | 1 | an `Int`, newline-terminated |
| `words(t)` | `words` | 1 | whitespace token count of a `Text` |
| `multi_new()` | `multi_new` | 0 | a fresh `multi T` — see the destination rule below |
| `map_new()` | `map_new` | 0 | a fresh `map<K, V>` — see the destination rule below |
| `push(m, v)` | `multi_push` | 2 | append to a `multi` |
| `count(c)` | `count` | 1 | length of a `multi` or a `map` |
| `latest(m)` | `latest` | 1 | last element of a `multi` (traps `BOUNDS` when empty) |
| `get(c, k)` | `multi_get` / `map_get` | 2 | element by index, or value by key (a missing key traps `KEY`) |
| `set(m, k, v)` | `map_set` | 3 | insert or replace in a `map` |
| `has(m, k)` | `map_has` | 2 | `1`/`0` |
| `int_to_text(n)` | `int_to_text` | 1 | decimal rendering of an `Int`, as a fresh owned `Text` — haxe-parity Task 2's one fenced VM addition, the type-directed half of string interpolation (below); also directly callable |
| `len(x)` | `len` | 1 | byte length of a `Text`, or element/entry count of a container |
| `byte_at(t, i)` | `byte_at` | 2 | byte value at an index; out of range traps `BOUNDS` |
| `print_err(t)` | `print_err` | 1 | a `Text` to stderr, newline-terminated |
| `starts_with(t, p)` / `ends_with(t, s)` | same | 2 | `1`/`0` |
| `index_of(t, n)` / `last_index_of(t, n)` | same | 2 | first/last byte offset, `-1` when absent |
| `substr(t, start, len)` | `substr` | 3 | fresh `Text`, clamped (never traps) |
| `trim(t)` / `to_lower(t)` | same | 1 | fresh `Text` |
| `char_of(b)` | `char_of` | 1 | fresh one-byte `Text` |
| `parse_int(t)` | `parse_int` | 1 | `?Int` — an unparseable text is `0`, which is how `?Int` spells nil |
| `split(t, sep)` / `split_ws(t)` | same | 2 / 1 | fresh `multi Text` |
| `join(m, sep)` | `join` | 2 | fresh `Text` from a `multi Text` |
| `slice(m, from, to)` | `slice` | 3 | fresh `multi` over `[from, to)`; `Text` elements are COPIED, so slice and source never both own one value |
| `pop(m)` / `shift(m)` | same | 1 | removes and returns the last/first element (ownership moves to the caller); empty traps `BOUNDS` |
| `sort(m)` / `reverse(m)` | same | 1 | in place; `sort` compares `Text` by content, everything else as signed integers |
| `remove(m, k)` | `map_remove` | 2 | `1`/`0`; drops the removed key and value |
| `key_at(m, i)` / `val_at(m, i)` | same | 2 | slot-ordered map enumeration — what `for k, v in m` lowers onto |
| `m[i] = v` on a `multi` | `multi_set` | 3 | in-place element write, dropping the element it replaces |

`get`, `set`, `push`, `count` and `has` resolve on the container they are
given, so one source name covers the `multi` and `map` ids the runtime
keeps apart.

**Sugar.** `c[i]` is exactly `get(c, i)` and `m[k] = v` is exactly
`set(m, k, v)` for a `map` and `multi_set(m, i, v)` for a `multi` (the
element it replaces is the container's, so the VM drops it).

**Shadowing.** A user-declared free `fn` of the same name always wins. A
declared name is never silently replaced by a builtin.

**`push` and `@gc` elements.** `push(m, v)`'s value argument is never a
resolved callee parameter (`push` has no declared signature), so the
owner pass's ordinary Take-gated transfer never reaches it; a `@gc` value
pushed into a `multi` is special-cased in `owner.ml`'s `analyze_call`
(the value escapes into the container exactly like a ctor field, RC_INC
included) specifically so a `multi`-mediated `@gc` cycle can be built
and later collected (`tests/corpus/gc/`, plan 3 task 5). **`set(m, k, v)`
has no equivalent special case** — a `@gc` key or value handed to `set`
is not retained, so a `map<_, SomeGcClass>` (or a `@gc`-keyed map) built
this way will under-count its element's refcount and the collector will
free it while the map still points at it. Nothing in the corpus
exercises this yet; treat it as an open gap, not a proven-safe pattern,
until `set` gets the same fix `push` did.

## A fresh container needs a destination of declared type

`multi_new()` and `map_new()` carry their element (and key/value) kinds as
an instruction immediate, and those kinds **are** the container's drop
plan at runtime (`runtime/src/gc.c`). They cannot be guessed: assuming
`SCALAR` for a `multi Item` leaks every element, and for a
`map<Text, _>` leaks every key. Milestone-1 `let` has no container type
annotation — its optional annotation is a bare identifier — so a fresh
container must be created where its type is declared:

```wo
class Store {
  items:   multi Item
  by_name: map<Text, Int>
}

fn main() {
  let s = Store { items: multi_new(), by_name: map_new() }   -- kinds from the fields
  push(s.items, Item { n: 7 })
  set(s.by_name, "seven", 7)
}
```

A bare `let m = map_new()` is `WO-E403`, reported at the creation site.
The same rule applies to a `take`/`mut` parameter of declared container
type, which is also a typed destination.

## Calls

- Argument count must match the callee's parameter count (`WO-E403`).
  Nothing upstream checks arity — `types.ml` declares `WO-E203` and never
  raises it — and a mismatched call reserves a register window the callee
  does not read, which the loader rejects outright.
- A method is called as `receiver.method(args)`. When the receiver's
  declared type is an **interface**, the call is dispatched by vtable
  (`ICALL`); satisfaction is structural (same method name, same parameter
  count), Go-style, with no `implements` keyword.
- `self` occupies the callee's `r0`, so a method's argument count is
  `1 + parameters`.

## Modules (haxe-parity Task 1)

A `.wo` file's **module is its directory** — no manifest, no declared
module name. Every file sharing a directory sees every other same-
directory file's declarations unconditionally (Task 8's existing
multi-file discovery, unchanged); a name declared in a *different*
directory needs `use` to become visible at all, and even then only if
it is marked `pub`.

- **`pub`** on a top-level `class`/`type`/`interface`/`fn` exports it
  outside its own module. Default is private-to-module — visible to
  every file in the same directory, invisible to every other module
  regardless of `use` (`WO-E217` if referenced anyway). `pub` on a
  class/interface method, or the `pub(read)` field-accessor marker
  (Haxe's `(default, null)`), is a different, later feature — not this
  one.
- **`use fs`** (a bare, single-segment name) is a **reserved stdlib
  namespace**: exactly `fs`, `proc`, `net`, `time`, `json`, `env`, no
  others, and always stdlib even if a same-named project directory
  exists. A call through one (`fs.stat(...)`) typechecks as
  UNKNOWN-BUT-RESERVED — no E207/E225/arity check, since the six
  namespaces' members arrive in plan 9 — and is `WO-E406` only if such
  a call survives all the way to emission; an unused `use fs` compiles
  clean (modulo `WO-W202`, below).
- **`use shared/util`** (slash-separated segments) is **project-
  relative**: it must name a directory this program's own discovery
  actually finds, or `WO-E216`. The alias a call site uses is always
  the path's *last* segment (`util.fn(...)`, not `shared.fn(...)`).
- **Resolution order** for a bare (unqualified) name: this file's own
  module, unconditionally; then every `use`d module's `pub` surface. If
  more than one used module exports the same `pub` name, that is
  `WO-E218` — collisions diagnose rather than silently pick a winner.
  A qualified reference (`alias.name(...)`) skips straight to its
  named module; `WO-E217` if `name` exists there but isn't `pub`.
- A `use` clause never referenced (bare or qualified) anywhere in its
  own file is `WO-W202` — a warning, so it does not fail the build.

## Program entry

The entry point is the **zero-argument free `fn main`**. A `main` that
takes parameters is not an entry (the format's own rule is a zero-argument
free fn), and `wovm` will report `module has no entry method`.

## Operators with no dedicated opcode

Lowered by the emitter, not added to the format:

| source | lowering |
| --- | --- |
| `a % b` | `a - (a / b) * b` — exact for the VM's truncating `DIV`, which traps on `0` and on `INT64_MIN / -1`, both correct for `%` too |
| `a != b` | `(a == b) == 0` |
| `a > b`, `a >= b` | `LT` / `LE` with the operands swapped |
| `a == b` on `Text` | `EQS` (content equality); `EQ` otherwise |
| `a .. b` | `CONCAT` — `+` is arithmetic only, never string addition |
| `a and b` | evaluate `a`; `JZ` past evaluating `b` (result stays `a`'s value); else evaluate `b` into the same register (haxe-parity Task 2) |
| `a or b` | evaluate `a`; `JZ` + `JMP` past evaluating `b` when `a` is already true; else evaluate `b` (haxe-parity Task 2) |

## Not lowerable in milestone 1

Each is `WO-E403` at the offending site, never invented bytecode:

- an element write into a `multi` (no element-store instruction);
- `for` over a `map` (v1 exposes no key enumeration);
- a name that is neither a local, a parameter, `self`, a declared `fn`,
  nor a builtin;
- a field or method on a type that is not a declared class — including a
  class named only inside `multi T` / `ref T`, which `types.ml`'s
  unknown-type check (`WO-E225`) does not look inside.
- `break`/`continue` outside any loop (haxe-parity Task 2) — nothing
  upstream tracks loop nesting to reject it earlier, so the emitter's
  own "no legal jump target" gate is the only one.
- interpolating (`"${expr}"`) a value that is neither `Text` nor `Int`
  (haxe-parity Task 2) — the brief's own scope; a class, `multi`, `map`,
  or other scalar has no defined textification here.

## Small control surface (haxe-parity Task 2)

`break`/`continue`/`do...while`, `const`, `and`/`or`, and string
interpolation — the haxe keyword verdict table's low-risk batch, added
2026-08-11.

- **`break`/`continue`** reuse the owner pass's own `return`-drop
  machinery, bounded to the nearest enclosing loop instead of the whole
  function: an owned value still alive in the loop body is dropped at
  the `break`/`continue` site itself, not left to leak (proven under
  ASan, `tests/corpus/run/lang-break-owned-drop/`). `continue`'s actual
  jump target depends on loop shape — `while`'s own condition check,
  `for`'s increment step, or `do...while`'s condition check — but the
  drop-set computation is identical either way.
- **`do { body } while cond`** — the body always runs at least once;
  lowered onto the same `JZ`/`JMP` pair `while`/`for` already use, just
  reordered.
- **`const NAME = <literal>`** (top-level, or bare — no `static` —
  class-level) is resolved entirely by `parser.ml`, before typecheck
  ever runs: every unshadowed reference is replaced by the literal it
  names, so nothing downstream (types/owner/emit) has any const-specific
  code at all. A local/parameter/`self` of the same name always shadows
  it. `static const` is Task 7's own syntax (`static`), not recognized
  here.
- **`and`/`or`** are real keywords (never `&&`/`||`), one precedence
  level below comparison (`or` loosest, then `and`, then comparison —
  so `a == 1 and b == 2` needs no parens). `Bool`-typed operands only,
  no truthiness: a confidently-non-`Bool` operand is `WO-E201`. Short-
  circuit, lowered to compare-and-jump above — no new opcode.
- **String interpolation** (`"${expr}"`) desugars at parse time to a
  `..` (`Concat`) chain of text segments and embedded expressions; each
  embedded expression's *textification* is decided at emit time, once
  its type is known: `Text` passes through untouched, `Int` is wrapped
  in `int_to_text` (above), anything else is `WO-E403` (see "Not
  lowerable in milestone 1"). `\$` is a literal `$` (so `\${x}` stays
  literal, never interpolates); a lone `$` not followed by `{` is also
  literal, unconditionally.

## `?T`

A nullable field stores exactly what `T` stores and spells nil as `0`.
The v1 format has no kind byte for it (field kinds run `0..5`; the loader
rejects `6`), and it needs none: every per-kind drop plan already ignores
a zero slot. `?T`'s field kind is therefore `T`'s. Note the consequence
for `@gc`: `?SomeGcClass` is a `GCREF` field like any other, so it
participates in refcounting and cycle detection normally.

## The systems stdlib's OS half (`fs`, `time`, `env`, `net`, `proc`)

Reserved module names resolve to one builtin per member
(`runtime/src/sysio.c`). Every one is a thin blocking libc call, so the
failure surface is uniform: a syscall that fails traps `WO_T_IO` carrying
errno's own message, and the source decides with `try ... catch` whether
that is fatal. Absence is never a trap — a missing path from `fs.stat` and an
unset `env.get` are nil.

| member | signature | notes |
| --- | --- | --- |
| `fs.exists(path)` | `-> Bool` | |
| `fs.list(dir)` | `-> multi Text` | names only, unsorted; unreadable dir traps `IO` |
| `fs.stat(path)` | `-> ?Stat` | `Stat { size, mtime (ms), inode, dir }` |
| `fs.read_all(path, cap)` | `-> Text` | truncated at `cap` |
| `fs.read_at(path, off, len)` | `-> Text` | short read allowed (a growing file is normal) |
| `fs.append(path, text)` | — | creates the file if absent |
| `time.now()` | `-> Int` | wall-clock ms; the existing `now` builtin |
| `time.sleep(ms)` | — | |
| `time.local(ms)` | `-> TimeParts` | `{ year, month, day, hour, minute, second, dow }`, dow 0 = Sunday |
| `time.iso(ms)` | `-> Text` | UTC, second precision |
| `env.get(name)` | `-> ?Text` | unset is nil |
| `env.stopping()` | `-> Bool` | SIGTERM/SIGINT latch, handlers installed on first use |
| `net.listen(host, port)` | `-> Int` | IPv4, SO_REUSEADDR, backlog 64; returns an fd |
| `net.accept(fd)` | `-> Int` | |
| `net.read(fd, max)` | `-> Text` | one read; the empty Text is EOF |
| `net.write(fd, text)` | — | writes all of it |
| `net.close(fd)` | — | |
| `proc.run(cmd, args)` | `-> ?Proc` | `Proc { code, out, err }`; stdout/stderr captured and capped |

`Stat`, `TimeParts` and `Proc` are **predeclared records**: no source declares
them, and their field ORDER is the contract with `sysio.c`, which writes them
by index. The compiler passes the record's class id as the member's last
argument, so the VM allocates what it fills.

## `json`

`json.encode(x) -> Text` and `json.decode(text) as T -> ?T`. Both are
metadata-driven (`runtime/src/json.c`): the class table's per-field names,
referenced classes and element kinds (`.wob` v2) are what let one
implementation encode and decode any record shape, with no per-type generated
code.

- `encode` takes the value's *static* kind alongside it, because a register
  alone cannot say whether it holds an i64 or a pointer; everything below the
  top level comes from object headers and the class table.
- `decode` parses and binds straight into the target class: keys are matched
  against field names, a nested object is built as that field's class, an
  array as a `multi` of that field's element kind, unknown keys are skipped,
  and absent keys stay nil. Malformed input yields nil — never a trap, which
  is what makes the `as` form a *checked* decode. `as` exists for no other
  purpose: there is no reinterpret cast in the doctrine.
- `json.Value` is a reserved type name for a decoded value the source does not
  inspect: it holds the raw JSON slice it came from (kind TEXT) and encodes
  back verbatim.

Two documented limits: a `Bool` field is a SCALAR slot like any other integer,
so it encodes as `0`/`1` rather than `false`/`true` (the kind byte does not
distinguish them); and a JSON number with a fraction or exponent decodes by
truncation to `Int`, since the language has no float.
