//! Per-shard write-ahead log — plan 09c (`docs/plan/09-concurrency-scaleout.md`)
//! + the durability core of plan 11, ported from the proven C sequence
//! (`prototypes/wo-rt-c` phases D/E, `docs/plan/exploration/c-runtime/00-plan.md`).
//!
//! One `shard-<t>.rwal` per worker. Frame format (identical shape to the C
//! prototype): `u32 len | u32 crc32(payload) | payload | u32 COMMIT` — a
//! record replays whole or not at all; a torn tail fails CRC/trailer
//! validation and is truncated. Payloads are JSON-serialized [`WalRec`]s.
//!
//! Dual-write order (the C crash-under-load test's hard-won lesson): the
//! engine applies to RAM, appends the frame, `fdatasync`s, and only then
//! returns — so the HTTP ack (written after the handler returns, including
//! for cross-shard jobs whose reply follows the owner's engine call) is
//! always behind the fsync. Group commit (one fsync per loop tick, acks
//! parked on the completion) is deliberately deferred to the io_uring port —
//! doing it on the epoll loop would reopen the exact ack-before-fsync race
//! the C phase-F bench caught. One fsync per commit is slower and correct.
//!
//! Boot: `Wal::open_and_replay` walks the log into the shard's engine —
//! parallel across workers, before any accept is armed. No snapshots yet
//! (the WAL grows unbounded; compaction is phase 11 proper).

use std::fs::{File, OpenOptions};
use std::io::{self, Read, Seek, SeekFrom, Write};
use std::os::unix::io::AsRawFd;
use std::path::Path;

use serde::{Deserialize, Serialize};
use serde_json::Value;

use crate::engine::{Engine, Row};

const WAL_COMMIT: u32 = 0xC0FF_EE42;
const WAL_PREALLOC: i64 = 4 * 1024 * 1024;
const MAX_FRAME: u32 = 16 * 1024 * 1024;

/// One logged mutation. `Create` carries the FULL post-default row (id,
/// timestamps included) so replay is byte-exact; `Update` carries the merge
/// body (merge is deterministic in log order). `Txn` bundles every mutation
/// of one method call (plan 13b) into a single frame: the frame's CRC +
/// trailer make it replay whole-or-not-at-all, so a crash mid-method can
/// never leave a partial method on disk.
#[derive(Debug, Serialize, Deserialize)]
#[serde(tag = "op", rename_all = "snake_case")]
pub enum WalRec {
    Create { ty: String, row: Row },
    Update { ty: String, id: i64, body: Value },
    Delete { ty: String, id: i64 },
    Txn    { recs: Vec<WalRec> },
}

#[derive(Debug)]
pub struct Wal {
    file: File,
}

/// An acknowledgment parked until its batch's fsync CQE (group commit).
/// `Conn` carries the C-proven generation stamp — kernel fds get reused, and
/// releasing by bare fd would ack a NEW connection's commit before ITS batch
/// is durable (the phase-F ABA bug, prevented by construction here).
pub enum Parked {
    /// A local connection whose response waits in its write buffer.
    Conn { fd: std::os::unix::io::RawFd, gen: u64 },
    /// A cross-shard reply — the owner runs this to release the requester.
    Reply(Box<dyn FnOnce() + Send>),
}

/// Group-commit WAL (io_uring): mutations STAGE frames + park their acks;
/// once per loop tick the worker flushes the staging buffer as one `WRITE`
/// SQE hard-linked to one `FSYNC` SQE; the fsync completion releases every
/// parked ack in the batch. Double-buffered — while a batch is in flight
/// (its buffer pinned for the kernel), new commits stage into the twin.
impl std::fmt::Debug for WalGroup {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result { f.write_str("WalGroup") }
}

pub struct WalGroup {
    file:     File,
    ring:     crate::runtime::Uring,
    offset:   u64,
    staging:  Vec<u8>,
    parked:   Vec<Parked>,
    inflight: Option<(Vec<u8>, Vec<Parked>)>,
    /// Batch sequence — encoded into user_data so a write CQE and an fsync
    /// CQE can never be attributed to the wrong batch.
    seq:       u64,
    got_write: Option<i32>,
    got_fsync: Option<i32>,
}

impl WalGroup {
    /// Wrap a replayed [`Wal`] (same file, offset at the validated tail).
    pub fn new(wal: Wal, ring: crate::runtime::Uring) -> io::Result<Self> {
        let mut file = wal.file;
        let offset = file.seek(SeekFrom::Current(0))?;
        let _ = file.seek(SeekFrom::Start(offset));
        Ok(Self { file, ring, offset, staging: Vec::new(), parked: Vec::new(), inflight: None,
                  seq: 0, got_write: None, got_fsync: None })
    }

    pub fn ring_fd(&self) -> std::os::unix::io::RawFd { self.ring.as_raw_fd() }

    /// Stage one record into the active batch. RAM is already applied; the
    /// ack must now be parked (see [`Parked`]) until this batch fsyncs.
    pub fn stage(&mut self, rec: &WalRec) -> io::Result<()> {
        let payload = serde_json::to_vec(rec).map_err(io::Error::other)?;
        self.staging.extend_from_slice(&(payload.len() as u32).to_le_bytes());
        self.staging.extend_from_slice(&crc32(&payload).to_le_bytes());
        self.staging.extend_from_slice(&payload);
        self.staging.extend_from_slice(&WAL_COMMIT.to_le_bytes());
        Ok(())
    }

    pub fn park(&mut self, p: Parked) {
        self.parked.push(p);
    }

    /// End-of-tick: if commits are staged and no batch is in flight, submit
    /// the whole batch as WRITE→FSYNC linked SQEs — one syscall.
    pub fn flush(&mut self) -> io::Result<()> {
        if self.inflight.is_some() || self.staging.is_empty() { return Ok(()); }
        let buf   = std::mem::take(&mut self.staging);
        let acks  = std::mem::take(&mut self.parked);
        let fd    = self.file.as_raw_fd();
        // user_data = (batch_seq << 1) | op-bit — CQEs are matched to THIS
        // batch only; a stale completion can never release the wrong acks.
        let ud_write = self.seq << 1;
        let ud_fsync = (self.seq << 1) | 1;
        // SAFETY: `buf` moves into `inflight` and stays pinned until the CQE.
        self.ring.push_write(fd, &buf, self.offset, true, ud_write);
        self.ring.push_fsync(fd, ud_fsync);
        self.ring.submit()?;
        self.inflight  = Some((buf, acks));
        self.got_write = None;
        self.got_fsync = None;
        Ok(())
    }

    /// Ring-fd readable: reap completions. Returns `Some((ok, acks))` only
    /// when BOTH the write and fsync CQEs of the in-flight batch have
    /// arrived — they routinely land in different ticks on real disks, and
    /// releasing on the write CQE alone would ack before durability.
    /// `ok = false` means short write or failed fsync: the caller must DROP
    /// the acks (close the connections), never release them.
    pub fn complete(&mut self) -> Option<(bool, Vec<Parked>)> {
        for (ud, res) in self.ring.pop_cqes() {
            if ud >> 1 != self.seq { continue; }    // not this batch (stale/corrupt)
            if ud & 1 == 0 { self.got_write = Some(res); }
            else           { self.got_fsync = Some(res); }
        }
        if self.inflight.is_none() { return None; }
        let (Some(wr), Some(fr)) = (self.got_write, self.got_fsync) else { return None; };
        let (buf, acks) = self.inflight.take()?;
        self.got_write = None;
        self.got_fsync = None;
        self.seq += 1;
        let mut ok = true;
        if wr != buf.len() as i32 {
            eprintln!("[wo] wal: short write {wr} != {}", buf.len());
            ok = false;
        }
        if fr < 0 {
            eprintln!("[wo] wal: fsync failed ({fr})");
            ok = false;
        }
        if ok { self.offset += buf.len() as u64; }
        Some((ok, acks))
    }
}

impl Wal {
    /// Open (creating if absent) the shard's log, replay every valid frame
    /// into `engine`, truncate any torn tail, and return a writer positioned
    /// at the validated end. Returns `(wal, replayed_records)`.
    pub fn open_and_replay(path: &Path, engine: &mut Engine) -> io::Result<(Self, usize)> {
        let mut file = OpenOptions::new().read(true).write(true).create(true).open(path)?;
        unsafe { libc::fallocate(file.as_raw_fd(), 0, 0, WAL_PREALLOC) };

        let mut buf = Vec::new();
        file.read_to_end(&mut buf)?;

        let mut off  = 0usize;
        let mut recs = 0usize;
        loop {
            let Some(frame) = read_frame(&buf, off) else { break };
            let (payload, next) = frame;
            match serde_json::from_slice::<WalRec>(payload) {
                Ok(rec) => engine.replay(&rec),
                Err(e)  => {
                    eprintln!("[wo] wal {}: undecodable record at byte {off} ({e}) — truncating", path.display());
                    break;
                }
            }
            recs += 1;
            off = next;
        }

        // Resume appends at the validated tail; drop torn bytes.
        file.set_len(off as u64)?;
        unsafe { libc::fallocate(file.as_raw_fd(), 0, 0, WAL_PREALLOC.max(off as i64)) };
        file.seek(SeekFrom::Start(off as u64))?;
        Ok((Self { file }, recs))
    }

    /// Append one record and make it durable. The caller's mutation is only
    /// allowed to stand (and its response to leave) after this returns Ok.
    pub fn append(&mut self, rec: &WalRec) -> io::Result<()> {
        let payload = serde_json::to_vec(rec).map_err(io::Error::other)?;
        let mut frame = Vec::with_capacity(payload.len() + 12);
        frame.extend_from_slice(&(payload.len() as u32).to_le_bytes());
        frame.extend_from_slice(&crc32(&payload).to_le_bytes());
        frame.extend_from_slice(&payload);
        frame.extend_from_slice(&WAL_COMMIT.to_le_bytes());
        self.file.write_all(&frame)?;
        self.file.sync_data()?;          // the D in ACID — ack ordering lives here
        Ok(())
    }
}

/// Validate and slice one frame at `off`. `None` = clean end or torn tail.
fn read_frame(buf: &[u8], off: usize) -> Option<(&[u8], usize)> {
    let u32_at = |o: usize| -> Option<u32> {
        buf.get(o..o + 4).map(|b| u32::from_le_bytes(b.try_into().unwrap()))
    };
    let len = u32_at(off)?;
    if len == 0 || len > MAX_FRAME { return None; }     // preallocated zeros / garbage
    let len = len as usize;
    let crc     = u32_at(off + 4)?;
    let payload = buf.get(off + 8..off + 8 + len)?;
    let trailer = u32_at(off + 8 + len)?;
    if crc32(payload) != crc || trailer != WAL_COMMIT { return None; }
    Some((payload, off + 8 + len + 4))
}

/// Hand-rolled CRC32 (poly 0xEDB88320) — same algorithm as the C prototype;
/// no external crate.
fn crc32(data: &[u8]) -> u32 {
    let mut c: u32;
    let mut table = [0u32; 256];
    for (i, t) in table.iter_mut().enumerate() {
        c = i as u32;
        for _ in 0..8 {
            c = if c & 1 != 0 { 0xEDB8_8320 ^ (c >> 1) } else { c >> 1 };
        }
        *t = c;
    }
    let mut crc = 0xFFFF_FFFFu32;
    for &b in data {
        crc = table[((crc ^ b as u32) & 0xFF) as usize] ^ (crc >> 8);
    }
    crc ^ 0xFFFF_FFFF
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::compile::Catalog;
    use crate::parser::parse;
    use serde_json::json;

    fn catalog() -> Catalog {
        Catalog::from_schemas(vec![parse(
            r#"@table(index: [title])
               type Note { id: Id
                           title: Text
                           service rest "/api/notes" expose list, get, create, update, delete }"#,
        ).unwrap()]).unwrap()
    }

    fn tmp(name: &str) -> std::path::PathBuf {
        let p = std::env::temp_dir().join(format!("wo-wal-test-{}-{name}", std::process::id()));
        let _ = std::fs::remove_file(&p);
        p
    }

    #[test]
    fn replay_restores_creates_updates_deletes_and_id_highwater() {
        let path = tmp("roundtrip");
        {
            let mut e = Engine::for_shard(catalog(), 0, 2);
            let (wal, n) = Wal::open_and_replay(&path, &mut e).unwrap();
            assert_eq!(n, 0);
            e.attach_wal(wal);
            e.create("Note", json!({"title":"a"})).unwrap();         // id 1
            e.create("Note", json!({"title":"b"})).unwrap();         // id 3
            e.update("Note", 1, json!({"title":"a2"})).unwrap();
            e.create("Note", json!({"title":"c"})).unwrap();         // id 5
            e.delete("Note", 3).unwrap();
        }
        // Fresh engine, replay from disk — the "first load".
        let mut e = Engine::for_shard(catalog(), 0, 2);
        let (wal, n) = Wal::open_and_replay(&path, &mut e).unwrap();
        assert_eq!(n, 5);
        let rows = e.list("Note").unwrap();
        let ids: Vec<i64> = rows.iter().map(|r| r["id"].as_i64().unwrap()).collect();
        assert_eq!(ids, vec![1, 5]);
        assert_eq!(rows[0]["title"], "a2");
        // Replay went through row_insert/row_remove — the secondary index is
        // rebuilt: the update moved id 1 from "a" to "a2", the delete cleared
        // id 3's entry.
        assert_eq!(e.find_by("Note", &[("title".into(), json!("a2"))]).unwrap().len(), 1);
        assert!(e.find_by("Note", &[("title".into(), json!("a"))]).unwrap().is_empty());
        assert!(e.find_by("Note", &[("title".into(), json!("b"))]).unwrap().is_empty());
        // id high-water restored: the next mint must not collide (and must
        // keep the shard-0-of-2 stride: odd ids).
        e.attach_wal(wal);
        let next = e.create("Note", json!({"title":"d"})).unwrap();
        assert_eq!(next["id"], 7);
        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn torn_tail_is_dropped_whole() {
        let path = tmp("torn");
        {
            let mut e = Engine::for_shard(catalog(), 0, 1);
            let (wal, _) = Wal::open_and_replay(&path, &mut e).unwrap();
            e.attach_wal(wal);
            e.create("Note", json!({"title":"keep"})).unwrap();
            e.create("Note", json!({"title":"casualty"})).unwrap();
        }
        // Tear the last record mid-payload. The file is fallocate'd, so the
        // data tail is the last non-zero byte (the 0xC0FFEE42 trailer), not
        // the file length.
        let bytes = std::fs::read(&path).unwrap();
        let tail = bytes.iter().rposition(|&b| b != 0).unwrap() as u64 + 1;
        let f = OpenOptions::new().write(true).open(&path).unwrap();
        f.set_len(tail - 7).unwrap();
        drop(f);

        let mut e = Engine::for_shard(catalog(), 0, 1);
        let (_, n) = Wal::open_and_replay(&path, &mut e).unwrap();
        assert_eq!(n, 1, "torn record must drop whole");
        assert_eq!(e.list("Note").unwrap().len(), 1);
        let _ = std::fs::remove_file(&path);
    }
}
