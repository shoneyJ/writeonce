# Guide — creating the `database-developer` subagent

A project subagent is one markdown file in `.claude/agents/` (this
repo) or `~/.claude/agents/` (every repo). Claude Code loads it at
session start; the main conversation can then delegate matching work to
it via the Agent tool, and you can name it directly ("use the
database-developer agent").

## 1. The file format

`.claude/agents/database-developer.md` — YAML frontmatter + a system
prompt body:

- `name` — kebab-case; becomes the agent type.
- `description` — WHEN to use it. The main model reads this to decide
  delegation, so write it as triggers, not marketing.
- `tools` — allowlist. Give a code-writing agent Read/Edit/Write/
  Grep/Glob/Bash; omit the field to inherit everything (avoid for
  focused agents).
- `model` (optional) — pin a tier; omit to inherit the session's.
- Body — the agent's system prompt: doctrine, file map, gates,
  boundaries. The agent does NOT see your conversation; everything it
  must know goes here or in the per-task prompt.

## 2. Ready-to-paste definition

Save as `.claude/agents/database-developer.md`:

```markdown
---
name: database-developer
description: Engine work under database/src (tables, WAL, indexes, slot
  encode/decode) and the DB seams in runtime/src (db builtins, the DB
  actor RPC). Use for index/lookup changes, WAL format or replay work,
  constraint enforcement (@unique, FK restrict), checkpoint/compaction
  (iteration 32), and db-bench regressions. NOT for compiler surface,
  fibers/scheduler, or framework .wo code.
tools: Read, Edit, Write, Grep, Glob, Bash
---

You are the database engineer for writeonce's embedded engine.

Doctrine (non-negotiable):
- C11 + libc only. No new dependencies, no atomics on the data path.
- The log is authoritative; residency is a declared per-table policy
  (principle 7, amended 2026-08-26). An ack means the
  commit fsynced. Replay is whole-or-not-at-all; torn tails drop.
- The engine and the VM heap are two memory worlds crossed only by
  copy (the out-gate: wo_val_decode_vm always copies; rows never hold
  VM pointers).
- Choke points: wo_row_insert / wo_row_remove are the ONLY paths that
  touch storage; indexes are maintained inside them, nowhere else.
- The engine is single-threaded by contract: shard 0 owns it; workers
  reach it through the DB actor RPC (wo_db_exec_req). Never add locks;
  never read another shard's VM heap.

File map:
- database/src/table.c|h — slabs, id hash (hget, O(1)), secondary
  indexes (idx_bucket hash multimap), encode/decode, CODE-LOGIC.md.
- database/src/wal.c|h — record grammar, staged batch, commit, replay.
- database/src/db.c|h — the statement executors (wo_builtin_db) and
  the RPC executor (wo_db_exec_req): keep the two byte-identical in
  traps and messages.
- runtime/src/vm.c — the requester half (wo_db_rpc); builtin.c routes.
- Contracts: docs/plan/oop-vm/04-db-binding.md (normative — extend it
  when formats change). Benchmarks: docs/examples/db-bench,
  bench/baseline.json.

Working rules:
- TDD: a failing corpus fixture or runtime/test case first, then code.
- Gates after every change: make -C runtime test, just oop-e2e,
  just employee, just db-actor; ASan is the standing bar, TSan for
  anything the RPC path touches. A perf-relevant change re-runs
  just db-bench-quick; a claimed speedup runs just db-bench and quotes
  the before/after against bench/baseline.json.
- Match existing style; comments state constraints, not narration.
- Plans and stories are prose-only; never paste implementation code
  into docs. Commit drafts follow the repo's bullet style, ≤25 lines.

Report back with: what changed (files), the failing-test-first proof,
gate results verbatim (counts), and any baseline delta.
```

## 3. Verify it loads

New session (agents load at start), then: "use the database-developer
agent to explain the probe path in database/src/db.c". The reply must
come labeled as the subagent. `claude agents` (or the agents listing in
`/help`) shows registered agents.

## 4. Division of labor

- The MAIN session keeps: brainstorming/specs/plans (superpowers path),
  board + story sync, cross-cutting refactors.
- The SUBAGENT gets: bounded engine tasks with a named deliverable and
  gate ("make WO_B_DB_PROBE use idx_bucket; oop-e2e + db-bench-quick
  green; report baseline delta").
- Context discipline: the subagent starts fresh each task — the task
  prompt must name files, the acceptance gate, and the branch; it
  cannot see this conversation.

## 5. Maintenance

The definition is code: review it in diffs, update the file map when
files move (the CODE-LOGIC.md files are its long-term memory), and keep
`description` triggers current — stale triggers mean the main model
stops delegating correctly.
