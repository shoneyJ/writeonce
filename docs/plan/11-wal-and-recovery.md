# 11 — WAL + crash recovery

**Context sources:** [`./10-storage-foundations.md`](./10-storage-foundations.md), [`../runtime/database/02-wo-language.md#concurrency-model`](../runtime/database/02-wo-language.md#concurrency-model), [`../runtime/database/03-inmemory-engine.md`](../runtime/database/03-inmemory-engine.md), [`./exploration/postgresql/wal.md`](./exploration/postgresql/wal.md), [`./exploration/postgresql/buffer-and-checkpoint.md`](./exploration/postgresql/buffer-and-checkpoint.md), [`./exploration/linux/12-pwrite-fsync.md`](./exploration/linux/12-pwrite-fsync.md), [`../02-recovery.md`](../02-recovery.md).

## Goal

`kill -9` mid-write loses nothing acknowledged. On restart, recovery replays the WAL into the in-memory `HashMap` and the engine serves traffic exactly as if nothing had happened. **The engine is still `HashMap`-backed in this phase** — phase 12 changes that. Phase 11 wires durability without changing the engine's read shape.

This is the phase where the locked architecture statement from [`02-wo-language.md` § Concurrency Model](../runtime/database/02-wo-language.md#concurrency-model) becomes code:

> "On `COMMIT`, the WAL record must be `fsync`'d before the client gets acknowledgment. Group commit drains many pending commits into one fsync SQE per tick."

## Design decisions (locked)

1. **WAL is separate from the segment files.** Segments hold post-recovery row data; the WAL is the durability log we replay from. Per [`./exploration/postgresql/wal.md`](./exploration/postgresql/wal.md). Different fsync cadence (every commit for WAL, every checkpoint for segments).
2. **LSN = monotonic byte offset across all WAL segments.** Postgres convention. 64-bit. Simple `<` comparisons. Matches the placeholder slot phase 10 reserved in the frame header.
3. **Group commit via the loop tick.** No separate writer thread. Every loop tick: drain all pending commits, **one** `fdatasync` covers all of them, then ack each request. Same effect as Postgres' group-commit fence; the fence is the tick boundary.
4. **`fdatasync`, not `fsync`, for the WAL.** WAL files are `posix_fallocate`'d up front to a fixed segment size — writes never extend them, so the inode metadata doesn't change and `fdatasync` is sufficient. Per [`./exploration/linux/12-pwrite-fsync.md`](./exploration/linux/12-pwrite-fsync.md).
5. **Control file via rename-on-write.** `data/control.tmp` → `fsync` → `rename` → parent-dir `fsync`. Atomic across crashes.
6. **Replay is idempotent.** Recovery replays records starting at `last_durable_lsn`; partial replay (crash mid-recovery) re-replays from the same anchor with the same effect.
7. **Module at `crates/wal/`.** `crates/wal/`'s placeholder doc-comment names this work. Populated here.

## Scope

### New files inside `crates/wal/src/`

| File | Responsibility | Approx LOC |
| --- | --- | --- |
| `lib.rs` | Re-exports `Wal`, `Lsn`, `Replay`, `ControlFile`. | ~30 |
| `lsn.rs` | `pub struct Lsn(pub u64)` — newtype with `Display`, ordering, segment-id + offset accessors. | ~50 |
| `wal.rs` | `Wal { dir, active_fd, active_seg_id, tail_lsn, pending: Vec<PendingCommit> }`. `append(rec) -> Lsn`, `commit() -> io::Result<Lsn>` (issues `fdatasync`), `enqueue_ack(fd) / drain_acks() -> Vec<RawFd>`. | ~250 |
| `segment.rs` | WAL-file rollover: open new `<seg-id>.wal`, `posix_fallocate` to 16 MiB, switch active fd, retire the previous segment. | ~120 |
| `control.rs` | `ControlFile { magic, version, last_durable_lsn, crc }` — read on startup, write on checkpoint. Rename-on-write. | ~120 |
| `replay.rs` | `Replay::from(dir, last_lsn) -> Iterator<Item = Result<Record>>` — walks WAL forward, yields decoded records to the caller. | ~150 |

Total: ~720 LOC. No v1 precedent — wo-wal doesn't exist (despite being named in the database series). New territory.

### File layout

```
docs/examples/blog/
└── data/
    ├── control                    ← 32-byte fixed-size; updated atomically
    ├── Article.seg                ← phase 10's segment files
    ├── ...
    └── wal/
        ├── 0000000000000001.wal   ← active WAL segment (16 MiB fallocated)
        └── 0000000000000002.wal   ← created at rollover
```

### Control file format (32 bytes)

```text
[u8 magic[4]   = b"WOCT"]
[u8 version    = 1]
[u8 _pad[3]    = 0]
[u64 last_durable_lsn LE]
[u64 created_unix_seconds LE]
[u32 crc32c]
```

Atomic update sequence (per [`./exploration/postgresql/buffer-and-checkpoint.md`](./exploration/postgresql/buffer-and-checkpoint.md)):

```rust
fs::write("control.tmp", &bytes)?;
let f = File::open("control.tmp")?;
unsafe { libc::fsync(f.as_raw_fd()); }
fs::rename("control.tmp", "control")?;
let dfd = unsafe { libc::open(data_dir.as_ptr(), libc::O_RDONLY) };
unsafe { libc::fsync(dfd); libc::close(dfd); }
```

### Engine integration

`Engine::create / update / delete` — each calls into the WAL after the in-memory mutation succeeds and the segment append (phase 10) succeeds:

```rust
let lsn = self.wal.append(WalRecord::Mutation { ty, op, row })?;
self.wal.enqueue_ack(/* request fd */ fd);
// loop tick later: drain_acks() runs after commit() fsyncs.
```

The HTTP handler in `crates/rt/src/server.rs` becomes:

```rust
fn create_h(engine: &Shared, ty: &str, req: &Request, _params: &RouteParams) -> Response {
    let body = parse_json_body(req);
    let mut eng = engine.lock().unwrap();
    match eng.create(ty, body) {
        Ok(row) => {
            // Engine's create now returns *after* the WAL append, but BEFORE
            // the fsync. The response is held until the next tick's group commit.
            Response::deferred(Status::CREATED, json!(row))
        }
        Err(e) => Response::status(Status::BAD_REQUEST).text(e.to_string()),
    }
}
```

`Response::deferred` is a new variant — the response object is stashed on the connection, but the wire bytes aren't sent until `wal.drain_acks()` returns this fd. Phase 11 introduces this concept; phase 12 keeps it.

### Recovery on startup

```rust
fn recover(data_dir: &Path) -> Result<Engine> {
    let ctl = ControlFile::read_or_initialize(data_dir)?;
    let mut engine = Engine::new(catalog);
    let mut max_seen = ctl.last_durable_lsn;
    for rec in Replay::from(data_dir.join("wal"), ctl.last_durable_lsn)? {
        let rec = rec?;
        match rec.payload {
            WalRecord::Mutation { ty, op, row } => engine.apply_replay(ty, op, row)?,
        }
        max_seen = rec.lsn;
    }
    // Don't advance the control file yet — checkpoint (phase 12+) does that.
    println!("[wo] recovered {} records, tail LSN {}", count, max_seen);
    Ok(engine)
}
```

`Engine::apply_replay` is `Engine::create / update / delete` minus the `wal.append` callback (already-replayed records re-applied don't get re-WAL'd).

### Group commit — the loop integration

`crates/rt/src/bin/wo.rs`'s `serve_loop` gains:

```rust
'outer: loop {
    let events = eloop.wait_once(Some(Duration::from_secs(60)))?;
    for ev in events { /* dispatch as before */ }

    // Group commit fence — runs once per tick after request dispatch.
    if engine.lock().unwrap().wal.pending_commits() > 0 {
        let _ = engine.lock().unwrap().wal.commit();   // one fdatasync
        for fd in engine.lock().unwrap().wal.drain_acks() {
            // Mark the connection writable; its queued response now flushes.
            eloop.modify(fd, Interest::READ_WRITE, Token(fd as u64))?;
        }
    }
}
```

### `Cargo.toml` delta

`crates/rt/Cargo.toml` adds `wal` as a path dep alongside `db`. Workspace adds `crates/wal` to the members list.

## Exit criteria

1. **`cargo build`** at root — `rt`, `db`, `wal` all compile.
2. **Unit tests in `crates/wal/src/`:**
   - `wal_append_assigns_monotonic_lsn` — successive appends produce strictly increasing LSNs.
   - `wal_rollover_at_segment_cap` — appending past `WAL_SEG_SIZE` opens segment 2 without losing tail.
   - `replay_yields_records_in_order` — write 100, replay returns 100 in LSN order.
   - `control_file_rename_on_write` — kill -9 between tmp-write and rename leaves old control intact.
   - `crc_mismatch_aborts_replay` — corrupting one byte in WAL aborts replay with `CrcMismatch`.
3. **Integration test `crates/rt/tests/wal_recovery.rs`:**
   - Starts `wo run docs/examples/blog` with `WO_LISTEN=127.0.0.1:0`.
   - POSTs 100 articles via the http stack.
   - Sends `kill -9` to the binary.
   - Restarts; GETs all 100 back.
4. **The api.rest 20-assertion battery still passes byte-identically** under `WO_FSYNC=on` (default).
5. **`WO_FSYNC=off` env var** — when set, skips the `fdatasync` for tests that don't care about durability. Drops cold-restart-recovery latency to zero.
6. **`strace -e fdatasync,fsync,rename`** during a 5-commit run shows ~5 `fdatasync` calls (one per tick), no `fsync` (no rollover, no checkpoint yet), no `rename` (no control update yet — that's phase 12+).

## Non-scope

- **No checkpoint loop yet.** Phase 11 reads the control file at startup and writes it at clean shutdown only; periodic checkpoint lands with phase 12 or shortly after. Until then, recovery walks the entire WAL on every restart — fine for a sample workload, expensive for production.
- **No segment compaction.** Old WAL segments stay on disk forever in this phase. A future phase truncates after a checkpoint advances the control file past them.
- **No `io_uring`.** Synchronous `pwrite` + `fdatasync`. `io_uring` becomes interesting when the loop drains many fds per tick; phase 11's commit cadence doesn't need it. Layered on later.
- **No partial-record handling on torn writes.** A WAL segment is `posix_fallocate`'d up front, so partial-write torn-record on the leading edge of the file is the only scenario; the CRC trailer detects it and replay stops cleanly.
- **No multi-process recovery.** Single-binary invariant.
- **No engine cutover to disk reads.** `Engine::list / get` still walk the in-memory `HashMap` — phase 12.

## Verification

```bash
cargo build                                          # rt + db + wal
cargo test --lib                                     # all unit tests green
cargo test --test wal_recovery                       # the kill-9 integration test

# durability smoke (manual)
WO_LISTEN=127.0.0.1:8765 cargo run --bin wo -- run docs/examples/blog &
PID=$!
sleep 1
for i in 1 2 3 4 5; do
  curl -sf -X POST http://127.0.0.1:8765/api/articles \
    -H 'Content-Type: application/json' \
    -d "{\"slug\":\"a$i\",\"title\":\"A$i\",\"author\":1,\"published\":true,\"meta\":{\"excerpt\":\"\",\"body_md\":\"\"}}" \
    > /dev/null
done
kill -9 $PID
WO_LISTEN=127.0.0.1:8765 cargo run --bin wo -- run docs/examples/blog &
PID=$!
sleep 1
curl -s http://127.0.0.1:8765/api/articles | python3 -c "import json,sys;print(len(json.load(sys.stdin)))"
# expect: 5
kill -INT $PID

# strace check
strace -e fdatasync,fsync,rename -f -p $(pgrep -f 'target/debug/wo run') 2>&1 | head -20

# v1 untouched
cd reference/crates && cargo build && cargo test
```

## After this phase

Durability is real but the engine is still `HashMap<type, BTreeMap<i64, Row>>` — every row, in full, lives in RAM. Phase 12 swaps the in-memory `Row` for an offset into the segment file and adds checkpoints, completing the transition to a durable, RAM-bounded engine. After phase 12 the runtime can serve a 10× larger dataset than fits in RAM without a redesign.
