# 12 — Engine cutover: rows live on disk

**Context sources:** [`./10-storage-foundations.md`](./10-storage-foundations.md), [`./11-wal-and-recovery.md`](./11-wal-and-recovery.md), [`../runtime/database/03-inmemory-engine.md`](../runtime/database/03-inmemory-engine.md), [`../runtime/database/07-wo-seg-migration.md`](../runtime/database/07-wo-seg-migration.md), [`./exploration/postgresql/buffer-and-checkpoint.md`](./exploration/postgresql/buffer-and-checkpoint.md), [`./exploration/postgresql/page-format.md`](./exploration/postgresql/page-format.md), [`./exploration/linux/12-pwrite-fsync.md`](./exploration/linux/12-pwrite-fsync.md).

## Goal

`Engine`'s row payload is no longer in RAM. The in-memory map is `HashMap<type, BTreeMap<i64, SegmentOffset>>`. Reads `pread` against the segment file and verify the CRC. RAM footprint is bounded by **id count + per-id overhead**, independent of row payload size — the runtime can now serve a dataset 10× larger than RAM.

A periodic checkpoint flushes dirty segments and advances the control-file LSN, bounding recovery time on restart.

## Design decisions (locked)

1. **In-memory index is `BTreeMap<i64, SegmentOffset>`.** Keeps the existing `Engine::list` insertion-order iteration. Roughly 24 bytes per entry (i64 key + u64 value + tree node overhead) — a million rows fits in 24 MiB regardless of row size.
2. **Reads via `pread` + decode + CRC verify.** No user-space buffer pool — the OS page cache is the cache (per [`./exploration/postgresql/buffer-and-checkpoint.md`](./exploration/postgresql/buffer-and-checkpoint.md)). Hot rows hit cached pages and the syscall returns memcpy-fast.
3. **Bounded LRU on top of pread.** Optional small `HashMap<(ty, offset), Row>` capped at `WO_CACHE_ROWS=10000` (configurable). Avoids re-decoding on hot reads. Eviction on insert when full. **Phase 12 ships without it** if the bench numbers are fine; included here as a follow-on hatch.
4. **Tombstoned offsets stay in the BTreeMap until compaction.** A delete writes a tombstone to the segment + marks the BTreeMap entry as `SegmentOffset::Tombstone`. List skips them. Counts as wasted space until a future compaction phase rewrites the segment.
5. **Checkpoint = `fsync` every active segment fd + advance control file.** Runs every `CHECKPOINT_INTERVAL_SECS=60` (configurable) and at clean shutdown.
6. **MVCC stays out of scope.** Subscriber pre-commit views are tick-boundary semantics, not version chains (per [`../runtime/database/03-inmemory-engine.md`](../runtime/database/03-inmemory-engine.md)). When a future phase adds LIVE subscriber predicate matching, version chains may join the engine — until then, the single-thread invariant gives us the same visibility guarantees for free.
7. **Secondary indexes deferred.** Phase 12 ships only the primary `id` BTree. `unique` + `index` schema attributes get their own follow-on phase.

## Scope

### Files rewritten inside `crates/rt/src/`

| File | Change |
| --- | --- |
| `engine.rs` | `BTreeMap<i64, Row>` → `BTreeMap<i64, SegmentOffset>`. `Engine::get` becomes `seg_store.read(ty, offset)?`. `Engine::list` walks the BTreeMap and `pread`s each record (sequential — page cache makes it fast for the sample workload). `Engine::create / update / delete` keep the phase-10 segment append + phase-11 WAL append, but no longer keep the `Row` in memory. |
| `bin/wo.rs` | After WAL recovery, populate the BTreeMap with `(id → offset)` pairs by walking the recovered records. Also: spawn a `TimerFd::periodic(CHECKPOINT_INTERVAL_SECS)` registered on the event loop; the checkpoint step runs when the timer fires. |

### New file inside `crates/db/src/`

| File | Responsibility | Approx LOC |
| --- | --- | --- |
| `checkpoint.rs` | `Checkpoint::run(seg_store, wal, control)` — fsync every segment fd, write `last_durable_lsn = wal.tail_lsn` to the control file (rename-on-write), prune retired WAL segments older than the new LSN. | ~150 |

### What `SegmentOffset` looks like

```rust
#[derive(Debug, Clone, Copy)]
enum SegmentOffset {
    Live(u64),       // byte offset in the segment file
    Tombstone(u64),  // ditto, but the row is logically deleted
}
```

A `BTreeMap<i64, SegmentOffset>` consumes ~24 B per entry (key + 16-byte enum). 10M rows → ~240 MiB index. Order-of-magnitude bigger than `O(rowcount × pointer)` because the enum carries a discriminant; collapse to `u64` with a high-bit tombstone flag if memory pressure justifies it later.

### Recovery (phase 11) becomes

```rust
fn recover(data_dir: &Path) -> Result<Engine> {
    let ctl = ControlFile::read_or_initialize(data_dir)?;
    let seg_store = SegStore::open(data_dir)?;
    let wal = Wal::open(data_dir.join("wal"), ctl.last_durable_lsn)?;
    let mut engine = Engine::new(catalog);
    engine.attach(seg_store, wal);

    // Walk the segments first to populate the offset index from durable rows.
    for ty in engine.catalog().order.iter() {
        for (id, offset, flags) in seg_store.iter(ty)? {
            engine.index_mut(ty).insert(id, match flags {
                Flags::ACTIVE    => SegmentOffset::Live(offset),
                Flags::TOMBSTONE => SegmentOffset::Tombstone(offset),
            });
        }
    }
    // Then replay any WAL records past the last checkpoint to catch up.
    for rec in Replay::from(data_dir.join("wal"), ctl.last_durable_lsn)? {
        engine.apply_replay(rec?)?;
    }
    Ok(engine)
}
```

The WAL replay still runs but covers a much smaller range — only what's been written since the last checkpoint. Recovery time is bounded by WAL volume between checkpoints, not by the entire history.

### Checkpoint as a loop step

```rust
let cp_timer = TimerFd::periodic(Duration::from_secs(60))?;
eloop.register(cp_timer.as_raw_fd(), Interest::READABLE, Token(cp_timer.as_raw_fd() as u64))?;

// In serve_loop:
fd if fd == cp_timer.as_raw_fd() => {
    let _ = cp_timer.read();   // drain timerfd's expirations
    let mut eng = engine.lock().unwrap();
    Checkpoint::run(&eng.seg_store, &eng.wal, &mut eng.control)?;
    println!("[wo] checkpoint at LSN {}", eng.control.last_durable_lsn);
}
```

The phase-02 `TimerFd::periodic` already exists; this is the first runtime caller for it.

### Bench

A small criterion-style microbench in `crates/rt/benches/engine_disk.rs`:

| Test | Target |
| --- | --- |
| Insert 100k rows (50-byte payload) | < 5 s wall, < 50 MiB RSS at end |
| Random read 100k rows under steady-state load | < 5 µs p50, < 100 µs p99 (page cache hot) |
| Cold-cache read 100k rows | < 200 µs p50 (one disk seek per read) |
| Recovery time after kill -9 mid-bench | < WAL_volume / disk_throughput, dominated by `fdatasync` round-trips |

`criterion` is normally an external crate; we're not adding deps. The bench is a `#[test]` with a `--release` runner — coarse but enough to catch regressions.

### `Cargo.toml` delta

None — `db` and `wal` are already in from phases 10 and 11.

## Exit criteria

1. **`cargo build`** at root, four direct deps unchanged (`anyhow`, `serde`, `serde_json`, `libc`).
2. **All existing unit tests still pass** after the engine rewrite. The two heaviest are `engine::tests::crud_roundtrip_auto_id` (port to verify offset semantics) and `server::tests::*` (HTTP-level CRUD — should be unaffected).
3. **End-to-end api.rest battery passes byte-identically** — same status codes, same JSON bodies, same key ordering.
4. **Integration test `crates/rt/tests/disk_engine.rs`:**
   - Seed 10k rows of a 1 KiB payload type. Memory after seed (`/proc/self/status` `VmRSS`) is bounded by `id_count × 24 B + listener_overhead`, NOT by `10000 × 1024`. Specifically: less than 40 MiB.
   - Restart with kill -9 mid-write; recovery completes in < 1 s for a 16-MiB-WAL-segment workload.
   - GET random ids — every read returns the right row, CRC verified.
5. **Checkpoint smoke** — start the binary, write 5 rows, wait `CHECKPOINT_INTERVAL_SECS+1` seconds, verify `data/control` is updated (mtime moved, `last_durable_lsn` advanced). `strace -e fsync,rename` during the wait shows the checkpoint sequence.
6. **Cold start with no `data/`** — a fresh `wo run` on an empty data dir just works (creates the dir, no replay needed). Same for `data/` + empty WAL.

## Non-scope

- **No secondary indexes.** `unique` and `index` schema attributes still trigger no extra storage. Future phase.
- **No compaction.** Tombstoned offsets and old segment bytes accumulate. Trigger compaction is a separate phase keyed on a `dead-bytes / live-bytes` ratio.
- **No MVCC.** Subscriber pre-commit views are tick-boundary semantics (`docs/runtime/database/03-inmemory-engine.md`). Version chains land alongside the cross-shard subscription work in `09c-per-shard-wal` / `09d-cross-shard-subscriptions`.
- **No `O_DIRECT`.** Page cache is the cache. Per [`./exploration/linux/12-pwrite-fsync.md`](./exploration/linux/12-pwrite-fsync.md).
- **No `io_uring` reads.** `pread` syscalls are short and the loop has no other work waiting; an async batched read API isn't worth its own complexity at this size.
- **No streaming list.** `Engine::list` returns all rows for a type in one call. Pagination + cursor support is a future phase keyed on a real workload that hits the wall.

## Verification

```bash
cargo build
cargo test --lib                                     # rt + db + wal unit tests
cargo test --test disk_engine                        # the new integration test
cargo test --release --test disk_engine -- --nocapture   # bench numbers visible

# manual end-to-end
cargo run --release --bin wo -- run docs/examples/blog &
PID=$!
sleep 1
# Seed 10000 rows
for i in $(seq 1 10000); do
  curl -sf -X POST http://127.0.0.1:8080/api/articles \
    -H 'Content-Type: application/json' \
    -d "{\"slug\":\"s$i\",\"title\":\"T$i\",\"author\":1,\"published\":true,\"meta\":{\"excerpt\":\"\",\"body_md\":\"\"}}" \
    > /dev/null
done
# Memory check
ps -o rss= -p $PID    # expect under ~50 MiB even with 10k rows × 1 KiB each
# Wait for checkpoint
sleep 65
ls -la docs/examples/blog/data/control     # mtime should be recent
kill -INT $PID

# Cold restart
cargo run --release --bin wo -- run docs/examples/blog &
PID=$!
sleep 1
curl -s 'http://127.0.0.1:8080/api/articles' | python3 -c 'import json,sys;print(len(json.load(sys.stdin)))'
# expect: 10000
kill -INT $PID

cd reference/crates && cargo build && cargo test     # v1 untouched
```

## After this phase

The single-thread runtime is durable, RAM-bounded, and recovery-fast. Phases 13+ pivot to layering features on top: secondary indexes, compaction, query-layer integration, then the `09a-09f` scaleout sequence which lifts the same primitives into per-shard form. The empty `crates/{value, engine, txn}` skeletons get populated as their phases activate; `wal/` and `db/` are now real code, used by `rt/`.

The `crates/rt/Cargo.toml` direct dep list at the end of phase 12 is `anyhow + serde + serde_json + libc + db + wal`. Phase 05 collapses `serde + serde_json` into the hand-rolled JSON module; phase 06 collapses `anyhow` into a bespoke error type. The `libc` + path-deps end state from [`./done/01-scafolding-crates.md`](./done/01-scafolding-crates.md) is reachable in two more phases past 12.
