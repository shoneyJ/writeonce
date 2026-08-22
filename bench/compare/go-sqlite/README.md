# go-sqlite — the comparison harness

Go (`database/sql` + mattn/go-sqlite3, cgo) mirroring
`docs/examples/db-bench`'s schema and modes line-for-line, so the
numbers align column-for-column. Not a gate — a reference point;
SQLite is the honest peer (embedded, single-writer, WAL, same
durability knob).

Run: `go build -o go-sqlite . && ./go-sqlite ram 20000` /
`./go-sqlite durable 20000 <ext4-dir>` — a tmpfs dir makes fsync free
and the durable numbers a lie (measured: 122k/s on /tmp vs 3.1k/s on
ext4; the campaign's own trap, re-confirmed).

## Measured 2026-08-22 (N=20k, same machine, ext4, single-shard vs single-conn)

| metric | writeonce | Go+SQLite | ratio |
| --- | --- | --- | --- |
| ram seed inserts/s | 245,188 | 296,965 | sqlite ×1.2 |
| ram read ops/s (p50µs) | 1,097,574 (1) | 429,645 (2) | **wo ×2.6** |
| ram query ops/s | 989,609 | 154,559 | **wo ×6.4** |
| ram write ops/s | 195,465 | 380,069 | sqlite ×1.9 |
| durable seed inserts/s (p50µs) | 4,460 (~220) | 3,113 (241) | **wo ×1.4** |
| durable write ops/s | 2,324 | 3,257 | sqlite ×1.4 |

Readings, honestly:

- **Reads/queries: writeonce wins 2.6–6.4×** — RAM-authoritative rows +
  the index probe answer without page decoding or a bytecode/VM ↔ cgo
  boundary; SQLite pays B-tree page traversal + the cgo call per op.
- **ram writes: SQLite wins ~1.9×** — writeonce's update path re-runs a
  probe per update (update-through-query) and its insert encodes slots
  per field; SQLite's page write is tight. Registered as target 1 in
  [`docs/plan/perf-targets.md`](../../../docs/plan/perf-targets.md).
- **durable seed: writeonce wins ~1.4×** (append-only WAL + fdatasync
  vs SQLite WAL frame + FULL sync); durable mixed writes flip back to
  SQLite ×1.4 — the update's extra probe again.
- Caveats: different languages (Go harness pays ~1µs cgo per op; wo
  pays its interpreter), both are the honest end-to-end app-visible
  cost of their stack. Single connection vs single shard; no
  concurrency comparison here (SQLite has one writer by design).
