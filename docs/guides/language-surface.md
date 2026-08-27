# The writeonce language surface — everything a `.wo` file may contain

Derived from the front end as it stands on 2026-08-24 and re-verified
against it on 2026-08-26, by reading `compiler/src/lexer.ml`,
`parser.ml`, `ast.ml` and `types.ml` — not a spec. Where this disagrees
with the compiler, the compiler is right.
The normative companions are
[`08-builtin-surface.md`](../plan/oop-vm/08-builtin-surface.md) (what the
runtime offers) and [`01-error-catalog.md`](../plan/oop-vm/01-error-catalog.md)
(every diagnostic). The reasoning under the front end is
[`compiler/src/CODE-LOGIC.md`](../../compiler/src/CODE-LOGIC.md).

Read this as the answer to "what can I write?" — the last section is the
matching answer to "what will the compiler refuse?", which is just as
much part of the surface.

Every form listed below was compiled and run against `woc`/`wovm` while
writing this page, not read off the parser and hoped for — with the one
exception §6 calls out by name (`group … by … into` parses and is then
refused).

---

## 1. Lexical

| thing | form | notes |
| --- | --- | --- |
| comment | `-- to end of line` | the only comment form; no block comment |
| identifier | `[A-Za-z_][A-Za-z0-9_-]*` | **internal dashes are legal** — `a-b` is ONE identifier, so binary minus after an identifier needs spaces (`a - b`) |
| integer | `42`, `0xFF`, `0b1011`, `1_000_000` | `_` only BETWEEN digits; hex/binary accumulate in 63-bit OCaml int, so a full-width `0xFFFF…FFFF` is out of reach (`-1` spells all-ones) |
| float | `1.5`, `2e9`, `1.0e-3` | a bare digit run stays `Int`; only a fraction or exponent makes a `Float` |
| text | `"..."` or `'...'` | escapes `\n \t \r \0 \\ \" \'`; anything else after `\` is that literal character. A raw newline inside is **WO-E005** |
| interpolation | `"${expr}"` | desugars at parse time to a `..` chain; `\$` is a literal `$`, and a lone `$` not followed by `{` is literal |
| raw text literal | `` `...` `` | iteration 37 — content verbatim (NO escape processing), newlines are content, common source margin removed at compile time. Holes: `${e}` raw, `{{ e }}` HTML-escaped |
| booleans / nil | `true`, `false`, `nil` | |
| newline | significant | terminates a statement (or `;`). A line ending in `..` continues on the next — the one newline suppression |
| build flags | `#if name` / `#else` / `#end` | token-level filter, flag NAMES only (no expressions); nesting allowed; set with `woc -D name` |

**Keywords** (37): `type class interface fn let mut take return if else
while for in true false use spawn using pub break continue do const and
or not inline switch case default typedef try catch nil as INSERT
SELECT`.

Deliberately NOT keywords, so they lex as ordinary identifiers: `self`,
`me`, `subscribe`, `receive`, lowercase `insert`/`select`, and every
query clause word (`from`, `where`, `group`, `by`, `into`, `order`,
`desc`, `take`, `select`) plus every type constructor word (`ref`,
`multi`, `map`, `backlink`, `actor`).

## 2. File and module level

A directory is a module; `pub` is the export line. A file may contain,
in any order:

| declaration | form |
| --- | --- |
| import | `use fs` (reserved stdlib namespace) or `use shared/util` (project-relative path) |
| import + extension methods | `using shared/textutil` — the module's `pub` free fns whose first parameter matches a receiver become callable as methods on it (compile-time rewrite) |
| class | `[pub] class Name { fields, methods, consts }` |
| plain type | `[pub] type Name { ... }` — identical field grammar to `class`; methods parse for real in both |
| structural record | `typedef Name = { field: T, ... }` — fields only, and two records of the same SHAPE are the same type |
| tagged union | `type Name = A \| B \| C(x: Int, ...)` — bare variants lower to integer tags, payload variants to records. Construction is call-style and POSITIONAL: `C(1, 2)`, never `C { x: 1 }` |
| interface | `[pub] interface Name { fn sig(...) -> T }` — signatures only, no fields, no bodies. Satisfaction is STRUCTURAL |
| free function | `[pub] fn name(params) -> T { ... }` |
| constant | `const NAME = <literal>` — substituted by the parser before typecheck; a local of the same name shadows it |

The program entry is the free `fn main`, zero-argument or `fn
main(args: multi Text) -> Int`.

### Annotations

| annotation | where | effect |
| --- | --- | --- |
| `@table(name: "…", index: [a], index: [b, c])` | on a `class`/`type` | the class IS a WAL-backed table |
| `@unique` | on a field | uniqueness constraint |
| `@gc` | on a class | **rejected** — GC-ness is inferred, never declared |

Unknown annotation names parse and are ignored; argument lists on field
annotations are consumed and discarded.

## 3. Types

| kind | spelling |
| --- | --- |
| scalars | `Int`, `Float`, `Bool`, `Text`, `Bytes`, `Timestamp`, `Id` |
| nullable | `?T` — legal on any of the above and on heap shapes |
| list | `multi T` |
| map | `map<K, V>` |
| row reference | `ref C` |
| reverse relation | `backlink C.field` — the computed inverse of a `ref` |
| actor address | `actor M` — M is the message type, inferred from the class's `receive` |
| declared types | any `class` / `type` / `typedef` / union name |
| stdlib types | `json.Value`, `net.Conn` |
| predeclared records | `Stat`, `TimeParts`, `Proc`, `Error` — no source declares them; field ORDER is the contract with the C runtime |

### Fields and parameters

- Field: `name: T`, optionally `= <default>` and/or `@ann`.
- `pub(read) name: T` — readable outside the declaring class, writable
  only inside it.
- Parameter conventions: **borrow is the default**; `mut x: T` for a
  mutable borrow, `take x: T` to move ownership in. These apply to
  NAMED parameters only.
- **`self` is implicit** — never written in the parameter list, and
  always writable inside its own class's methods (`self.total += 1`
  needs no annotation). `self` is an ordinary identifier, not a keyword.
- Methods may be `static fn`; class-level constants may be `static
  const` or bare `const`.

## 4. Statements

| statement | form |
| --- | --- |
| binding | `let x = e`, `let x: T = e` |
| assignment | `x = e`, `obj.f = e`, `m[k] = e` |
| compound assignment | `x += e`, `-=`, `*=`, `/=`, `%=` — parse-time sugar for the written-out form (there are no bitwise compound assigns) |
| conditional | `if c { } else if c { } else { }` |
| while | `while c { }` |
| do-while | `do { } while c` — body always runs once |
| for | `for x in <multi \| query>`, `for k, v in <map>` |
| loop control | `break`, `continue` — owned values alive in the body are dropped at the jump site |
| return | `return` / `return e` |
| database write | `insert C { f: v, ... }`, `delete e` |
| expression | any expression in statement position |

## 5. Expressions

| form | spelling |
| --- | --- |
| literals | int, float, text, raw text, bool, `nil` |
| container literals | `[]`, `[a, b, c]` (a `multi`), `{}` (an empty `map`) — a fresh container needs a destination of declared type |
| constructor | `C { field: v, ... }` |
| access | `x.f`, `c[i]` (a `multi` — out of range traps), `m[k]` (a `map` — a missing key is **nil**) |
| call | `f(a, b)`, `x.m(a)`, `mod.member(a)` |
| unary | `-e`, `not e` |
| binary | see the ladder below |
| checked conversion | `e as T` — its ONE meaning is decoding JSON text, yielding `?T`. There is no reinterpret cast |
| actor spawn | `spawn C { fields }` → an `actor M` address |
| trapping guard | `try <expr> catch (e) <expr-or-block>` — an EXPRESSION; `e` binds the `Error` record `{ code, line, method, msg }` |
| multi-way choice | `switch e { case A: ...; default: ...; }` — an expression. Arms match VALUES; `case a, b:` fires for either. A union payload binds positionally: `case Rect(w, h): w * h`. `default` is required UNLESS the subject is a union whose variants are all covered |
| query | `from … select …`, see below |
| interpolation | inside `"…"` and `` `…` `` |

### Operator precedence, loosest to tightest

1. `or`
2. `and`
3. comparison — `== != < <= > >=`
4. concatenation — `..`
5. additive — `+ -` **and `|` `^`**
6. multiplicative — `* / %` **and `&` `<<` `>>`**
7. unary — `-`, `not`
8. `as` conversion — binds to a postfix expression, so tighter than unary
9. postfix — call, index, field

Bitwise operators do not get their own tiers: `|`/`^` ride the additive
rung and `&`/`<<`/`>>` the multiplicative one (Go's arrangement). They
are Int-only on BOTH sides — there is no Float twin — and a literal
shift count outside `0..63` is a compile error.

`+` is arithmetic ONLY, never string addition. `and`/`or` are
short-circuit, `Bool`-typed operands only — there is no truthiness, and
no `&&`/`||`/`!`/`~` anywhere in the language.

## 6. Queries (language-integrated, never SQL text)

```
from <var> in <source>
  where <expr>            -- zero or more
  group <expr> by <key> into <gvar>    -- PARSES, THEN REFUSED (see below)
  order by <expr> [desc]
  take <expr>
  select <expr>
```

The source is either a table class (`from p in Product`) or a
navigation (`from s in dept.staff` — a `backlink` or a `multi`). Present
today: **from / where / order / take / select**. A query is an
expression and is also what `for x in <query>` iterates.

`group … by … into` is the one clause above that is grammar without
semantics: the parser accepts it (`parser.ml`) and the typechecker then
rejects it with WO-E250 — "group-by aggregation is not supported yet",
or "group-by on a navigation query is not supported yet" for the
navigation form (`types.ml`). It is listed because the syntax is
settled, not because it runs. Joins are not in the slice at all.

## 7. Concurrency

- `spawn C { fields }` constructs the actor's state (fields MOVE in) and
  starts it; the value is an `actor M` address.
- `send(addr, msg)` — fire and forget; the message MOVES to the runtime.
- `call(addr, msg) -> R` — a send that parks the calling fiber until the
  receive returns. Every `receive` program-wide must agree on `R`, and
  `R` must be a copyable scalar. A dead callee traps, never hangs.
- A class becomes an actor by declaring `fn receive(msg: M)` — `receive`
  is an ordinary identifier, not a keyword.
- Blocking stdlib calls park the fiber. There is no `async`, no `await`,
  and no user-visible thread.

## 8. Builtins and the stdlib

**Free builtins** (a user-declared `fn` of the same name always wins):
`print`, `print_err`, `print_int`, `now`, `words`, `len`, `count`,
`byte_at`, `char_of`, `substr`, `trim`, `to_lower`, `starts_with`,
`ends_with`, `index_of`, `last_index_of`, `split`, `split_ws`, `join`,
`parse_int`, `int_to_text`, `multi_new`, `map_new`, `push`, `get`,
`set`, `has`, `remove`, `latest`, `pop`, `shift`, `slice`, `sort`,
`reverse`, `key_at`, `val_at`, `send`, `call`, `sha1`, `sha256`,
`hmac_sha256`, and the Float/Bytes bridges `float`, `trunc`,
`parse_float`, `float_to_text`, `float_cmp`, `bytes_len`, `bytes_at`,
`bytes_slice`, `bytes_eq`, `bytes_concat`, `bytes_of_text`,
`text_of_bytes`, `base64_encode`, `base64_decode`.

**Reserved module namespaces**, each resolving to builtins:

| module | covers |
| --- | --- |
| `fs` | `exists`, `list`, `stat`, `read_all`, `read_at`, `append` |
| `time` | `now`, `sleep`, `local`, `iso`, `ticks` |
| `env` | `get`, `stopping` (the SIGTERM/SIGINT latch) |
| `net` | `listen`, `listen_unix`, `accept`, `accept_dl`, `read`, `read_dl`, `write`, `write_dl`, `peer`, `close` |
| `proc` | `run` |
| `json` | `encode`, `decode` (paired with `as T`) |

A failing syscall traps `IO` with errno's message; **absence is never a
trap** — a missing path or an unset variable is nil.

## 9. What the language deliberately does NOT have

This list is doctrine, not a backlog. Each was considered and rejected.

- **Closures and function values.** Capture is a field on a class. This
  is why there is no dependency injection and no callback API anywhere.
- **`&&`, `||`, `!`, `~`.** Word operators only (`and`, `or`, `not`);
  complement is `-1 ^ x`.
- **Inheritance.** Interfaces are structural; there is no `extends`.
- **`inline fn`.** The keyword exists solely to produce a clear
  rejection — optimization is the compiler's job.
- **A reinterpret cast.** `as` decodes JSON and nothing else.
- **Varargs, and generics beyond the built-in containers.**
- **Truthiness.** A condition must be `Bool`.
- **A block comment, and a `{{`/`${`/backtick escape inside a raw
  literal.** Those three are written by concatenating an ordinary
  `"..."` string with `..` — one greppable door.
- **`async`/`await`.** Fibers park; the shard runs someone else.
- **A runtime template engine.** Markup is a compile-time literal or it
  does not exist.
- **Non-empty map literals** (`{ k: v }` in expression position) —
  indistinguishable from a constructor literal without lookahead nothing
  else needs.
- **A full-width 64-bit integer literal**, and **joins in queries** —
  both real limits rather than doctrine.
