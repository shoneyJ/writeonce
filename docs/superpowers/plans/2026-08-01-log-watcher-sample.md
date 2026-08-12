# log-watcher .wo Sample Implementation Plan

> **Status: ⬜ pending** (story iteration 7 — the acceptance gate) — the eight-file `.wo` sample compiles clean and detects a silent death live. Blocked on iterations 5–6. The sample is already authored ([`docs/examples/log-watcher/`](../../examples/log-watcher/README.md)) and currently reports 93 diagnostics, down from 307. Board: [00-status.md](../../00-status.md)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
>
> **Style rule (user convention):** concept, reason, and required behavior in words only; the executor writes the code.

**Goal:** Plan 10 — re-express `~/projects/log-watcher` in `.wo` at `docs/examples/log-watcher/`, file-for-file, ported test fixtures passing — the systems track's acceptance workload (spec criteria 4–5).

**Architecture:** Plan 10 of the roadmap, the proof plan. Depends on plans 8 (language) and 9 (stdlib) complete. No new language or runtime features may land here — a task that cannot express its file has found a defect in plans 8/9 and stops (that feedback loop is this plan's purpose; the repo pattern "samples force the grammar" runs in verification direction now). The Haxe original at `~/projects/log-watcher/src/` is the behavioral reference; its test suite's cases (`test/TestMain.hx`) are the fixture source. The port is behavior-faithful, not line-faithful — `.wo` idioms (optionals over sentinel values, records, switch expressions, RAII handles) where they read better, with the README mapping table recording every deliberate divergence.

**Tech Stack:** `.wo` only, plus corpus fixtures. The MCP scope is the original's hand-rolled subset: stateless Streamable-HTTP, tools-only, no SSE — matching `docs/plan/15-mcp-streamable-http.md`'s neighborhood but implemented in-sample over `net`.

## Global Constraints

- All track constraints carry over (no commits — drafts to `.dev/commit.md`; docs under `docs/`; sample keeps only an orientation README beside code).
- **No new features in this plan** — expressiveness gaps stop the task and report against plans 8/9.
- **Behavior parity is fixture-defined:** every ported fixture states which Haxe test case it mirrors; divergences (improvements included) are README-tabled, never silent.
- **The pure-core discipline is preserved:** tail state machine, cron math, and MCP `handle` stay socket-free and clock-injected, exactly like the original — testability was its best design decision.
- **Acceptance = spec criteria 4 and 5:** empty "could not express" column; live silent-death detection on a real tempfile.

---

## File Structure

```
docs/examples/log-watcher/
  README.md            mapping table: .wo file ↔ .hx sibling ↔ divergences ↔ could-not-express
  main.wo              subcommand dispatch, config decode (Task 5)
  logtail.wo           TailState record + bounded tail poll (Task 1)
  watcher.wo           quiet-period alert state machine (Task 1)
  cron.wo              cron.d parse + next-fire (Task 2)
  probes.wo            flock/pgrep probes as static fns (Task 3)
  supervisor.wo        tick loop, scheduled/active watches, detections sink (Task 3)
  mcp.wo               typed records, pure handle(), serve loop (Task 4)
  tools.wo             the MCP tool implementations over fs (Task 4)
tests/corpus/sample-logwatcher/   ported fixtures per task
```

---

### Task 1: `logtail.wo` + `watcher.wo` — the tail state machine

**Concept & reason:** the heart of the original: `TailState` (offset, inode, last level, last-newline clock) as a typedef record; poll semantics ported exactly — first-sight starts a bounded chunk before EOF, inode change or shrink = rotation restart, burst jumps to tail, torn final line held back, complete lines classified by level prefix (the relaxed timestamp-aware rule the original converged on). `watcher.wo` layers the alert rule: last entry error + quiet period elapsed → alert transition. Both take injected clocks (`now` parameters) — the original's testability discipline. Divergence expected and tabled: `?TailState` and `?stat` optionals replace the `-1`-inode and exists-flag sentinels.

- [ ] Port fixtures from the Haxe suite's tail/watcher groups: first-sight window, rotation by rename, truncate restart, torn-line holdback, level classification incl. timestamped lines, quiet-period alert timing.
- [ ] Write the two files; fixtures green.
- [ ] Record commit draft: `docs(examples): log-watcher port — logtail/watcher (records, bounded read_at polls, rotation by inode, torn-line holdback, injected clocks); tail fixture group green.`

### Task 2: `cron.wo` — cron.d parsing + next-fire

**Concept & reason:** the original's `Cron.hx`: parse `/etc/cron.d`-format entries (five-field schedules, user column, command with `>> logfile` redirection extraction — the zero-config trick that derives what to watch), collapse same-log entries, compute next-fire from a schedule list. Unreadable directory reports as a skipped entry, never a throw (the production fix the original carries — preserved via `?` returns). Switch expressions over field patterns replace the original's if-chains where clearer (tabled divergence).

- [ ] Port fixtures: schedule parsing edges (steps, ranges, lists, weekday names), redirection extraction, same-log collapse, next-fire across day/week boundaries, unreadable-dir skip.
- [ ] Write the file; green.
- [ ] Record commit draft: `docs(examples): log-watcher port — cron.d parse (redirection-derived watch list, unreadable-dir skip as data), next-fire math; cron fixture group green.`

### Task 3: `probes.wo` + `supervisor.wo` — the daemon loop

**Concept & reason:** probes as `static fn`s over `proc.run` — flock's exit-1-means-held with exists-guard, pgrep's exit-0-means-alive, every unknown code falling in the safe direction (the original's comment-documented contract, now in the README table). The supervisor: the single-threaded tick loop verbatim — rescan on interval, pre-fire lock probes with the PROBE_LEAD constant, watch activation/completion, service watches with alert-transition detections appended as JSONL through `fs.append`, all clock-injected. The daemon `run()` wraps tick in the `while !env.stopping() { tick; time.sleep }` idiom — the original's `while(true)` improved by the shutdown flag (tabled).

- [ ] Port fixtures: probe exit-code table; supervisor tick scenarios (activation at fire, skip-locked window, completion pruning, rescan on dir change, detection line shape).
- [ ] Write both files; green.
- [ ] Record commit draft: `docs(examples): log-watcher port — flock/pgrep safe-direction probes, supervisor tick loop (lock-lead probes, JSONL detections, stopping-flag daemon idiom); supervisor fixture group green.`

### Task 4: `mcp.wo` + `tools.wo` — the MCP server

**Concept & reason:** the crown piece: hand-rolled MCP-over-HTTP in `.wo`. Typed request/response records; `handle(req) -> resp` stays a pure function — auth-first Bearer check, method/path/size gates, JSON-RPC envelope (initialize/ping/tools-list/tools-call, notifications answered 202), tool dispatch returning isError results for model-recoverable failures — all decoded/encoded through typed `json` records (the `Dynamic`-free rewrite is the port's most instructive diff). The serve loop: `net.listen` on 127.0.0.1, one request per connection, read with the body cap, write with byte-length framing (the original's UTF-8 lesson holds by construction — lengths are byte lengths in the stdlib). `tools.wo` implements the tool subset that needs only shipped capability: list_logs, tail_log, search_log (bounded windows over `fs.read_at`); the sqlite-backed minilog tools are OUT — tabled as "expressible when the DB track's in-RAM SQL lands", not a could-not-express row (the spec scoped embedded SQL out).

- [ ] Port fixtures from the MCP test group: envelope cases (auth 401, wrong method 405, oversized 413, parse error -32700, unknown method -32601, notification 202), tool-call round-trips, socket-level smoke (scripted client, one connection).
- [ ] Write both files; green.
- [ ] Record commit draft: `docs(examples): log-watcher port — MCP subset in .wo (pure handle() over typed json records, Bearer auth, tools list/tail/search over fs), net serve loop; envelope + socket fixtures green.`

### Task 5: `main.wo` + README + acceptance

**Concept & reason:** close the loop. `main.wo`: the three subcommands — `watch` (single-watcher poll loop), `run` (supervisor + optional config), `mcp` (config + env-fallback API key, required-field errors exit 1 with usage) — config decoded via `json.decode as` into the config record, usage text on anything else. The README mapping table: every `.wo` file, its `.hx` sibling, tabled divergences, and the **could-not-express column — acceptance demands it empty** (criterion 4). The live test (criterion 5): a scripted scenario starts the built sample in watch mode against a tempfile, feeds timestamped lines ending in an error, waits past the quiet period, asserts exactly one detection — the original's measured behavior, reproduced. Acceptance also gains a **diagnostic-count gate**: `woc docs/examples/log-watcher` emits 307 diagnostics today (167 `WO-E101` + 140 `WO-E207` across 7 files) — the pre-port baseline — and that count must fall monotonically from iteration 5 onward, reaching exactly 0 here. `just oop-accept` gains the sample build + fixture groups + the live scenario + the diagnostic-count check; kanban and the systems spec get their shipped-status notes.

- [ ] Write main.wo + README table; port config-loading fixtures (defaults, partial config, missing-key mcp errors).
- [ ] Wire the live scenario + gate; run acceptance: criteria 4 and 5 checked against the spec, plus the diagnostic-count gate (307 → 0).
- [ ] Record commit draft: `docs(examples): log-watcher port complete — main.wo subcommands + typed config, README mapping table (could-not-express: empty), live silent-death scenario in oop-accept, diagnostic count 307 to 0; systems-track criteria 4-5 checked.`

---

## Plan self-review notes

- **Spec coverage (Part 4, criteria 4–5):** all five `.hx→.wo` mappings from the spec's table have tasks; the pure-core discipline, the README table, and both acceptance criteria are explicit task outputs. The minilog/sqlite tools exclusion matches the spec's out-of-scope list and is recorded as scoped-out, not inexpressible. Task 5 also carries the gap-closure amendment's diagnostic-count gate (307 → 0, recorded 2026-08-10).
- **Feedback-loop honesty:** the no-new-features constraint plus stop-on-gap rule makes this plan the verification instrument for plans 8/9 — its failure mode is a defect report, not a workaround.
- **Order rationale:** pure cores first (tail, cron) — testable without any daemon; probes/supervisor next (compose them); MCP after json/net are proven by earlier tasks; main last, wiring everything.
