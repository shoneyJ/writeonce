# log-watcher `.wo` sample (docs-first) + principles doc — design spec

**Date:** 2026-08-07
**Status:** approved design, pre-implementation
**Scope:** authoring two docs artifacts — the `docs/examples/log-watcher/` sample and `docs/00-principles.md`
**Companion specs:** [`2026-08-01-systems-track-design.md`](2026-08-01-systems-track-design.md) (language surface + stdlib + sample mapping), [`2026-08-01-oop-compiler-vm-design.md`](2026-08-01-oop-compiler-vm-design.md) (OOP core, memory model), [`2026-08-03-blue-green-vm-design.md`](2026-08-03-blue-green-vm-design.md) (in-runtime deployment)
**Companion plan:** [`docs/superpowers/plans/2026-08-01-log-watcher-sample.md`](../plans/2026-08-01-log-watcher-sample.md) (plan 10 — the executable acceptance plan this authoring feeds)

## Motivation

The systems track names `~/projects/log-watcher` (~1,200 lines of Haxe, a
single-binary Linux daemon) as its driving workload and defines the sample
port in Part 4 of its spec. Plan 10 sequences that port but gates on plans 8
(language adoptions) and 9 (stdlib) shipping, because its acceptance requires
compiling fixtures. This session does not wait: it authors the sample as a
**docs-first artifact** — the same pattern by which `docs/examples/blog/` and
`docs/examples/ecommerce/` existed before the runtime executed them and
thereby forced the grammar. Alongside it, the repo gains its first canonical
principles document, `docs/00-principles.md`, distilling doctrine currently
scattered across specs, plan docs, and CLAUDE.md.

## Decisions locked during brainstorming

| Question | Decision |
| --- | --- |
| Session deliverable | **Sample `.wo` files + principles doc now, docs-first.** No compiler/runtime code; fixtures stay with plan 10. Rejected: waiting for plans 8/9; revising approved specs; starting compiler implementation. |
| principles doc placement | **Repo-level:** `docs/00-principles.md`. The example README links to it. Rejected: per-example principle file. |
| Sample scope | **Approach A + short deploy note:** all 8 files per plan 10's structure, strict approved syntax, README mapping table, plus one README paragraph linking the blue-green spec. Rejected: minimal 3-file sample (breaks the mapping, leaves could-not-express unproven); full speculative `wo remote` walkthrough. |

## Section 1 — Scope & positioning

**In scope:** `docs/examples/log-watcher/` — eight `.wo` files plus an
orientation README — and `docs/00-principles.md`.

**Out of scope:** corpus fixtures, `just` recipes, compiler or runtime code,
kanban restructuring. Plan 10 keeps ownership of fixtures and of acceptance
criteria 4–5 (empty could-not-express column verified by a real compile; live
silent-death detection) when plans 8/9 ship.

**Authoring contract:** every construct used in the sample must be traceable
to an approved surface — the OOP spec's section 3 (classes, interfaces,
methods, MVS parameter conventions), the systems-track verdict table's adopt
rows (switch expressions, typedef records, `?T` optionals, enum payloads,
try/catch/throw, `static fn`, `using`, `use` modules, `pub`, abstracts,
interpolation, `#if`), Part 2 program mode (`fn main`, blocking builtins,
`env.stopping()`), and Part 3's five stdlib modules (`fs`, `proc`, `net`,
`time`, `json`). No construct may be invented here. If the port cannot
express a behavior inside that surface, that is a **defect report against
plans 8/9**, recorded in the README's could-not-express column — the same
feedback-loop role plan 10 assigns, run early.

**Behavioral reference:** the Haxe source at `~/projects/log-watcher/src/`.
The port is behavior-faithful, not line-faithful; each `.hx` file is read
before its `.wo` sibling is written. Deliberate divergences (optionals over
sentinels, records, switch expressions, RAII handles, the stopping-flag
daemon idiom) are recorded in the README table, never silent.

## Section 2 — The sample

File set and content, matching plan 10's structure:

| `.wo` file | `.hx` sibling | carries |
| --- | --- | --- |
| `logtail.wo` | LogTail.hx | `TailState` record; bounded `fs.read_at` tail polls (never front-to-back); rotation restart on inode change or shrink; burst jump to tail; torn-final-line holdback; line classification by level prefix incl. timestamped app-log lines |
| `watcher.wo` | Watcher.hx | the alert rule: last entry `error` + quiet period elapsed → alert transition; injected clock parameters |
| `cron.wo` | Cron.hx | cron.d entry parse (five-field schedules, user column); `>> logfile` redirection extraction (the zero-config watch derivation); same-log collapse; next-fire computation; unreadable directory reported as skipped data, never a throw |
| `probes.wo` | Flock.hx, Pgrep.hx | `static fn` probes over `proc.run`: flock exit-1-means-held with exists-guard, pgrep exit-0-means-alive, unknown codes falling in the safe direction |
| `supervisor.wo` | Supervisor.hx | single-threaded tick loop; rescan interval; pre-fire lock probes (PROBE_LEAD); watch activation/completion; error-only JSONL detections through `fs.append`; daemon shape `while !env.stopping() { tick; time.sleep }` |
| `mcp.wo` | Mcp.hx | typed request/response records; **pure `handle(req) -> resp`, socket-free** (the original's best design decision, preserved); Bearer auth first; method/path/size gates; JSON-RPC envelope (initialize, ping, tools/list, tools/call; notifications answered 202); serve loop over `net.listen`/`accept` on 127.0.0.1, one request per connection |
| `tools.wo` | Tools.hx | tool subset over shipped capability: `list_logs`, `tail_log`, `search_log` as bounded windows over `fs.read_at`; the sqlite-backed minilog tools are scoped out to the DB track (recorded as scoped-out, not could-not-express) |
| `main.wo` | Main.hx | `fn main(args: multi Text) -> Int`; subcommands `watch` / `run` / `mcp`; config decoded via `json.decode … as` into a record with `?fields`; usage text and exit codes on bad invocation |

**README.md** (orientation only, per repo docs rule):

- the mapping table above with two more columns: **divergences** (each
  deliberate `.wo`-idiom improvement) and **could-not-express** (defects
  against plans 8/9; target empty);
- a status line: authored ahead of the compiler; plan 10 verifies by
  compilation and fixtures when plans 8/9 ship;
- one paragraph linking the blue-green spec: this daemon is the shape of
  program the runtime updates in place — propose, approve, atomic switch,
  resident rollback — via `wo remote`, once that subsystem ships;
- a pointer to `docs/00-principles.md`.

## Section 3 — `docs/00-principles.md`

One page; each principle is a short statement, a one-line why, and a link to
the spec or doc that enforces it. The thirteen principles (the thirteenth
added 2026-08-08 by story amendment):

1. **One binary is the whole system.** App, database, API, and UI ship as a
   single deployable; there is nothing else to operate.
2. **Zero dependencies — kernel primitives only.** libc-only C runtime,
   stdlib-only OCaml compiler; epoll/io_uring, inotify, signalfd are the
   framework.
3. **Memory safety without a GC tax.** Mutable value semantics: single
   owner, second-class borrows (the Rust-borrow shape without lifetime
   inference), hybrid static+runtime enforcement; `@gc` is a per-class
   opt-in collected per shard with no global pause.
4. **No inheritance, ever.** Composition, structural interfaces, and tagged
   unions; no `extends`, no `override`, no virtual hierarchies.
5. **Thread-per-core shards; ownership moves, data never shares.** Cross-
   shard communication is a message send; no shared mutable engine state.
6. **The runtime never stops.** Blue/Green VM slots, in-runtime compile,
   atomic dispatch switch, resident rollback; the binary embeds its own
   source.
7. **RAM is authoritative; the WAL makes it durable.** Ack after fsync;
   mirrors (Postgres) are reconstructible backups, never a commit path.
8. **Samples force the grammar.** Examples are the de facto integration
   tests; a feature exists when a sample exercises it.
9. **Linux is the target.** The kernel is the substrate, not an abstraction
   boundary to hide.
10. **Capabilities are typed builtins.** No FFI, no shell strings, no
    escape hatches; the read-only posture is the default posture.
11. **Plain diagnostics are the product.** Stable error codes, two-site
    ownership messages; MVS only beats Rust ergonomics if the errors are
    plain.
12. **The runtime is a recipe box.** Web frameworks and databases arrive
    later as `.wo` libraries composing separable runtime capabilities, not
    as monoliths.
13. **Statically typed, all the way to the register.** No `Dynamic`, no
    `untyped`, no `cast`; untagged VM registers because the compiler knows
    every slot's type; `@`-annotations are the compile-time ORM.

Placement note: `docs/` currently starts at `01-problem.md`; `00-` is free
and reads as "start here". CLAUDE.md's "Where to read next" gains one line
pointing at it (smallest possible touch).

## Section 4 — Error handling (in the authored artifacts)

The sample demonstrates the approved error doctrine rather than inventing
one: optionals (`?T`) for expected absence (missing stat, failed decode,
missing env var), try/catch over traps for genuine faults, `throw` only
where the original throws. The probes' Haxe `catch (e:Dynamic) return false`
idiom becomes optional-returning calls — a README-tabled divergence.

## Section 5 — Verification (docs artifact, this session)

- **Surface audit:** every construct in every `.wo` file traceable to a
  verdict-table row, OOP spec section 3, or Part 2/3 of the systems spec;
  anything else is removed or logged as could-not-express.
- **Behavior audit:** every `.wo` function names its `.hx` source behavior;
  the mapping table is complete (eight rows, no blank cells).
- **Principles audit:** every principle's link resolves to an existing doc;
  no principle contradicts a locked decision.
- **Spec self-review** per the brainstorming skill, then user review.
- Compilation, fixtures, and the live silent-death scenario remain plan 10
  acceptance — explicitly not claimed here.

## Success criteria

1. `docs/examples/log-watcher/` holds the eight `.wo` files and README; the
   mapping table's could-not-express column is empty or contains only
   defect reports filed against plans 8/9.
2. Every construct used is traceable to approved specs (surface audit
   passes).
3. `docs/00-principles.md` exists with the thirteen principles, each linked
   to its enforcing doc; CLAUDE.md points at it.
4. The example README links the blue-green spec and the principles doc.
5. Nothing outside `docs/` and CLAUDE.md is touched; no fixtures, no code.
