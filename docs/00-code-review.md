# Code Review: log-watcher Compilation Requirements

This document was a gap analysis of what the `woc` front end needs before the
log-watcher sample compiles. Its findings were extracted on 2026-08-10 into
[`superpowers/specs/2026-08-10-logwatcher-gap-closure-design.md`](superpowers/specs/2026-08-10-logwatcher-gap-closure-design.md)
and the plans it amends; its Phase 1–4 roadmap is retired in favour of the
approved story iterations. See [`00-status.md`](stories/00-status.md) for current
status.

## Standing critique (undated, author unrecorded)

Native speed — the big one. Everything is interpreted: ~40× behind Go on raw compute, no JIT, no AOT-to-native. The scheduling primitives win benchmarks; a compute-bound handler loses them all back. There is also no Float type at all (the storefront prices in cents for a reason), no SIMD story, and fixed interpreter ceilings (4096 register slots, 256 frames, ~42 KiB per fiber until growable contexts land).

- Language expressiveness. No generics — the cache stores Text and tells you to json.encode; no function values or closures (doctrine, but it's why every handler is a class with one method); byte-based strings with no Unicode awareness; no Result-style error values (traps + try only); pattern matching is a switch, not destructuring. Some of this is deliberate rejection, but "deliberate" doesn't make the expressiveness appear.
- Concurrency holes the arc hasn't closed. send is one-way — no reply/request-response primitive (my own benchmarks couldn't await the actor and had to sleep); no supervision, links, or actor death (actors live until process end); unbounded mailboxes with zero backpressure; no timers beyond sleep; round-robin placement with no work stealing; multi-shard DB access still traps (stage 3 unbuilt); accept lives on one shard.
- Production plumbing. No TLS anywhere (proxy-mandated forever), no HTTP/2 or WebSockets yet, no crypto primitives (blocked on the bit-ops-vs-builtins fork), observability is print/stderr — no metrics, tracing, or profiler; no debugger, no LSP (discussed, never built); deps are git-rev-only with no registry, no transitive resolution, no semver; blue-green deploy and schema migrations are recorded futures, not features.
- Proof maturity. 22's benchmark battery has never run — every number so far is a scratch measurement on one machine; TSan covers one demo; no fuzzing, no CI beyond local just, and the whole ecosystem is one framework, five samples, and one committed consumer. The honest summary: the architecture is ahead of the product — the doctrine bets (ownership+inference, actors, one binary, io_uring) are landing and measurable, while the surface a developer touches daily (types, tooling, ecosystem) is years behind the languages it benchmarks against.

## Verification 2026-08-20

> **Read the 2026-08-26 re-verification at the bottom before quoting anything
> from this section.** Eight of its rows have since been overtaken by shipped
> work. The section is kept as written — it is a dated measurement, and
> rewriting it would destroy the record of what was true when the iteration
> order was re-sequenced against it.

Every claim above was checked against the tree. **26 of 27 hold. One number
does not, and two problems are worse than stated.**

### Rejected: "~40× behind Go on raw compute"

Unsourced. Nothing in the repo measures compute against Go. The only Go
comparison on record runs the other way and on a different workload:
[`plan/exploration/c-runtime/00-plan.md`](plan/exploration/c-runtime/00-plan.md)
records the C prototype at **908,916 reads/s and 643,250 fsync-acked
commits/s on 8 shards vs Go `net/http` at 495k/355k with ~8× worse p99** on a
20-core box — I/O-bound serving, not compute. `runtime/bench/goref/` holds a
Go reference program, but no compute-bound result from it is written down
anywhere.

The figure also contradicts this document's own closing sentence: the
doctrine bets cannot be "measurable" while iteration 22 has never run. Drop
the number or produce the benchmark.

### Understated

- **`map<K,V>` lookup is a linear scan.** `runtime/src/cont.h`: parallel
  key/value arrays, "linear scan lookup — deliberate milestone-1 KISS". Every
  `get`/`has`/`set` is O(n). For a language whose framework routes requests
  and whose planned cache is keyed, this outranks the missing `Float` as a
  compute problem — and the compute paragraph never mentions it.
- **The multi-shard DB gap is structural, not a missing flag.**
  `wo_engine_start` (`runtime/src/vm.c`) `memset`s each worker VM to zero, so
  `rt.db` and `rt.wal` are NULL by construction off the primary;
  `wo_builtin_db` then returns `WO_T_DB "database engine not initialized"`. It
  is a clean trap rather than a crash — but any multi-shard program that
  touches the database is broken today.

### Confirmed, with corrections to the numbers

| Claim | Evidence |
| --- | --- |
| interpreted only, no JIT, no AOT | no `jit` anywhere; `specs/2026-08-01-oop-compiler-vm-design.md` records AOT-to-C as rejected |
| no `Float`, no `Bytes` | absent from `compiler/src/types.ml`; no float builtin; `net.read` returns `Text` |
| no SIMD | nothing in `runtime/src` or `compiler/src` |
| fixed interpreter ceilings | **mislabeled**: 4096 is the value **stack** (`WO_STACK_SLOTS`), registers are 64 per frame (`WO_MAX_REGS`), frames 256 (`WO_MAX_FRAMES`) — all `runtime/src/wob.h`. Unlisted: `WO_MAX_SHARDS 64`, `WO_ARENA_MAX_CLASS 1024`, `WO_MAX_CATCH 64`. ~42 KiB/fiber matches the arc spec |
| no generics | `multi T` / `map<K,V>` are runtime-provided native classes by design |
| no function values or closures | no such form in the lexer keyword set or typechecker |
| byte-based strings, no Unicode | `WO_B_BYTE_AT`, ASCII `WO_B_TO_LOWER`; `json.c` decodes BMP only |
| no Result-style errors | traps + `try`/`catch` only |
| pattern matching is a switch | `KwSwitch`/`KwCase`; no destructuring form |
| `send` is one-way | `WO_B_SEND=69` is the last builtin (`WO_B_MAX 69u`) — no ask/reply opcode |
| no supervision, links, actor death | nothing in the runtime |
| unbounded mailboxes, no backpressure | `vm.h`: `msgs` is a "FIFO ring, growable"; `mcap` only grows |
| no timers beyond sleep | `time.sleep` is the only one; no `timerfd` in the runtime |
| round-robin placement, no work stealing | `eng_rr` cursor, `vm.c` |
| accept on one shard | one listener, `SO_REUSEADDR` only — no `SO_REUSEPORT` |
| no TLS | the only `tls` in the runtime is thread-local storage (`wo_tls_vm`) |
| no HTTP/2 or WebSockets | framework `http/` is parse/serve/types/auth/multipart; h2c parked per `00-status.md` |
| no crypto primitives | none |
| observability is print/stderr | `WO_B_PRINT`, `PRINT_INT`, `PRINT_ERR`; no counters, tracing, or profiler |
| no debugger, no LSP | neither exists |
| deps git-rev only | no semver, registry, or transitive resolution in the compiler |
| blue-green + migrations are futures | iteration 26 still pending |
| 22's battery never run | 22 is ⬜ "needs a spec first"; no `bench/baseline.json`, no `just db-bench`; `runtime/bench/` is the retired C prototype's harness |
| TSan covers one demo | only `scripts/fibers-accept.sh` builds and runs `wovm_tsan` |
| no fuzzing, no CI | no `.github/`, no fuzz target |
| one framework, five samples, one consumer | exact: `writeonce-serve`; employee, employee-list, fibers, gc-cycle, log-watcher; `web-app` |

### Consequence

The iteration order in
[`stories/language-runtime-database/00-story.md`](stories/language-runtime-database/00-story.md)
was re-sequenced against these findings on 2026-08-20 — Seq only, no `#`
renumbered, no file moved. See that table's second re-sequencing note.

---

## Re-verification 2026-08-26

Re-run against the tree, reading source rather than documents. **Eight rows
have been overtaken by shipped work; the rest still hold.** Overtaken:

| 2026-08-20 row | What the source says now |
| --- | --- |
| "no `Float`, no `Bytes`" | `types.ml`'s `builtin_scalars` is `["Int"; "Bool"; "Text"; "Timestamp"; "Id"; "Float"; "Bytes"]` — iteration 19, plus the `float`/`trunc` bridges and the `bytes_*`/`base64_*` builtins |
| "`send` is one-way — `WO_B_SEND=69` is the last builtin (`WO_B_MAX 69u`)" | `WO_B_MAX` is `95u`; `WO_B_CALL = 88` is a send that parks the caller for a typed scalar reply (iteration 24, WO-E226) |
| "no crypto primitives" | `WO_B_SHA1 = 85`, `WO_B_SHA256 = 86`, `WO_B_HMAC_SHA256 = 87`; `runtime/src/crypto.c`, vector-accepted in `test_crypto.c` (iteration 34) |
| "unbounded mailboxes, no backpressure" | mailboxes are capped (`WO_MAILBOX`, default 1024) with a sender-side reserve and a catchable `WO_T_ACTOR` trap on overflow |
| "no supervision, links, actor death" | **partly** overtaken: actor death landed with `call` — a dead or mid-call callee traps the caller instead of hanging it. `monitor` (id 89) and `time.after` (id 90) are still literal holes in the builtin enum; supervision trees remain absent |
| "22's battery never run — no `bench/baseline.json`, no `just db-bench`" | `bench/baseline.json` exists with the campaign's metrics, `just db-bench`/`db-bench-quick` are recipes, `bench/results/` holds the runs, iteration 22 is done |
| "no fuzzing, **no CI**" | `.github/workflows/release.yml` builds, verifies and publishes on a `v*` tag. Fuzzing is still absent, and CI is release-only — nothing runs the gates per change, which is iteration 30's remaining half |
| "one framework, five samples, one consumer" | two libraries (`writeonce-serve`, `writeonce-view`) and 13 samples, 8 of them gated |
| "The multi-shard DB gap is structural" (Understated) | closed by the arc's stage 3: the string `"database engine not initialized"` no longer exists in `runtime/src/`, worker statements marshal to the owner shard, and `just db-actor` gates it |

Still true, re-checked at the source: interpreted-only with no JIT and no SIMD;
the ceilings correction (`WO_STACK_SLOTS 4096`, `WO_MAX_REGS 64`,
`WO_MAX_FRAMES 256`, `WO_MAX_SHARDS 64`); no generics beyond `multi`/`map`; no
closures or function values; byte strings with no Unicode awareness; traps and
`try` instead of Result values; `switch` without destructuring; round-robin
placement with no work stealing; no timers beyond `time.sleep`; no TLS; no
HTTP/2; observability is `print`/stderr with no counters, tracing or profiler; no
debugger and no LSP; deps are git-rev-only with no registry, semver or transitive
resolution; blue-green and migrations are futures; TSan covers one demo. And
**`map<K,V>` lookup is still a linear scan** — `runtime/src/cont.h` says so in
its own header comment, which keeps it the compute problem this document argued
it was.

Two capability gaps this re-run named that the original critique did not, now
[iteration 38](stories/language-runtime-database/38-content-platform-capabilities.md):
`fs` has six builtins (ids 40–45) and can create, grow and read a file but never
replace, truncate, delete or rename one; and there is no `net.connect` anywhere
in `runtime/src/`, so no program can open an outbound connection.
