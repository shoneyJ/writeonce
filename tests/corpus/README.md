# tests/corpus/ — the conformance spine

Fixture kinds, exact-outcome matching (byte-equal stdout / exact `WO-E###` / exact trap code / exact gc step+freed counts):

| dir | kind | landed by plan |
| --- | --- | --- |
| `run/` | compiles + runs, expected stdout | 3 |
| `compile-fail/` | must fail with expected `WO-E###` | 3–4 |
| `trap/` | must trap with expected code | 3–4 |
| `gc/` | cycle collection scenarios | 3 |
| `actor/` | shard/spawn/send (ASan+TSan) | 4 |
| `db/` | insert/select + crash/replay | 5 |
| `lang/` | Haxe-parity adoptions | 8 |
| `sys/` | fs/proc/net/time/json stdlib | 9 |
| `sample-logwatcher/` | ported log-watcher fixtures | 10 |

Runner: `scripts/oop-e2e.sh` (plan 3, task 2 — includes the how-to-add-a-fixture doc pointer `docs/plan/oop-vm/02-corpus.md`).
