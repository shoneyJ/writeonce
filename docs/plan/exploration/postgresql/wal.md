# WAL — append-only durability log

`access/transam/xlog*.c` is Postgres' write-ahead log: every mutation writes a record to a sequential log on disk *before* the in-memory page changes are considered durable. On `COMMIT`, the WAL is `fsync`'d up to the commit's LSN and only then is the client acknowledged. The data files themselves can be flushed lazily — recovery rebuilds them from the WAL.

Writeonce mirrors the algorithm. The single-thread loop replaces multi-process coordination with per-tick group-commit drainage; everything else carries over.

## Postgres source

| File | Responsibility |
| --- | --- |
| [`access/transam/xlog.c`](../../../../reference/postgresql/src/backend/access/transam/xlog.c) | Top-level WAL machinery: insertion locks, segment rollover, flush coordination, control-file rendezvous. |
| [`access/transam/xloginsert.c`](../../../../reference/postgresql/src/backend/access/transam/xloginsert.c) | Build a WAL record (header + payload + backup-block deltas) and place it into the in-memory WAL buffer. |
| [`access/transam/xlogreader.c`](../../../../reference/postgresql/src/backend/access/transam/xlogreader.c) | Decode WAL records during recovery — pure parser, no I/O. Useful as the read-side spec. |
| [`access/transam/xlogrecovery.c`](../../../../reference/postgresql/src/backend/access/transam/xlogrecovery.c) | The replay loop. Walks the WAL from the last-checkpoint LSN, replays each record into shared buffers, advances the redo pointer. |
| [`postmaster/walwriter.c`](../../../../reference/postgresql/src/backend/postmaster/walwriter.c) | Background process that flushes the WAL buffer to disk asynchronously. Writeonce does this **inline in the loop tick**. |

## The five Postgres WAL ideas writeonce keeps

1. **LSN = monotonic byte offset across all segments.** A 64-bit cursor that combines `(segment_id, offset_within_segment)` into one value. Cheap arithmetic — comparing two LSNs is a single `<` on `u64`. Gives every record a permanent address.

2. **Segment rollover.** WAL is broken into fixed-size files (`pg_wal/<24-hex-name>`, default 16 MiB) so old segments can be archived/recycled without touching the active write head. Writeonce inherits the size choice — 16 MiB is a Postgres-tested sweet spot between rollover frequency and archived-file granularity.

3. **Group commit.** Many backends call `XLogFlush(commit_lsn)` concurrently; only the first one issues the `fsync`, the rest wait on the result. The fsync covers everyone's records up to the highest LSN flushed. **In writeonce, this is automatic** — the loop drains all pending commits between two `wait_once`s, then issues one `fsync` covering all of them. No coordination primitive needed.

4. **`pwrite` + `fsync` (or `fdatasync`) at commit.** Postgres uses `pg_pwrite()` (a wrapper over `pwrite64`) for buffer-aligned positional writes and `pg_fsync()` for the durability barrier. Writeonce uses the same syscall pair. See [`linux/12-pwrite-fsync.md`](../linux/12-pwrite-fsync.md).

5. **Control file as the recovery anchor.** A small fixed-size file (`global/pg_control`) stores the **last redo-safe LSN** — the point where recovery starts. Updated atomically (Postgres uses a careful write-fsync-rename sequence). On startup, recovery reads it, then scans WAL forward from that LSN.

## What writeonce drops

- **WAL buffer + walwriter process.** Postgres has an in-memory ring of WAL pages (`XLogCtl`) and a separate process that flushes it asynchronously between commits. Writeonce keeps a per-tick pending-commit list and flushes inline; no buffer ring, no extra process.
- **Replication slots / archive command.** WAL streaming and `archive_command` belong to the multi-machine story. Writeonce is single-binary; replication is a phase past 12.
- **Backup blocks (`XLOG_FPI`, full-page images).** Postgres logs whole pages on first modification after a checkpoint to defend against torn writes (page = 8 KiB, kernel write atomicity = 4 KiB on most fs). Writeonce's record-level CRC32C and per-record framing replace this — no torn-write hazard at the page granularity because writeonce doesn't have pages until a later phase.
- **WAL levels (`wal_level = minimal | replica | logical`).** Postgres tunes how much detail to log based on whether replicas exist. Writeonce always logs the same shape.

## Record framing — Postgres vs. writeonce

Postgres records: `XLogRecord` header (24 bytes: total length, xid, prev LSN, info, rmgr id, CRC32C) + per-rmgr block headers + payload. Variable length. Compact.

Writeonce records (per `docs/plan/10-storage-foundations.md`): `[u32 length LE][u8 flags][u8 record_kind][u64 LSN][payload bytes][u32 CRC32C]`. Simpler — no resource manager indirection, no backup blocks, no XID. The single-thread loop owns all the schema metadata so the rmgr layer collapses into a one-byte `record_kind`.

## Group commit — the writeonce shape

```text
loop tick:
    events = epoll_wait_once();
    for each readable conn fd:
        read request, run handler, mutate engine
        if handler committed: append WAL record, push fd to commits[]
    if !commits.empty():
        fsync(wal_fd)
        for fd in commits: send 200 OK, mark conn writable
    drain writable fds
```

Same effect as Postgres' group-commit fence (one `fsync` flushes many commits) without the IPC. The fence is the loop tick boundary itself.

## Pointers when implementing phase 11

- [`xloginsert.c:XLogInsert()`](../../../../reference/postgresql/src/backend/access/transam/xloginsert.c) — entry point for "insert this record into the WAL." Read the prologue + the LSN-assignment loop, ignore the buffer-juggling.
- [`xlog.c:XLogFlush()`](../../../../reference/postgresql/src/backend/access/transam/xlog.c) — "make this LSN durable on disk." Read the early-out for "already flushed" and the group-commit waiter logic.
- [`xlogrecovery.c:PerformWalRecovery()`](../../../../reference/postgresql/src/backend/access/transam/xlogrecovery.c) — the replay loop. Read the redo-pointer advance logic; ignore the multi-process startup signaling.

## Used by

[`docs/plan/11-wal-and-recovery.md`](../../11-wal-and-recovery.md) — WAL framing, group commit, control file, replay loop. Pair with [`linux/12-pwrite-fsync.md`](../linux/12-pwrite-fsync.md) for syscall details.
