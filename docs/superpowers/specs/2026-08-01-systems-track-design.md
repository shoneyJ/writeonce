# writeonce systems track — Haxe keyword study, program mode, systems stdlib (design)

**Date:** 2026-08-01
**Status:** approved design, pre-implementation
**Companion spec:** [`2026-08-01-oop-compiler-vm-design.md`](2026-08-01-oop-compiler-vm-design.md) (the OOP core this track extends)
**Driving workload:** `~/projects/log-watcher` — ~1,200 lines of Haxe compiled to C++ (`--cpp`), a single-binary systems daemon: log-tail watcher, cron.d supervisor, flock/pgrep probes, hand-rolled MCP-over-HTTP server, JSONL detection sink.

## Motivation

writeonce today can only be a full-stack server. log-watcher is the counterexample class: a CLI daemon that reads files, probes processes, serves a small TCP protocol, and sleeps in a poll loop. The language should build such applications too. Method: study the language log-watcher is written in — **Haxe** — keyword by keyword, adopt broadly what fits, reject explicitly what breaks doctrine, and prove the result by re-expressing log-watcher in `.wo`.

## Decisions locked during brainstorming

| Question | Decision |
| --- | --- |
| "Hexa" meaning | **Haxe** — log-watcher's language. |
| Deliverable | **Spec + `.wo` sample workload.** Repo pattern: samples force the grammar (blog/ecommerce/pricing precedent). |
| Adoption stance | **Broad Haxe parity MINUS doctrine breakers.** No inheritance (`extends`/`override`/`super`), no `Dynamic`/`untyped`, no macros — plan-13 doctrine and the OOP spec stay locked. Everything else adopts liberally. |
| Systems access | **Program mode + safe builtin stdlib.** No FFI/extern — capabilities are typed builtins implemented in the C runtime. |

## Part 1 — The Haxe keyword verdict table

Every Haxe keyword (plus the contextual ones), one verdict each: **have** (writeonce equivalent exists), **adopt** (new surface this track adds), **reject** (with the reason). This table is normative for the plan documents.

| Haxe keyword | Verdict | writeonce mapping / reason |
| --- | --- | --- |
| `class` | have | `class` (state + methods, no hierarchy) |
| `interface` | have | structural interfaces (OOP spec section 3) |
| `function` | have | `fn` |
| `var` (locals) | have | `let`; mutability via MVS rules, no second keyword |
| `this` | have | `self` (identifier, positionally bound) |
| `if` / `else` | have | same |
| `for` / `in` | have | same |
| `while` | have | same |
| `return` | have | same |
| `true` / `false` | have | same |
| `enum` | have+adopt | tagged unions exist; **adopt payload variants** (`Pending \| Failed(reason: Text)`) with exhaustive switch |
| `final` | have | MVS immutability by default; `const` for named constants |
| `new` | have | constructor brace literal `Type { … }`; no keyword |
| `switch` / `case` / `default` | **adopt** | expression-form switch, exhaustive over unions; `default` optional when exhaustive |
| `typedef` | **adopt** | structural record aliases with optional fields (`?field`) — the `SupConfig`/`TailState` pattern |
| `null` / `Null<T>` | **adopt** | `?T` optional types; forced handling before use (no nil deref trap possible); bare `null` only assignable to `?T` |
| `try` / `catch` | **adopt** | expression-form over the trap system: `catch` binds the structured error `{code, method, line, msg}`; uncaught = existing trap surface. `throw` (explicit raise) is **cut** — 0 uses in the driving workload; parked post-iteration-12 |
| `break` / `continue` | **adopt** | loop control |
| `do` (do-while) | **adopt** | parity, trivial |
| `static` | **adopt** | class-level `fn`/`const` — namespaced functions without instances (`Flock.held`, `Pgrep.alive` pattern) |
| `abstract` | **reject** | a distinct scalar type adds a conversion surface without buying safety this language needs; domain scalars are plain `Int`/`Text` |
| `using` | **adopt** | static extension methods — doctrine-safe reuse (composition sugar, the inheritance substitute) |
| `import` / `package` | **adopt** | `use` + directory-as-module; stdlib namespaces (`fs`, `proc`, `net`, `time`, `env`, `json`) |
| `is` | **cut** | runtime type test restricted to union variants and interface values; a compile error on statically-known types — 0 uses in the driving workload; parked post-iteration-12 |
| `inline` | **adopt (values)** | `const` compile-time values; inline *functions* rejected — optimization is the compiler's job |
| `public` / `private` | **adopt (as `pub`)** | default private; `pub` exports; property-accessor pattern `(default, null)` becomes `pub(read)` — public read, owner-only write |
| `#if` / `#else` / `#end` | **adopt** | build-flag conditional compilation only (the `-D portable` pattern); flags from the build command, no expression language beyond flag names |
| string interpolation `'${}'` | **adopt** | in string literals |
| `&&` / `\|\|` | **adopt** | spelled `and`/`or` (words, not symbols) — the lexer has no `&` case at all (a bare `&` reports `WO-E001`), so words cost nothing to add as keywords and read better in the sample's conditional-heavy code; one new precedence level below comparison and above assignment (`or` binds loosest, then `and`, then comparison, then the arithmetic ladder); short-circuit; `Bool`-typed operands only, `Bool` result, no truthiness; lowers to compare-and-jump on existing opcodes (`JZ` plus a jump), no VM change |
| `extends` | **reject** | no-inheritance doctrine (plan 13, OOP spec) — is-a via unions, has-a via composition |
| `super` | **reject** | no hierarchy to call up |
| `override` | **reject** | nothing to override |
| `overload` | **reject** | one name, one signature; keeps dispatch and diagnostics simple |
| `implements` | **reject (keyword)** | satisfaction is structural and implicit; declaring it adds a lie surface |
| `dynamic` / `Dynamic` | **reject** | static typing is the VM's foundation (untagged registers); typed `json.decode` covers the real use |
| `untyped` | **reject** | no escape hatch from the type system |
| `macro` | **reject** | kills the fast-compile promise; codegen belongs to `wo gen` tooling |
| `extern` | **reject** | no FFI hole in the memory-safety story; capabilities are audited builtins |
| `cast` | **reject** | no unsafe casts; conversions are typed (abstract `from`/`to`, explicit builtins) |
| `operator` | **reject** | no operator overloading; KISS |

## Part 2 — Program mode

**Entry.** A project containing a free `fn main(args: multi Text) -> Int` compiles as a **program**; the return value is the exit code. `wo run <dir>` executes it. `woc build` produces the single binary (plan-3 trailer mechanics unchanged). A project with `service` blocks and no `main` remains a server. Both present: `main` runs first and decides what to start — exactly log-watcher's shape (`watch`/`run`/`mcp` subcommands selecting the daemon flavor).

**Blocking model — the load-bearing decision.** Program mode runs one shard and blocking builtins are legal (`time.sleep`, `proc.run`, blocking `net.accept`): the Haxe original is a poll loop around `Sys.sleep`, and writeonce expresses that directly. Server shards keep the never-block doctrine — the *same* stdlib calls are loop-integrated (io_uring) there. One API, two execution disciplines, selected by mode. No `async`/`await` keyword exists in either mode.

**CLI surface.** `env.args() -> multi Text`, `env.get(name) -> ?Text`, `env.exit(code)` (never returns), `print`/`print_int` (exist) plus `print_err`; output flushes on newline (the reason for log-watcher's `Util.say` disappears).

**Daemon idiom.** `while true { …; time.sleep(ms) }` is supported and expected. Signals: the runtime owns signalfd (doctrine); SIGTERM/SIGINT set a shutdown flag programs poll via `env.stopping() -> Bool`. No signal callbacks.

## Part 3 — Systems stdlib

Six builtin modules, scoped to what log-watcher's code actually uses. **Every handle (file, socket, process) is an owned object whose drop closes it** — MVS deterministic destruction is RAII: no close bookkeeping, no leaked fds by construction, and a handle sent nowhere dies at scope end.

| Module | Surface | log-watcher use it covers |
| --- | --- | --- |
| `env` | `args()`; `get(name) -> ?Text`; `exit(code)`; `stopping() -> Bool` | CLI subcommand dispatch and exit-code propagation for `main`; `env.get` reads the MCP API key with an environment fallback (1 use); `env.stopping` drives the poll-loop shutdown check (4 uses) — the daemon idiom's exit condition. |
| `fs` | `exists(path)`; `stat(path) -> ?{size, inode, mtime}`; `read_at(path, offset, max) -> Text`; `read_all(path, cap)`; `append(path, text)`; `list(dir) -> multi Text` | rotation detection needs the inode; bounded tail-chunk reads (never front-to-back scans); JSONL detection sink (open-append-close); cron.d directory scan. **No write/truncate/delete in v1** — the read-only posture is the default posture. |
| `proc` | `run(cmd, args: multi Text) -> {code: Int, out: Text, err: Text}`, bounded capture | the `flock -n` exit-code probe and `pgrep -f`. Args-array only — no shell-string form, command injection unrepresentable. |
| `net` | `listen(addr, port) -> Listener`; `accept(listener) -> Conn`; `read(conn, max) -> Text`; `write(conn, text)` | the hand-rolled MCP HTTP subset (127.0.0.1 accept loop, one request per connection). TCP only in v1. |
| `time` | `now()` wall ms (exists); `sleep(ms)`; `iso(ms) -> Text`; `local(ms) -> {year, month, day, hour, minute, dow}` | the daemon sleep; `iso` gives JSONL detection timestamps and MCP response fields a stable textual instant; `local` gives cron next-fire computation broken-out calendar fields, including day-of-week. `mono()` is **cut** — 0 uses in the driving workload; parked post-iteration-12. |
| `json` | `json.decode(text) as RecordType -> ?RecordType`; `json.encode(value) -> Text` | config loading and JSON-RPC — **typed**, replacing Haxe's `Dynamic` idiom: missing optional fields are fine, shape mismatches yield nil, never a trap. The `as` here is the decode-target position only — a checked conversion returning `?T`, not a cast; it exists nowhere else (the `cast` rejection stands). Reuses the HTTP plan's C codec as builtins. |

### Core builtins

The sample calls **22 unqualified builtin names across ~176 sites**, none of
them in any spec: `len` ×55, `push` ×17, `byte_at` ×11, `starts_with` ×9,
`index_of` ×8, `has` ×7, `split` ×6, `split_ws` ×6, `join` ×5, `parse_int` ×5,
`trim` ×4, `slice` ×4, `substr` ×4, `pop` ×3, `ends_with` ×2,
`last_index_of` ×2, `to_lower` ×2, `sort` ×2, `char_of` ×1, `shift` ×1,
`remove` ×1, `reverse` ×1. `print_err` joins this set — Part 2 already named
it; this table never listed it.

These are **always in scope** — no `use` line, no namespace — the same status
`print`, `print_int`, `now`, `words`, `count`, `latest` already have, and they
share the same flat `WO_B_*` id space in the VM's builtin table as every other
builtin. Grouping into text operations, collection operations, and map
operations is documentation only, not namespaces. Each builtin has a
fixed-arity typed contract, resolved at compile time like every other
builtin; out-of-range indices **trap** (`T_BOUNDS`), never return a sentinel.

**Deliberately not adopted:** iteration/closure builtins (`map`, `filter`,
`reduce`) — the language has no function-value type, and adding higher-order
functions would require one.

## Part 4 — The sample workload

`docs/examples/log-watcher/` — the Haxe original re-expressed in `.wo`, file-for-file:

| `.wo` file | `.hx` sibling | carries |
| --- | --- | --- |
| `main.wo` | Main.hx | subcommand dispatch, config decode into a `typedef` record with `?fields` |
| `watcher.wo`, `logtail.wo` | Watcher.hx, LogTail.hx | `TailState` record, bounded tail reads, rotation-by-inode, quiet-period rule |
| `cron.wo` | Cron.hx | cron.d parse, next-fire computation |
| `probes.wo` | Flock.hx, Pgrep.hx | `proc.run` exit-code probes as `static fn`s |
| `mcp.wo` | Mcp.hx, Tools.hx | typed request/response records; **pure `handle(req) -> resp` kept socket-free** (the original's best design decision, preserved); serve loop over `net` |

A README table records the mapping and what (if anything) each file could not express — an empty "could not express" column is this track's acceptance criterion.

## Error handling

One system, two surfaces. Traps remain the runtime truth (OOP spec section 6). This track adds the language surface: `try expr catch (e) fallback-expr` — `e` is the structured error record; uncaught faults still surface as traps. `throw` (explicit raise) is **cut** — 0 uses in the driving workload; parked post-iteration-12. Optionals (`?T`) handle *expected* absence (missing file stat, failed decode, missing env var) — the stdlib returns nil for those, reserving traps for genuine faults. The Haxe original's `try … catch (e:Dynamic) return false` probes become optional-returning calls — clearer than the original.

## Testing

- **Language adoptions:** each feature lands with conformance-corpus fixtures — golden (runs, expected stdout) and must-fail (expected `WO-E###`) — extending the OOP track's corpus and error catalog.
- **Stdlib:** corpus fixtures against real resources — tempdir files (stat/inode/rotation simulation via rename), spawned `/bin/true`-class processes, loopback sockets. Handle-drop RAII proven under ASan (a leaked fd test: open many handles in a loop, assert no fd growth).
- **Acceptance:** the log-watcher sample compiles; its testable cores (tail state machine, cron next-fire, MCP `handle`) pass fixtures ported from the Haxe test suite's cases; the sample binary runs `watch` against a growing tempfile and detects an error-final quiet period.

## Success criteria

1. The keyword table is fully implemented: every **adopt** row parses, typechecks, and executes with corpus coverage; every **reject** row has a diagnostic or a documented absence.
2. `fn main` program mode: `wo run` executes a CLI program; exit codes propagate; `woc build` produces a self-contained binary for it.
3. All six stdlib modules pass their corpus fixtures; handle RAII is ASan-proven.
4. `docs/examples/log-watcher/` compiles and its README mapping table has an empty "could not express" column.
5. The sample's watch mode detects a silent death (error-final + quiet period) end to end on a real tempfile.

## Out of scope (named)

Threads/worker pools in program mode (the shard-actor track owns concurrency); UDP/TLS; `fs` mutation beyond append; signal callbacks; sqlite-equivalent embedded SQL over RAM (that is the DB engine's job — a future sample can wire MiniLog's idea to `select`); Haxe macro-based reflection idioms. `throw` (explicit raise), `time.mono`, and `is` are also cut — 0 uses in the driving workload each; parked post-iteration-12.
