# `log-watcher` — the systems-track sample workload

The Haxe original (`~/projects/log-watcher`, ~1,200 lines compiled to C++)
ported file for file, per the approved
[systems-track design](../../superpowers/specs/2026-08-01-systems-track-design.md)
(Part 4). A single-binary systems daemon: log-tail watcher, cron.d
supervisor, flock/pgrep probes, hand-rolled MCP-over-HTTP server, JSONL
detection sink. Program mode (`fn main`, blocking legal, one shard) plus the
stdlib modules it needs — `fs`, `proc`, `net`, `time`, `json`, `env` — carry
all of it; read each `.wo` next to its `.hx` sibling.

> **Status: shipped — the systems track's acceptance gate.** Run it with
> `just log-watcher` (`just log-watcher::build` / `::soak 60` for the rest).
> Landed with iteration 7 on 2026-08-15: executable, not merely compilable —
> zero ASan leaks in all three modes, SIGTERM ends parked syscalls, fds flat,
> `LW_SOAK` gate. The sample existed to force the grammar it uses (the
> blog/ecommerce/pricing precedent), and every form it needed — `use`,
> `typedef`, standalone union aliases (`type CronResult = …`), `pub(read)`,
> `switch`, `try` — is now shipped surface.

## The mapping

| `.wo` file | `.hx` sibling | carries | could not express |
| --- | --- | --- | --- |
| `main.wo` | Main.hx | subcommand dispatch; config decode into a `typedef` with `?fields` | — |
| `logtail.wo` | LogTail.hx | `TailState` record, bounded tail reads, rotation-by-inode, strict/relaxed classification, `last_lines` | — |
| `watcher.wo` | Watcher.hx | the per-log state machine: ALERT/CLEAR rule, cron `done()`/`result()`, `CronResult` union | — |
| `cron.wo` | Cron.hx | cron.d parse (aliases, redirect target, flock path), next-fire scan with the Vixie dom/dow OR quirk | — |
| `supervisor.wo` | Supervisor.hx | the 250 ms event loop: rescan, collapse, pre-fire lock probe, completions, detections sink | — |
| `probes.wo` | Flock.hx, Pgrep.hx | `proc.run` exit-code probes as `static fn`s, safe-direction fallbacks | — |
| `mcp.wo` | Mcp.hx, Tools.hx | typed request/response records; pure `handle(req) -> resp` kept socket-free; serve loop over `net` | — |

The spec's Part 4 table folds Supervisor.hx into the other files;
`supervisor.wo` stays separate because the original is a separate file and
the loop is the program. MiniLog.hx and its two tools
(`load_log_db`/`query_log_db`) are deliberately absent: the spec's
out-of-scope list assigns "sqlite-equivalent embedded SQL over RAM" to the
DB engine — a future sample wires MiniLog's idea to `select`.

## What the port deletes

| Haxe | Why it's gone |
| --- | --- |
| `import sys.FileSystem / sys.io.File / sys.io.FileSeek` | one `use fs`; `read_at` takes the offset — no seek state, no open/close bookkeeping (RAII: handle drop = close) |
| `LogTail.newState()` | record field defaults; construction is the brace literal |
| `Util.hx` (`say` = println + flush) | `print` flushes on newline (spec Part 2) |
| `loadConfig`'s Dynamic field-poking | one `json.decode(raw) as FileConfig` — typed, `?fields`, nil on mismatch |
| `try … catch (e:Dynamic)` probes | `try … catch (e)` over the trap system; expected absence is `?T`/nil instead |
| `#if portable` linker pragma | `woc build` is a static single binary by doctrine |
| socket `try c.close() catch` dance | connection is an owned value; scope end closes it |
| `while(true)` with no way out | `env.stopping()` — SIGTERM/SIGINT land as a flag, no signal callbacks |

## What this sample forces (spec follow-ups)

The spec's five modules cover the I/O; writing real code forced these
additions, in shrinking order of importance:

1. **`time.local(ms) -> {year, month, day, hour, minute, dow}`** and
   **`time.iso(ms) -> Text`** — cron next-fire needs calendar decomposition
   in the host timezone; the detections sink needs an ISO stamp. The spec's
   `time` sketch (now/mono/sleep) cannot express cron.
2. **`json.Value`** — an opaque, re-encodable JSON value. JSON-RPC echoes
   `id` back verbatim (number | string | null); no record type can hold it.
   Decode-target position only, like the `as` rule.
3. **`fs.stat` gains `dir: Bool`** — cron.d scanning must skip
   subdirectories; `{size, inode, mtime}` cannot.
4. **Text builtins**: `len`, `substr(s, start, len)`, `split`, `split_ws`,
   `trim`, `starts_with`, `ends_with`, `index_of`, `last_index_of`,
   `to_lower`, `byte_at`, `char_of`, `parse_int`, `"${…}"` interpolation.
   No regex module exists — the original's five ERegs are hand-rolled scans
   (see `classify_loose`, `is_env_line`, `find_log`, `sanitize`).
5. **Collection builtins**: `push`, `pop` (returns the removed element),
   `shift`, `slice`, `reverse`, `sort`, `join`; `map` index (returns `?V`),
   `has`, `remove`, `for k, v in m`.

## Deliberate divergences

| Original | Port | Why |
| --- | --- | --- |
| `Float` seconds everywhere | `Int` milliseconds | no Float scalar in the language; ms-as-Int matches the runtime (and Money's minor-units precedent) |
| config `services` entries: string **or** `{path}` object | strings only | typed decode; the object form was never used in the deployed config |
| tool schemas built per call | one JSON `const` | static data is static; the wire bytes are identical |
| JSON-RPC envelope via `Json.stringify` of a Dynamic | typed sub-records `json.encode`d into a concatenated envelope | typed json has no heterogeneous-object builder; `id` passes through as `json.Value` |
| `list_logs` mtime: seconds (float) | ms (int) | consistency with every other timestamp in the port |

## Memory model: why this sample has zero `@gc` and zero `@table`

The Haxe original transcompiles to C++ (`bin/src/*.cpp`, hxcpp target),
which makes the contrast measurable — there, **everything** is `@gc`:

1. Every object is GC-heap: `new Watcher_obj` behind `hx::ObjectPtr`, with
   `HX_DEFINE_STACK_FRAME` on every method so the collector can scan roots.
2. Every typedef is a `Dynamic` hash object: `TailState` has no C++ struct —
   the hot poll path does `this->st->__Field(HX_("lastLevel",…))`, a hashed
   string lookup per field access, every 2 s, per watched file
   (`LogTail.cpp` has 34 `Dynamic` sites, `Supervisor.cpp` 91, `Mcp.cpp` 123).
3. Allocation per poll: `poll()` returns a fresh anonymous GC object each
   call.

The port's ownership graph is a pure tree, so MVS covers all of it with
owned values and `@gc` earns its keep nowhere:

| Object | Owner | wo semantics |
| --- | --- | --- |
| `TailState` | its `Watcher` (field) | owned value; the `__Field` hash lookup becomes a fixed-offset load |
| `Watcher` | `Supervisor.services` **or** `.active` — never both | owned in one container; `remove()` IS the destructor — the Haxe kill-self comment, now literal |
| `CronWatch` | `scheduled` map | owned; iteration mutates via borrow |
| `PollResult`, `ParseResult`, every record | callee frame | stack lifetime, `DROP` at scope end, zero heap |
| `Tools`, `Mcp` | `main` → `Mcp.tools` | owned chain |

hxcpp pays GC on 100% of these objects; MVS pays on 0%. `@gc` would only
enter if a shape changed — one `Watcher` aliased by two live registries at
once, or a shared mutable cache aliased across requests (the OOP spec's
`PriceCache` pattern). log-watcher has neither.

`@table` is also correctly absent: it configures storage for engine-bound
classes, and program mode has no DB engine (sub-project 3). The C++ shows
exactly where it lands later — `MiniLog.cpp`'s in-memory sqlite,
`entries(id, path, seq, level, body, byteOffset)` + `idx_level`, which is
the future sample the spec names (wiring MiniLog's idea to `select`):
`@table(name: "entries", index: [path, seq])` on a `LogEntry` class turns
`load_log_db`/`query_log_db` into plain `insert`/`select`. Same for the
detections JSONL → a `@table` type with `insert` replacing
open-append-close. Until the engine links, annotating anything here would
claim storage that doesn't exist.

## Try it (when the track ships)

```bash
woc build docs/examples/log-watcher            # single static binary
log-watcher watch /var/log/myapp.log 10 2      # ALERT/CLEAR on stdout
log-watcher run /etc/cron.d config.json        # supervisor
log-watcher mcp /etc/cron.d config.json        # MCP on 127.0.0.1:<port>
```

Acceptance (spec): compiles; the tail state machine, cron next-fire, and
MCP `handle` pass fixtures ported from the Haxe test suite; `watch` detects
an error-final quiet period against a growing tempfile, end to end.
