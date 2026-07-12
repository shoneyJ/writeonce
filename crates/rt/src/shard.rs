//! The shard bus — plan 09b (`docs/plan/09-concurrency-scaleout.md`).
//!
//! With 09b every worker owns its own [`Engine`] — `Arc<Mutex<Engine>>` is
//! gone. A request that lands on shard K (kernel `SO_REUSEPORT` hash) but
//! targets a row owned by shard J ships a **job** — a boxed closure — to J's
//! mailbox, wakes J's event loop through its mail `eventfd`, and waits for
//! the reply. Two rules keep this deadlock-free:
//!
//! 1. **Jobs never block.** A job is a pure local engine operation on the
//!    owning thread; it cannot itself wait on another shard.
//! 2. **Waiters keep serving.** While shard K waits for J's reply it pumps
//!    its own inbox, so J (or anyone) waiting on K is never starved.
//!
//! Row → owner mapping is the interleaved-id rule (`Engine::for_shard`):
//! shard t mints ids t+1, t+1+n, … so `owner(id) = (id-1) % n` with zero
//! coordination. Creates are always local (the receiving shard mints from
//! its own stride); reads/updates/deletes hop at most once; lists fan out
//! to every shard and merge. The C proving ground for the wake mechanism is
//! `prototypes/wo-rt-c` (eventfd broadcast); the mailbox-per-thread design
//! is plan 09 decision 2 and 09d's one-message-per-thread fan-out shape.

use std::cell::RefCell;
use std::io;
use std::rc::Rc;
use std::sync::mpsc::{channel, Receiver, RecvTimeoutError, Sender};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use crate::engine::Engine;
use crate::runtime::EventFd;

/// A unit of work shipped to the owning shard. Runs against that shard's
/// engine on that shard's thread; replies through whatever channel it
/// captured.
pub type Job = Box<dyn FnOnce(&mut Engine) + Send>;

/// Created once in `main`, shared by every worker: each shard's job sender
/// and mail eventfd. Inboxes are taken (once each) by their owning worker.
pub struct ShardBus {
    senders: Vec<Sender<Job>>,
    wakes:   Vec<EventFd>,
    inboxes: Mutex<Vec<Option<Receiver<Job>>>>,
}

impl ShardBus {
    pub fn new(n: usize) -> io::Result<Arc<Self>> {
        let mut senders = Vec::with_capacity(n);
        let mut inboxes = Vec::with_capacity(n);
        let mut wakes   = Vec::with_capacity(n);
        for _ in 0..n {
            let (tx, rx) = channel();
            senders.push(tx);
            inboxes.push(Some(rx));
            wakes.push(EventFd::new()?);
        }
        Ok(Arc::new(Self { senders, wakes, inboxes: Mutex::new(inboxes) }))
    }

    /// The owning worker claims its inbox at startup. Panics on double-take —
    /// that would be a wiring bug, not a runtime condition.
    pub fn take_inbox(&self, shard: usize) -> Receiver<Job> {
        self.inboxes.lock().unwrap()[shard].take().expect("inbox already taken")
    }

    pub fn mail_fd(&self, shard: usize) -> &EventFd { &self.wakes[shard] }
}

/// Per-worker handle: this shard's engine plus the bus. Deliberately `!Send`
/// (`Rc`/`RefCell`) — it exists on exactly one thread, which is the point.
pub struct ShardCtx {
    pub id: usize,
    pub n:  usize,
    pub engine: Rc<RefCell<Engine>>,
    inbox:  Receiver<Job>,
    bus:    Arc<ShardBus>,
    /// Connection unparks discovered while pumping inside a handler — the
    /// worker loop takes and applies them after the handler returns.
    unparks: RefCell<Vec<(std::os::unix::io::RawFd, u64, bool)>>,
}

impl ShardCtx {
    pub fn new(id: usize, n: usize, engine: Engine, bus: Arc<ShardBus>) -> Rc<Self> {
        let inbox = bus.take_inbox(id);
        Rc::new(Self { id, n, engine: Rc::new(RefCell::new(engine)), inbox, bus,
                       unparks: RefCell::new(Vec::new()) })
    }

    /// Flush + reap this shard's group-commit WAL. Reply parks release
    /// immediately; connection parks queue for the worker loop. Called at
    /// tick end, on ring-fd events, AND from every pump-wait — a waiter
    /// that didn't flush its own batch would deadlock with a peer waiting
    /// on it (cross-shard mutual commit).
    pub fn wal_pump(&self) {
        let completed = {
            let mut e = self.engine.borrow_mut();
            e.wal_flush();
            e.wal_complete()
        };
        if let Some((ok, acks)) = completed {
            for p in acks {
                match p {
                    crate::wal::Parked::Reply(cb) => {
                        // A failed batch drops the callback: the requester's
                        // channel disconnects → 500, never a false ack.
                        if ok { cb() }
                    }
                    crate::wal::Parked::Conn { fd, gen } => {
                        self.unparks.borrow_mut().push((fd, gen, ok));
                    }
                }
            }
        }
    }

    /// Worker loop: take any connection unparks the pumps produced.
    pub fn take_unparks(&self) -> Vec<(std::os::unix::io::RawFd, u64, bool)> {
        std::mem::take(&mut self.unparks.borrow_mut())
    }

    /// Which shard owns a row id, per the interleaved-mint rule.
    pub fn owner_of(&self, row_id: i64) -> usize {
        ((row_id - 1).rem_euclid(self.n as i64)) as usize
    }

    /// Execute every queued job against the local engine. Called from the
    /// event loop on a mail-eventfd event, and from the wait loops below.
    pub fn drain_inbox(&self) {
        while let Ok(job) = self.inbox.try_recv() {
            job(&mut self.engine.borrow_mut());
        }
    }

    /// Run `f` against the engine that owns `owner` — locally if that's us,
    /// else ship it and wait, pumping our own inbox so peers waiting on us
    /// make progress. Returns `None` only if the owner is gone (shutdown).
    pub fn run_on<R, F>(&self, owner: usize, f: F) -> Option<R>
    where
        R: Send + 'static,
        F: FnOnce(&mut Engine) -> R + Send + 'static,
    {
        if owner == self.id {
            return Some(f(&mut self.engine.borrow_mut()));
        }
        let (tx, rx) = channel();
        // Group commit: if the job staged a WAL frame on the owner, its
        // reply parks on the owner's batch and is sent on the fsync CQE —
        // so our requester-side response leaves only after durability.
        let job: Job = Box::new(move |e| {
            let r = f(e);
            if e.take_staged() {
                e.park_reply(Box::new(move || { let _ = tx.send(r); }));
            } else {
                let _ = tx.send(r);
            }
        });
        if self.bus.senders[owner].send(job).is_err() {
            return None;
        }
        let _ = self.bus.wakes[owner].write(1);
        loop {
            match rx.recv_timeout(Duration::from_micros(100)) {
                Ok(r)                              => return Some(r),
                Err(RecvTimeoutError::Timeout)     => { self.drain_inbox(); self.wal_pump(); }
                Err(RecvTimeoutError::Disconnected) => return None,
            }
        }
    }

    /// Run `f` on every shard (self included) and collect the results.
    /// Cross-shard reads — `list` — are the fan-out-and-merge case.
    pub fn fanout<R, F>(&self, f: F) -> Vec<R>
    where
        R: Send + 'static,
        F: Fn(&mut Engine) -> R + Send + Sync + Clone + 'static,
    {
        let (tx, rx) = channel();
        let mut remote = 0usize;
        for (t, sender) in self.bus.senders.iter().enumerate() {
            if t == self.id { continue; }
            let tx = tx.clone();
            let f  = f.clone();
            let job: Job = Box::new(move |e| { let _ = tx.send(f(e)); });
            if sender.send(job).is_ok() {
                let _ = self.bus.wakes[t].write(1);
                remote += 1;
            }
        }
        let mut out = Vec::with_capacity(remote + 1);
        out.push(f(&mut self.engine.borrow_mut()));
        while out.len() < remote + 1 {
            match rx.recv_timeout(Duration::from_micros(100)) {
                Ok(r)                               => out.push(r),
                Err(RecvTimeoutError::Timeout)      => { self.drain_inbox(); self.wal_pump(); }
                Err(RecvTimeoutError::Disconnected) => break,
            }
        }
        out
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::compile::Catalog;
    use crate::parser::parse;
    use serde_json::json;

    fn catalog() -> Catalog {
        Catalog::from_schemas(vec![parse(
            r#"type Note { id: Id
                           title: Text
                           service rest "/api/notes" expose list, get, create }"#,
        ).unwrap()]).unwrap()
    }

    #[test]
    fn interleaved_ids_map_back_to_their_shard() {
        let cat = catalog();
        let bus = ShardBus::new(3).unwrap();
        let ctx0 = ShardCtx::new(0, 3, Engine::for_shard(cat.clone(), 0, 3), bus.clone());
        let ctx1 = ShardCtx::new(1, 3, Engine::for_shard(cat.clone(), 1, 3), bus.clone());

        let a = ctx0.engine.borrow_mut().create("Note", json!({"title":"a"})).unwrap();
        let b = ctx0.engine.borrow_mut().create("Note", json!({"title":"b"})).unwrap();
        let c = ctx1.engine.borrow_mut().create("Note", json!({"title":"c"})).unwrap();
        let (a, b, c) = (a["id"].as_i64().unwrap(), b["id"].as_i64().unwrap(), c["id"].as_i64().unwrap());
        assert_eq!((a, b, c), (1, 4, 2));
        assert_eq!(ctx0.owner_of(a), 0);
        assert_eq!(ctx0.owner_of(b), 0);
        assert_eq!(ctx0.owner_of(c), 1);
    }

    #[test]
    fn cross_shard_job_round_trips() {
        let cat = catalog();
        let bus = ShardBus::new(2).unwrap();
        let ctx1 = ShardCtx::new(1, 2, Engine::for_shard(cat.clone(), 1, 2), bus.clone());

        // Shard 1's thread: serve jobs (one drain after the send below).
        let bus2 = bus.clone();
        let cat2 = cat.clone();
        let t = std::thread::spawn(move || {
            // shard 0 lives on this thread
            let ctx0 = ShardCtx::new(0, 2, Engine::for_shard(cat2, 0, 2), bus2);
            ctx0.engine.borrow_mut().create("Note", json!({"title":"on-zero"})).unwrap();
            // serve until the job arrives
            for _ in 0..1000 {
                ctx0.drain_inbox();
                std::thread::sleep(Duration::from_micros(200));
            }
        });

        // From shard 1, read the row owned by shard 0 (id 1).
        let row = ctx1.run_on(0, |e| e.get("Note", 1).unwrap()).flatten();
        assert_eq!(row.unwrap()["title"], "on-zero");
        drop(ctx1);
        t.join().unwrap();
    }
}
