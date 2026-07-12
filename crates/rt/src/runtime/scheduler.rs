//! Thread-per-core scheduler — plan 09a (`docs/plan/09-concurrency-scaleout.md`).
//!
//! Go parallel: `src/runtime/proc.go`, drastically simplified — there are no
//! goroutines to schedule. `WO_THREADS` OS threads (default: online cores)
//! spawn at boot; each pins itself to a core with `sched_setaffinity`, binds
//! its own `SO_REUSEPORT` listener on the shared port, and runs its own
//! [`EventLoop`] over its own connections. A connection accepted on thread K
//! is driven and closed on thread K — no migration, no work stealing.
//!
//! Per plan 09a, **engine state stays globally shared** (`Arc<Mutex<Engine>>`
//! inside the per-thread `Router`s) — one thing at a time; the sharded engine
//! is 09b. The C proving ground for this exact sequence is
//! `prototypes/wo-rt-c` phase A (see `docs/plan/exploration/c-runtime/`).
//!
//! Shutdown: signals are blocked in `main` before any worker spawns (the
//! mask is inherited), so only worker 0 — which owns the `signalfd` — ever
//! sees SIGINT/SIGTERM. It broadcasts by writing every worker's `eventfd`;
//! each loop wakes, drains its connections, and joins.

use std::collections::HashMap;
use std::io;
use std::os::unix::io::{AsRawFd, RawFd};
use std::sync::Arc;
use std::time::Duration;

use crate::http::{Connection, Listener, Router};

use super::{EventFd, EventLoop, Interest, SignalFd, Token};

const MAX_THREADS: usize = 64;

/// Resolve the worker count: `WO_THREADS` env override, else online cores.
pub fn thread_count() -> usize {
    std::env::var("WO_THREADS")
        .ok()
        .and_then(|s| s.parse::<usize>().ok())
        .filter(|&n| n >= 1)
        .unwrap_or_else(|| {
            std::thread::available_parallelism().map(|n| n.get()).unwrap_or(1)
        })
        .min(MAX_THREADS)
}

fn pin_to_core(core: usize) {
    let cores = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(1);
    unsafe {
        let mut set: libc::cpu_set_t = std::mem::zeroed();
        libc::CPU_ZERO(&mut set);
        libc::CPU_SET(core % cores, &mut set);
        // 0 = the calling thread. Best-effort: a denied affinity (cgroup
        // restrictions) must not stop the worker from serving.
        libc::sched_setaffinity(0, std::mem::size_of::<libc::cpu_set_t>(), &set);
    }
}

/// Everything a worker needs beyond its listener: the router over its own
/// shard, and (since 09b) an optional auxiliary fd + callback — the shard
/// bus's mail eventfd, drained into the local engine when it fires.
pub struct Worker {
    pub router: Router,
    pub mail:   Option<(RawFd, Box<dyn FnMut()>)>,
    /// Group-commit hooks (io_uring WAL): the pollable ring fd, a pump
    /// (flush staged batch / reap completions), a drain of pending
    /// connection unparks `(fd, gen, durable_ok)`, and a parker invoked
    /// when a drive leaves a connection gated on the next fsync.
    pub wal:    Option<WalHooks>,
}

pub struct WalHooks {
    pub ring_fd:   RawFd,
    pub pump:      Box<dyn FnMut()>,
    pub unparks:   Box<dyn FnMut() -> Vec<(RawFd, u64, bool)>>,
    pub park_conn: Box<dyn FnMut(RawFd, u64)>,
}

/// Spawn `thread_count()` pinned workers, each serving `addr` behind
/// `SO_REUSEPORT` with the [`Worker`] built by `worker_fn(id)`. Blocks
/// until a SIGINT/SIGTERM shuts every worker down.
pub fn serve<F>(addr: &str, worker_fn: F) -> anyhow::Result<()>
where
    F: Fn(usize) -> Worker + Send + Sync + 'static,
{
    let n = thread_count();

    // Block SIGINT/SIGTERM NOW — every worker inherits the mask, so the
    // signalfd (owned by worker 0) is the only delivery path.
    let signals = SignalFd::new()?;
    let sig_raw = signals.as_raw_fd();

    let wake: Arc<Vec<EventFd>> =
        Arc::new((0..n).map(|_| EventFd::new()).collect::<io::Result<Vec<_>>>()?);
    let worker_fn = Arc::new(worker_fn);

    let mut handles = Vec::with_capacity(n);
    for t in 0..n {
        let wake      = Arc::clone(&wake);
        let worker_fn = Arc::clone(&worker_fn);
        let addr      = addr.to_string();
        let sigfd     = (t == 0).then_some(sig_raw);
        handles.push(
            std::thread::Builder::new()
                .name(format!("wo-shard-{t}"))
                .spawn(move || worker(t, &addr, sigfd, &wake, &*worker_fn))?,
        );
    }

    for h in handles {
        match h.join() {
            Ok(Ok(()))   => {}
            Ok(Err(e))   => eprintln!("[wo] worker error: {e}"),
            Err(_)       => eprintln!("[wo] worker panicked"),
        }
    }
    drop(signals);
    Ok(())
}

fn worker(
    id: usize,
    addr: &str,
    sigfd: Option<RawFd>,
    wake: &[EventFd],
    worker_fn: &(dyn Fn(usize) -> Worker + Send + Sync),
) -> anyhow::Result<()> {
    pin_to_core(id);

    let listener = match Listener::bind_reuseport(addr) {
        Ok(l) => l,
        Err(e) => {
            // Without a listener this worker is useless — take the whole
            // process down cleanly rather than serving with a hole.
            for w in wake { let _ = w.write(1); }
            anyhow::bail!("shard {id}: bind {addr}: {e}");
        }
    };
    let Worker { router, mut mail, mut wal } = worker_fn(id);

    let mut eloop  = EventLoop::new()?;
    let listen_fd  = listener.as_raw_fd();
    let wake_fd    = wake[id].as_raw_fd();

    eloop.register(listen_fd, Interest::READABLE, Token(listen_fd as u64))?;
    eloop.register(wake_fd,   Interest::READABLE, Token(wake_fd as u64))?;
    if let Some(sfd) = sigfd {
        eloop.register(sfd, Interest::READABLE, Token(sfd as u64))?;
    }
    let mail_fd = mail.as_ref().map(|(fd, _)| *fd);
    if let Some(mfd) = mail_fd {
        eloop.register(mfd, Interest::READABLE, Token(mfd as u64))?;
    }
    let ring_fd = wal.as_ref().map(|w| w.ring_fd);
    if let Some(rfd) = ring_fd {
        eloop.register(rfd, Interest::READABLE, Token(rfd as u64))?;
    }
    let mut next_gen: u64 = 1;

    let mut conns: HashMap<RawFd, Connection> = HashMap::new();

    'outer: loop {
        let events = match eloop.wait_once(Some(Duration::from_secs(60))) {
            Ok(evs) => evs,
            Err(e)  => {
                eprintln!("[wo] shard {id}: event loop error: {e}");
                continue;
            }
        };

        for ev in events {
            let fd = ev.token().0 as RawFd;

            if fd == wake_fd {
                let _ = wake[id].read();
                break 'outer;                       // shutdown broadcast
            }

            if Some(fd) == ring_fd {
                if let Some(w) = wal.as_mut() { (w.pump)(); }
                continue;
            }

            if Some(fd) == mail_fd {
                // Reset the edge, then drain the shard-bus inbox into the
                // local engine. (The eventfd counter is read-and-zeroed;
                // jobs arriving mid-drain re-arm the edge.)
                let mut buf = [0u8; 8];
                unsafe { libc::read(fd, buf.as_mut_ptr() as *mut libc::c_void, 8) };
                if let Some((_, drain)) = mail.as_mut() { drain(); }
                continue;
            }

            if Some(fd) == sigfd {
                // Drain the siginfo and broadcast shutdown to every shard
                // (including ourselves — we exit through the wake path).
                let mut buf = [0u8; 128];           // sizeof(signalfd_siginfo)
                let r = unsafe {
                    libc::read(fd, buf.as_mut_ptr() as *mut libc::c_void, buf.len())
                };
                let signo = if r >= 4 {
                    u32::from_ne_bytes([buf[0], buf[1], buf[2], buf[3]])
                } else { 0 };
                println!();
                println!("[wo] received signal {signo} — broadcasting shutdown to {} shards", wake.len());
                for w in wake { let _ = w.write(1); }
                continue;
            }

            if fd == listen_fd {
                // Drain the accept queue (edge-triggered).
                while let Some(cfd) = listener.accept()? {
                    eloop.register(cfd, Interest::READABLE, Token(cfd as u64))?;
                    conns.insert(cfd, Connection::with_gen(cfd, next_gen));
                    next_gen += 1;
                }
                continue;
            }

            // Connection event — same state machine as the single-threaded
            // loop; the only difference is whose loop it runs on.
            let Some(conn) = conns.get_mut(&fd) else { continue };
            let want_writable = match conn.drive(ev.readable, ev.writable, ev.hangup, ev.error, &router) {
                Ok(w)  => w,
                Err(_) => { conns.remove(&fd); continue; }
            };

            if conn.is_done() {
                eloop.deregister(fd).ok();
                conns.remove(&fd);                  // Drop closes the fd.
            } else if conn.is_parked() {
                let g = conn.gen();
                if let Some(w) = wal.as_mut() { (w.park_conn)(fd, g); }
            } else if want_writable {
                let _ = eloop.modify(fd, Interest::READ_WRITE, Token(fd as u64));
            }
        }

        // End of tick: flush the group-commit batch (one WRITE→FSYNC pair,
        // one syscall) and release any unparks the pumps produced. Released
        // connections may serve pipelined requests that commit again — loop
        // until quiescent so nothing sleeps on an unflushed batch.
        if let Some(w) = wal.as_mut() {
            for _ in 0..64 {
                (w.pump)();
                let pending = (w.unparks)();
                if pending.is_empty() { break; }
                for (fd, gen, ok) in pending {
                    let Some(conn) = conns.get_mut(&fd) else { continue };
                    if conn.gen() != gen || !conn.is_parked() { continue; }
                    if !ok {
                        eloop.deregister(fd).ok();
                        conns.remove(&fd);          // never ack non-durable
                        continue;
                    }
                    conn.unpark();
                    let want_writable = match conn.drive(false, true, false, false, &router) {
                        Ok(wb) => wb,
                        Err(_) => { conns.remove(&fd); continue; }
                    };
                    if conn.is_done() {
                        eloop.deregister(fd).ok();
                        conns.remove(&fd);
                    } else if conn.is_parked() {
                        let g = conn.gen();
                        (w.park_conn)(fd, g);
                    } else if want_writable {
                        let _ = eloop.modify(fd, Interest::READ_WRITE, Token(fd as u64));
                    }
                }
            }
        }
    }

    for (fd, _) in conns.drain() {
        let _ = eloop.deregister(fd);
    }
    Ok(())
}
