# 10 — Storage Foundations: on-disk row codec + segment append path

**Context sources:** [`./done/04-cutover-remove-tokio-axum.md`](./done/04-cutover-remove-tokio-axum.md), [`../runtime/database/03-inmemory-engine.md`](../runtime/database/03-inmemory-engine.md), [`../runtime/database/07-wo-seg-migration.md`](../runtime/database/07-wo-seg-migration.md), [`./exploration/postgresql/smgr-and-md.md`](./exploration/postgresql/smgr-and-md.md), [`./exploration/postgresql/page-format.md`](./exploration/postgresql/page-format.md), [`./exploration/linux/12-pwrite-fsync.md`](./exploration/linux/12-pwrite-fsync.md), [`./exploration/linux/09-fallocate.md`](./exploration/linux/09-fallocate.md), [`reference/crates/wo-seg/src/`](../../reference/crates/wo-seg/src/).

## Goal

Every engine mutation appends a typed record to a per-type segment file on disk. **Reads still hit the in-memory `HashMap` — no behaviour change visible to clients yet.** Killing the process after a write leaves a real `data/<TypeName>.seg` on disk; restart re-creates an empty `HashMap` and ignores the segment (recovery is phase 11). This phase only proves the **on-disk row format**.

Lays the codec + filesystem layout that phase 11 (WAL + recovery) and phase 12 (disk-backed engine) build on top of.

## Design decisions (locked)

1. **One segment file per type.** `data/<TypeName>.seg`. No per-record file proliferation, no per-database tablespaces, no relfilenode indirection (per [`./exploration/postgresql/smgr-and-md.md`](./exploration/postgresql/smgr-and-md.md) — Postgres' multi-file model exists for multi-tenant ops; writeonce binds to one data dir per `wo run`).
2. **Append-only with tombstone byte.** Updates and deletes append a new record (with the old one's id + a `TOMBSTONE` flag); compaction is a follow-on phase. Same model as v1 wo-seg.
3. **Length-prefix framing with CRC32C trailer.** `[u32 length LE][u8 flags][u8 record_kind][u64 LSN][payload bytes][u32 CRC32C]`. The CRC trailer is the **one design point where writeonce diverges from v1 wo-seg**: wo-seg skipped checksums; we don't.
4. **Payload codec is `serde_json` for now.** Phase 05 (hand-rolled JSON) swaps it; the codec slot is a single `RowCodec` trait so the swap is mechanical.
5. **`posix_fallocate` to 1 MiB at file creation.** Doubles when full. Avoids `ENOSPC` mid-write and minimizes filesystem-level fragmentation. Per [`./exploration/linux/09-fallocate.md`](./exploration/linux/09-fallocate.md).
6. **`pwrite` for the append, no fsync yet.** This phase does not commit a durability barrier — the bytes land in the OS page cache and that's it. Phase 11 adds the fsync. Lets us validate the format without conflating it with fsync semantics.
7. **Module at `crates/db/`, not extracted from `rt`.** `crates/db/` has been a placeholder since the scaffolding phase — this phase populates it. Other crates (`engine`, `value`, `wal`, `txn`) stay placeholders until their phases activate.

## Scope

### New files inside `crates/db/src/`

| File | Responsibility | Approx LOC |
| --- | --- | --- |
| `lib.rs` | Re-exports `SegStore`, `Frame`, `Flags`, `RecordKind`, `RowCodec`, `LSN`. Replaces today's empty `lib.rs` doc-comment. | ~30 |
| `frame.rs` | `Frame` struct + `encode(payload, flags, kind, lsn) -> Vec<u8>` + `decode(bytes) -> Result<Frame>` with CRC verification. | ~150 |
| `crc.rs` | CRC32C via the SSE 4.2 `crc32c.h` algorithm. Software fallback for older CPUs. ~80 lines hand-rolled vs. pulling a crate. | ~80 |
| `codec.rs` | `trait RowCodec { fn encode(&self, row: &Row, buf: &mut Vec<u8>); fn decode(&self, bytes: &[u8]) -> Result<Row>; }` + `JsonCodec` impl backed by today's `serde_json`. | ~50 |
| `seg.rs` | `SegStore { dir: PathBuf, fds: HashMap<String, RawFd>, tails: HashMap<String, u64> }`. `open(dir)`, `append(ty, &Row) -> Result<u64-offset>`, `read(ty, offset) -> Result<Row>` (used by phase 11 recovery, not by the engine yet). | ~250 |

Total: ~560 LOC. The framing math + fallocate + pwrite plumbing is ported from [`reference/crates/wo-seg/src/{writer.rs,reader.rs,header.rs}`](../../reference/crates/wo-seg/src/) with the CRC trailer added.

### File layout written under `<wo_run_dir>/`

```
docs/examples/blog/
├── app.wo
├── ui/...
└── data/                          ← created by phase 10
    ├── Article.seg
    ├── Author.seg
    ├── Comment.seg
    └── Tag.seg
```

`data/` is gitignored (already covered by `/data` and `/docs/examples/*/data` in `.gitignore`). Empty when no rows exist; created lazily on first write.

### Record framing (illustrated)

```text
                ┌─ length excludes itself; covers flags..CRC.
                ▼
[u32 length LE][u8 flags][u8 kind][u64 LSN][payload bytes ...][u32 CRC32C]
                  │         │
                  │         └─ 0x00 = ROW, 0x01 = TOMBSTONE, others reserved
                  └─ 0x00 = ACTIVE, 0x01 = DELETED  (per-record live bit)
```

`flags` is a per-record live bit — flip it to `DELETED` to soft-delete in place without rewriting the payload. `kind` is the discriminator for upcoming record kinds (phase 11 introduces `WAL_BEGIN`, `WAL_COMMIT`); for phase 10 every record is `ROW`. `LSN` is `0` until phase 11 starts assigning real LSNs — it's a placeholder slot now so phase 11 doesn't reshape the format.

### Engine integration

The `Engine::create / update / delete` methods in `crates/rt/src/engine.rs` get a `seg_store: Arc<Mutex<SegStore>>` field plumbed through `Engine::new`. After every successful in-memory mutation:

```rust
self.seg_store.lock().unwrap()
    .append(ty, &row)
    .map_err(|e| anyhow!("seg append: {e}"))?;
```

Failure aborts the whole mutation — the in-memory write is rolled back. This phase does NOT introduce a "best-effort persistence" mode.

`Engine::list / get` remain unchanged; reads stay in-memory.

### `Cargo.toml` delta

```diff
 [dependencies]
 anyhow     = "1"
 serde      = { version = "1", features = ["derive"] }
 serde_json = "1"
 libc       = "0.2"
+
+[dependencies.db]
+path = "../db"
```

`crates/db/Cargo.toml` itself stays at `libc + serde_json` (the latter via `RowCodec`'s `JsonCodec`). When phase 05 lands, the `serde_json` import collapses into the runtime's hand-rolled `Value`.

The root workspace member list also activates: `crates/db` joins `crates/rt` as a non-empty member.

## Exit criteria

1. **`cargo build`** at root — both `crates/rt` and `crates/db` compile. Five direct deps (`anyhow`, `serde`, `serde_json`, `libc`, `db`).
2. **`cargo test --lib`** — all existing 37 `rt` tests still green; new `db` tests cover:
   - `frame_roundtrip` — encode then decode produces the same `Frame`.
   - `crc_detects_corruption` — flipping one byte in the payload makes `decode` return `CrcMismatch`.
   - `seg_append_writes_to_disk` — `append` then re-`open` reads the same row back.
   - `seg_grows_when_full` — appending past the initial 1 MiB triggers a fallocate-grow without losing existing records.
3. **End-to-end** — `cargo run --bin wo -- run docs/examples/blog`, `curl -X POST /api/articles` with a body, then `xxd docs/examples/blog/data/Article.seg | head -3` — output shows the magic length prefix and the JSON payload.
4. **`reference/rest/blog.rest`** — 20-assertion battery still passes byte-identically.
5. **Restart leaves the segment on disk but ignores it.** `wo run`, write 5 rows, ctrl-C, `wo run` again, `GET /api/articles` returns `[]`. The segment file still exists. Phase 11 will start replaying it.

## Non-scope

- **No fsync.** Pure write path; durability barrier is phase 11.
- **No WAL.** Mutations go straight to the segment. Phase 11 introduces a separate WAL log; segments become the post-checkpoint home for replayed records.
- **No reads from disk.** `Engine::get` stays in-memory. Phase 12 cuts over.
- **No secondary indexes.** Phase 12 introduces a primary `id` BTree on disk; secondary indexes (`unique`, `index` schema attributes) are a later phase.
- **No compaction.** Tombstoned records pile up. Compaction lands when a benchmark says it has to.
- **No cross-type transactions / RETURNING aliases.** The locked schema design (`02-wo-language.md`) names cross-paradigm transactions; the runtime gets there in a later phase.
- **No `crates/db` API stability.** Internal-only until `crates/db/Cargo.toml` declares `[lib]`-level external surfaces.

## Verification

```bash
cargo build                                    # rt + db both compile
cargo test --lib                               # rt + db unit tests
cargo test -p db                               # db-only

# manual end-to-end
cargo run --bin wo -- run docs/examples/blog &
PID=$!
sleep 1
curl -s -X POST http://127.0.0.1:8080/api/articles \
  -H 'Content-Type: application/json' \
  -d '{"slug":"a","title":"A","author":1,"published":true,"meta":{"excerpt":"e","body_md":"b"}}'
ls -la docs/examples/blog/data/
xxd docs/examples/blog/data/Article.seg | head -5
kill -INT $PID

# restart sanity — phase 10 is "format-only", no replay
cargo run --bin wo -- run docs/examples/blog &
PID=$!
sleep 1
curl -s http://127.0.0.1:8080/api/articles    # expect []
kill -INT $PID

cd reference/crates && cargo build && cargo test   # v1 untouched
```

## After this phase

The on-disk format exists but is dead weight — written, never read. Phase 11 brings it to life: introduces a separate WAL log, fsync at commit, group commit per loop tick, and a recovery loop that replays the WAL into the in-memory `HashMap` on startup. Phase 12 then cuts the engine over to read from segments instead of from RAM, completing the transition from in-memory to durable storage.
