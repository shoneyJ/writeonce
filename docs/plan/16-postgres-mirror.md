# 16 — PostgreSQL mirror: RAM-authoritative database, Postgres as the backup

> **Kanban: 🔄 in progress (Track 3 — Storage & durability)** — 16a ✅, 16b ✅ shipped; 16c–16f ⬜. Board: [00-kanban.md](00-kanban.md)

**Context sources:** [`README.md` § persistent database](../../README.md) (the product goal this implements: *"reads and writes database to RAM, persist data to postgres SQL"*), [`../runtime/database/03-inmemory-engine.md`](../runtime/database/03-inmemory-engine.md) (RAM-resident doctrine: disk sits behind the read path, never in front), [`./09-concurrency-scaleout.md`](./09-concurrency-scaleout.md) (per-shard WAL + ack-after-fsync this rides behind), [`./13-class-model-live-pricing.md`](./13-class-model-live-pricing.md) (the Product/Price worked example; `@table(name: "prices")` names the mirrored table), [`../runtime/database/07-wo-seg-migration.md`](../runtime/database/07-wo-seg-migration.md) (the dual-write precedent), `reference/postgresql/` (research symlink — `src/include/libpq/` for the wire protocol), PostgreSQL docs *Frontend/Backend Protocol*.

## Context

The engine is RAM-resident by design and durable through its own per-shard WAL (09c). What's missing is an **external, queryable, operator-friendly copy** of the data — something a DBA can point `psql`, Grafana, or a nightly `pg_dump` at. That's what Postgres is here: **a backup mechanism**, not a storage engine.

The contract, in one line each:

- **The entire database lives in RAM.** Reads never touch Postgres — ever.
- **Writes go to RAM (+ WAL) and to Postgres** — but the Postgres write is asynchronous: the client's ack gates on the WAL fsync exactly as before; the mirror follows behind.
- **Postgres is disposable.** RAM is authoritative, so backup repair is always "re-push RAM state" — never a merge.

Worked example throughout: `Product.set_price(amount)` from the [pricing demo](../examples/pricing/) → a `Price` row in RAM → the same row visible in `psql` as `SELECT * FROM prices`.

## Design decisions (locked)

1. **Async mirror, never a commit path.** The `wo-pg` thread is downstream of the commit: shard engines clone committed mutations onto a bounded channel (`try_send` — a full channel drops loudly, it never blocks a worker). Postgres being down costs clients nothing.
2. **Mirror what RAM holds.** The tap emits full post-merge rows (`MirrorRec::Upsert{ty, id, row}` — unlike `WalRec::Update`, which carries only the merge body), so a Postgres row is always byte-equivalent to its RAM row. Method transactions (13b) mirror as one `MirrorRec::Txn` → one `BEGIN…COMMIT` — an aborted method never reaches the channel at all.
3. **Hand-rolled wire client, zero new crates.** `crates/rt/src/pg.rs` speaks protocol v3 (startup, auth `trust`/`password`/`md5` with a hand-rolled MD5 — the CRC32 precedent; SCRAM is 16f) over a blocking `std::net::TcpStream`, **simple query protocol only**. Blocking is fine: the only caller is the dedicated mirror thread.
4. **One `wo-pg` thread per process.** N shard senders → one receiver; per-row ordering is preserved because a row's mutations always come from its owner shard (one FIFO sender). Batches drain opportunistically; statement errors are isolated per record (logged, skipped), socket errors reconnect with capped backoff.
5. **Boot = full resync.** Engines attach the mirror AFTER WAL replay and push their entire state as upserts (`mirror_sync_all`). Consequence: restarting `wo` against a fresh/empty/behind Postgres converges it — verified live (a record dropped during an outage reappeared after restart).
6. **Schema 16b: one JSONB table per type** — `"<storage_name>" (id BIGINT PRIMARY KEY, row JSONB NOT NULL)`, named by `@table(name: "prices")` (default: the type name). Upsert = `INSERT … ON CONFLICT (id) DO UPDATE`. Typed columns are 16c.
7. **Config: `WO_PG=postgres://user[:pass]@host[:port]/db`** env var (the `WO_DATA`/`WO_LISTEN` convention). Unset = mirror off, zero cost.
8. **The WAL stays the recovery source; Postgres is the backup of last resort.** Boot replay reads the WAL as today; restoring FROM Postgres (WAL lost) is 16e's explicit opt-in.

## Sub-phase sequence

### `16a` — hand-rolled wire client — ✅ shipped

`crates/rt/src/pg.rs` (~450 lines, stdlib only): `PgConfig::from_url`, `Conn::connect` (startup + auth trust/cleartext/md5, RFC-1321 MD5 hand-rolled with test vectors), `simple_query` (RowDescription/DataRow/CommandComplete/ErrorResponse/ReadyForQuery; a backend error drains to ready so the connection stays usable), `escape_literal`/`escape_ident`.
**Exit (met):** unit tests (MD5 vectors, URL forms, escaping) green; gated integration test (`WO_PG_TEST=…`) round-trips DDL/upsert/select/error-recovery/multi-statement against `postgres:16`; md5-auth container connects with the right password and fails cleanly with the wrong one.

### `16b` — the mirror pipeline — ✅ shipped

`crates/rt/src/mirror.rs`: `MirrorRec{Upsert, Delete, Txn}`, `spawn(cfg, rx, tables)` → the `wo-pg` thread (DDL bootstrap per (re)connect, batched apply, per-record error isolation, capped-backoff reconnect, loud drop accounting). Engine tap (`crates/rt/src/engine.rs`): `attach_mirror`, `mirror_send` (txn-buffered like the WAL buffer — dispatched as one `Txn` on commit, dropped on abort), `mirror_sync_all` boot push; taps sit AFTER `wal_log` accepts in `create`/`update`/`delete`/`commit_txn`. Wiring in `bin/wo.rs` behind `WO_PG`.
**Exit (met, verified live on 2 shards):** `set_price` rows appear in `psql` under the `@table` name `prices` with full JSONB; the abort case (`amount: 0` → 409) mirrors **nothing**; `docker stop` mid-writes → writes keep acking 200, reads unaffected; fresh empty container → reconnect + DDL bootstrap + new writes flow; `wo` restart → WAL replay + bulk sync **heals the gap** (the row written during the outage appeared). 69 unit tests green; blog/ecommerce/hello unchanged without `WO_PG`.

### `16c` — typed schema projection

Catalog scalar fields become real columns (`Id`/`Int`/`Money`/`ref` → `BIGINT`, `Text`/`Timestamp`/unions → `TEXT`, `Bool` → `BOOLEAN`; arrays/structs stay in a residual `row JSONB`); `@table(index: [product, at])` → `CREATE INDEX IF NOT EXISTS`; schema evolution via `ADD COLUMN IF NOT EXISTS`.
**Exit:** `SELECT avg((row->>'amount')::bigint)` becomes `SELECT avg(amount) FROM prices WHERE product = 1` in psql, using the mirrored index.

### `16d` — failure & lossless resync

Replace drop-and-log with dirty-flag repair: channel overflow or reconnect marks shards dirty; workers re-enqueue their tables at tick (the 09b mail-eventfd mechanism), so convergence no longer waits for a process restart. Mirror lag + queue depth + drop counters surface on `GET /`.
**Exit:** kill Postgres under sustained write load, restart it → row counts converge with zero client errors and no `wo` restart.

### `16e` — restore from Postgres

Boot source of last resort when the WAL is gone: `WO_PG_RESTORE=1` makes each shard `SELECT id, row FROM …` its own partition (`(id-1) % n = shard`) before arming accept, seeding RAM and re-logging a fresh WAL.
**Exit:** `rm -rf wo-data` → boot with restore → `current_price` answers from the restored state; id high-water marks keep the interleave.

### `16f` — SCRAM-SHA-256 auth

Hand-rolled SHA-256 + HMAC + PBKDF2 (RFC 7677 exchange) so stock `postgres:16` works without `pg_hba` changes.
**Exit:** connects to an out-of-the-box scram-auth server; wrong password fails with the server's error.

## Verification (16a/16b, reproducible)

```bash
docker run -d --rm --name wo-pg -e POSTGRES_HOST_AUTH_METHOD=trust -e POSTGRES_DB=wo -p 54329:5432 postgres:16
WO_PG_TEST=postgres://postgres@127.0.0.1:54329/wo cargo test --lib -- pg_ mirror_   # integration tests
just pricing-pg-demo                                                                # scripted end-to-end
```

| Check | Result |
| --- | --- |
| `set_price 4999/5999` → `psql: SELECT * FROM prices` | rows present, full JSONB, `@table` name honoured |
| Aborted method (`amount: 0` → 409) | nothing in Postgres — only committed txns mirror |
| Postgres stopped mid-writes | writes ack 200, reads unaffected, mirror retries with backoff |
| Fresh empty database on reconnect | DDL bootstrap recreates tables, stream resumes |
| `wo` restart against behind/empty Postgres | WAL replay + boot sync converges it (outage gap healed) |
| No `WO_PG` | zero behavioural change; 69 unit tests green |

## Non-scope

- **No reads from Postgres on any serving path** — doctrine; even 16e's restore happens before accept is armed.
- **No TLS** to Postgres (mirror a local/private endpoint; revisit with 16f).
- **No extended query protocol / prepared statements** — simple protocol is enough for a backup writer; revisit only if 16c profiling demands it.
- **No two-way sync / conflict resolution.** Postgres is write-only from writeonce's perspective (16e restore excepted); external writes to the mirrored tables are unsupported and will be overwritten.
- **No dependency creep.** `crates/rt` gains no crates for this plan — the wire client is part of the same hand-rolled surface as the HTTP layer.

## Cross-references

- [`./10-storage-foundations.md`](./10-storage-foundations.md) / [`11`](11-wal-and-recovery.md) / [`12`](12-engine-disk-cutover.md) — the native disk engine; the mirror is orthogonal (external queryable backup vs. native durability) and both sit behind the RAM read path.
- [`./13-class-model-live-pricing.md`](./13-class-model-live-pricing.md) — `@table(name:)` names the mirrored tables; 13b method txns map to Postgres txns; the pricing demo is the acceptance workload.
- [`../runtime/database/07-wo-seg-migration.md`](../runtime/database/07-wo-seg-migration.md) — the dual-write pattern precedent.
- [`./15-mcp-streamable-http.md`](./15-mcp-streamable-http.md) — the other "speak an established protocol, hand-rolled" track; 16a is to Postgres what 15a is to MCP.
