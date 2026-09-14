---
track: porch
iteration: "10"
status: pending
readiness: refine
---

# porch 10 — memory features over `@table`: TTL cache, feature flags, durable jobs

> Part of [Story — `porch`, the writeonce web framework](00-story.md).
> **Stub, created 2026-09-11** by the language-18 re-brainstorm (fork 1): the
> three `.wo` pieces of
> [language 18](../language-runtime-database/18-memory-db-features.md) moved
> here; 18 keeps the language + engine half, `transaction { }`. Their spec
> text is Part B of
> [`2026-08-20-memory-db-features-design.md`](../../superpowers/specs/2026-08-20-memory-db-features-design.md);
> its jobs section is superseded (below).

## Carried over from 18, already settled — do not re-brainstorm

1. **Cache**: pure `.wo`, TTL + capacity, lazy expiry on read, FIFO eviction
   over LRU (stated tradeoff), Text values (no generics), a time-injected
   seam (`get_at`/`put_at`) so the fixture injects stamps instead of sleeping.
2. **Flags**: `@table(name: "wf_flags")` with `name @unique` and an Int 0/1
   `on`; a read-through map filled on first read, updated in the same call as
   the write. Framework tables carry the `wf_` prefix.
3. **Job row**: `wf_jobs` — `kind`, `payload` (Text, json), `attempts`,
   `not_before` (wall ms, 0 = due). `enqueue` is an ordinary `insert` and is
   what goes inside the business write's `transaction { }`.

## Re-settled by 18 on 2026-09-11 (fork 2) — the execution model

Drain-on-request and the `Dispatcher.idle()` seam are retired: they assumed
"no timers", which has been false since iteration 24. A job runner is an
**actor** the app spawns; the app `send`s it a poke *after* the block's
closing brace (a `send` inside the block is WO-E111 by design — the row must
be durable before anyone acts on it); the runner queries due jobs
(`not_before <= time.now`, `take budget`), deletes on success, bumps
`attempts` on failure, and re-arms itself with `time.after` for the earliest
`not_before` (one-shot, generation-counter idiom). The table is the durable
outbox; the poke is a hint — at boot the app pokes once and every survivor
runs without a request. The engine provides nothing new.

## To refine here (what makes this `ready`)

- Budget per drain and the backoff shape (app-side in v1 per the spec — keep
  or move into the runner?).
- Where the runner lives (porch-owned actor class vs app-owned satisfying an
  interface — the `Jr`/`Mw` wrapper pattern).
- The web-app demo and gate legs (order + `confirm` job in one block,
  `kill -9` after the POST, restart, the job runs; flags across restart) —
  these need 18's T1–T6 landed first.
- Whether porch 1's pool actor is the runner's home (serialize through the
  same actor that owns store writes) or a second actor.

## Needs

Language 18 (`transaction { }`) for the demo's headline; nothing else beyond
what `spawn`/`send`/`time.after` and `@table` already provide.
