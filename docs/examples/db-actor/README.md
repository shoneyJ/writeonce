# `db-actor` — the database reached from any shard

> **Status: shipped — arc stage 3's acceptance gate.** Run it with
> `just db-actor`. Landed 2026-08-21 with the shard-fiber arc
> ([story 8](../../stories/language-runtime-database/08-shard-actor-runtime.md)
> · [plan](../../superpowers/plans/2026-08-20-shard-fiber-arc.md)).

The database lives on **one** shard — the owner, shard 0 — because RAM is
authoritative and a single writer is what makes the WAL's ordering meaningful.
That is a problem the moment actors are placed round-robin across cores: a
`spawn`ed actor has no say in which shard it lands on, and before stage 3 a
worker-shard `insert` trapped `WO_T_DB` with "database engine not initialized".

Stage 3's answer is a **transparent DB actor**: statements issued off the owner
shard marshal to it, execute there, and materialize their replies back. The
program's source says nothing about any of it — the same `insert` and the same
`from … select` work wherever the actor happens to run. This sample exists to
prove exactly that, which is why its acceptance criterion is *placement
independence* rather than any particular output.

## What it does

`Note` is a `@table` with a secondary index on `tag`. `Writer` is an actor: each
one inserts a row, then scans the whole table and prints the sum it sees. `main`
spawns two writers, waits, then scans once itself.

With the default shard count, round-robin placement puts at least one writer off
the owner shard — so one of those inserts and one of those scans travel the RPC
path under test, and the other does not. Both must produce the same shape.

```bash
just db-actor                       # the gate
woc docs/examples/db-actor/         # or build it by hand
WO_SHARDS=1 ./docs/examples/db-actor/target/db-actor   # force the local path
```

## What the gate proves

`scripts/db-actor-accept.sh`, 8 checks:

| Check | Why it is shaped that way |
| --- | --- |
| multi-shard, three rounds | The writer lines are asserted as a **set**, not a sequence — scheduling decides their order, and pinning it would be testing the scheduler, not the RPC. The `main` line is exact. |
| both `WO_IO` backends forced | The reply park has to be plane-independent: io_uring and epoll must give the same answer, or the parking is leaking into semantics. |
| single shard, byte-exact | The local path is untouched by stage 3. Any drift here means the RPC changed the non-RPC case. |
| `WO_DATA` restart pair | A worker's insert must commit on the **owner's** WAL before its ack, so a restart replays it: 2 rows, then 2+2 after a second run. This is the durability claim the RPC could most easily break. |

Run under `wovm_asan` and `wovm_tsan` as well — cross-shard message passing is
exactly where a data race would hide, and TSan covering this demo is the one
place it runs.

## Read it for

- **How little the source knows.** Compare `Writer.receive` here against the
  same statements in [`employee`](../employee/): identical. Transparency is the
  feature.
- **Why `main` waits.** `main` is not an actor and has no mailbox, so it sleeps
  rather than awaiting — the gap iteration 31's `call` closes for actors and
  [24's marker](../../active-slice-2026-08-23-chat-ws-lifecycle.md) tracks.

Reasoning under the engine side: [`database/src/CODE-LOGIC.md`](../../../database/src/CODE-LOGIC.md).
Contract: [`plan/oop-vm/04-db-binding.md`](../../plan/oop-vm/04-db-binding.md).
