# docs/plan/oop-vm/ — OOP + systems track contracts

The normative contract documents both stacks cite. Landed by their named plan tasks:

| doc | contract | plan |
| --- | --- | --- |
| `00-wob-format.md` | `.wob` bytecode format (compiler↔VM) | 1 |
| `01-error-catalog.md` | every `WO-E###` code | 2, grows 3/8 |
| `02-corpus.md` | how to add conformance fixtures | 3 |
| `03-shard-actor.md` | shard ownership, mailboxes, send-as-move | 4 |
| `04-db-binding.md` | row format, WAL records, query subset | 5 |
| `05-http-service.md` | route section, trap→HTTP table, JSON subset | 6 |
| `06-ui-live.md` | delta frames, subscribe protocol, wo:live | 7 |
| `07-systems-stdlib.md` | per-function nil-vs-trap contracts | 9 |

Specs and plans: `docs/superpowers/{specs,plans}/`. Repo map: `docs/08-project-structure.md`.
