//! Raw io_uring — no liburing, kernel ABI structs defined by hand, exactly
//! the sequence proven in C (`prototypes/wo-rt-c/wo-rt.c` ring_init/enter;
//! card: `docs/plan/exploration/linux/07-io_uring.md`).
//!
//! Scope (this phase): the **storage ring** for per-shard group commit —
//! batched WAL `WRITE` + hard-linked `FSYNC` SQEs, one `io_uring_enter` per
//! flush. The ring fd is pollable, so it registers in the existing epoll
//! loop and completions arrive as just another readable event; the full
//! network port (accept/recv/send SQEs) is a later phase.
//!
//! Requires `IORING_FEAT_SINGLE_MMAP` (kernel ≥ 5.4).

use std::io;
use std::os::unix::io::RawFd;
use std::sync::atomic::{AtomicU32, Ordering};

const SYS_SETUP: libc::c_long = 425;
const SYS_ENTER: libc::c_long = 426;

const IORING_OFF_SQ_RING: i64 = 0;
const IORING_OFF_SQES:    i64 = 0x1000_0000;
const IORING_ENTER_GETEVENTS: u32 = 1;
const IORING_FEAT_SINGLE_MMAP: u32 = 1;

pub const OP_FSYNC: u8 = 3;
pub const OP_WRITE: u8 = 23;
pub const IOSQE_IO_LINK: u8 = 1 << 2;
pub const FSYNC_DATASYNC: u32 = 1;

#[repr(C)]
#[derive(Default)]
struct SqOffsets { head: u32, tail: u32, ring_mask: u32, ring_entries: u32, flags: u32, dropped: u32, array: u32, resv1: u32, user_addr: u64 }

#[repr(C)]
#[derive(Default)]
struct CqOffsets { head: u32, tail: u32, ring_mask: u32, ring_entries: u32, overflow: u32, cqes: u32, flags: u32, resv1: u32, user_addr: u64 }

#[repr(C)]
#[derive(Default)]
struct Params {
    sq_entries: u32, cq_entries: u32, flags: u32,
    sq_thread_cpu: u32, sq_thread_idle: u32,
    features: u32, wq_fd: u32, resv: [u32; 3],
    sq_off: SqOffsets, cq_off: CqOffsets,
}

/// One submission-queue entry — the 64-byte kernel layout.
#[repr(C)]
#[derive(Clone, Copy, Default)]
struct Sqe {
    opcode: u8, flags: u8, ioprio: u16, fd: i32,
    off: u64, addr: u64, len: u32, op_flags: u32,
    user_data: u64,
    buf_index: u16, personality: u16, splice_fd_in: i32,
    _pad: [u64; 2],
}

#[repr(C)]
#[derive(Clone, Copy)]
struct Cqe { user_data: u64, res: i32, flags: u32 }

pub struct Uring {
    fd:       RawFd,
    // SQ ring pointers (into the shared mmap)
    sq_tail:  *const AtomicU32,
    sq_mask:  u32,
    sq_array: *mut u32,
    // CQ ring pointers
    cq_head:  *const AtomicU32,
    cq_tail:  *const AtomicU32,
    cq_mask:  u32,
    cqes:     *const Cqe,
    sqes:     *mut Sqe,
    local_tail: u32,
    to_submit:  u32,
}

// The ring is owned and driven by exactly one worker thread (plan 09
// decision 4); raw pointers into its own mmaps don't change that.
unsafe impl Send for Uring {}

impl Uring {
    pub fn new(entries: u32) -> io::Result<Self> {
        let mut p = Params::default();
        let fd = unsafe { libc::syscall(SYS_SETUP, entries, &mut p as *mut Params) } as RawFd;
        if fd < 0 { return Err(io::Error::last_os_error()); }
        if p.features & IORING_FEAT_SINGLE_MMAP == 0 {
            unsafe { libc::close(fd) };
            return Err(io::Error::other("kernel lacks IORING_FEAT_SINGLE_MMAP (need >= 5.4)"));
        }

        let sq_sz = p.sq_off.array as usize + p.sq_entries as usize * 4;
        let cq_sz = p.cq_off.cqes  as usize + p.cq_entries as usize * std::mem::size_of::<Cqe>();
        let ring_sz = sq_sz.max(cq_sz);
        let ring = unsafe {
            libc::mmap(std::ptr::null_mut(), ring_sz, libc::PROT_READ | libc::PROT_WRITE,
                       libc::MAP_SHARED | libc::MAP_POPULATE, fd, IORING_OFF_SQ_RING)
        };
        if ring == libc::MAP_FAILED { let e = io::Error::last_os_error(); unsafe { libc::close(fd) }; return Err(e); }

        let sqes_sz = p.sq_entries as usize * std::mem::size_of::<Sqe>();
        let sqes = unsafe {
            libc::mmap(std::ptr::null_mut(), sqes_sz, libc::PROT_READ | libc::PROT_WRITE,
                       libc::MAP_SHARED | libc::MAP_POPULATE, fd, IORING_OFF_SQES)
        };
        if sqes == libc::MAP_FAILED { let e = io::Error::last_os_error(); unsafe { libc::close(fd) }; return Err(e); }

        let at = |off: u32| unsafe { (ring as *mut u8).add(off as usize) };
        let sq_mask = unsafe { *(at(p.sq_off.ring_mask) as *const u32) };
        let cq_mask = unsafe { *(at(p.cq_off.ring_mask) as *const u32) };
        let sq_tail = at(p.sq_off.tail) as *const AtomicU32;
        let local_tail = unsafe { (*sq_tail).load(Ordering::Relaxed) };

        Ok(Self {
            fd,
            sq_tail,
            sq_mask,
            sq_array: at(p.sq_off.array) as *mut u32,
            cq_head:  at(p.cq_off.head) as *const AtomicU32,
            cq_tail:  at(p.cq_off.tail) as *const AtomicU32,
            cq_mask,
            cqes:     at(p.cq_off.cqes) as *const Cqe,
            sqes:     sqes as *mut Sqe,
            local_tail,
            to_submit: 0,
        })
    }

    /// The ring fd — readable when completions are pending, so it registers
    /// straight into the epoll loop.
    pub fn as_raw_fd(&self) -> RawFd { self.fd }

    fn sqe(&mut self) -> &mut Sqe {
        let idx = self.local_tail & self.sq_mask;
        unsafe { *self.sq_array.add(idx as usize) = idx; }
        self.local_tail = self.local_tail.wrapping_add(1);
        self.to_submit += 1;
        let s = unsafe { &mut *self.sqes.add(idx as usize) };
        *s = Sqe::default();
        s
    }

    /// Queue a positional write. SAFETY contract: `buf` must stay alive and
    /// unmoved until this op's CQE is reaped — the caller double-buffers.
    pub fn push_write(&mut self, fd: RawFd, buf: &[u8], offset: u64, link: bool, user_data: u64) {
        let s = self.sqe();
        s.opcode    = OP_WRITE;
        s.fd        = fd;
        s.addr      = buf.as_ptr() as u64;
        s.len       = buf.len() as u32;
        s.off       = offset;
        s.flags     = if link { IOSQE_IO_LINK } else { 0 };
        s.user_data = user_data;
    }

    pub fn push_fsync(&mut self, fd: RawFd, user_data: u64) {
        let s = self.sqe();
        s.opcode    = OP_FSYNC;
        s.fd        = fd;
        s.op_flags  = FSYNC_DATASYNC;
        s.user_data = user_data;
    }

    /// Publish queued SQEs with one syscall. Non-blocking — completions are
    /// observed via epoll on the ring fd.
    pub fn submit(&mut self) -> io::Result<()> {
        if self.to_submit == 0 { return Ok(()); }
        unsafe { (*self.sq_tail).store(self.local_tail, Ordering::Release); }
        let n = self.to_submit;
        self.to_submit = 0;
        loop {
            let r = unsafe { libc::syscall(SYS_ENTER, self.fd, n, 0u32, IORING_ENTER_GETEVENTS, 0usize, 0usize) };
            if r >= 0 { return Ok(()); }
            let e = io::Error::last_os_error();
            if e.raw_os_error() != Some(libc::EINTR) { return Err(e); }
        }
    }

    /// Reap every pending completion as `(user_data, res)`.
    pub fn pop_cqes(&mut self) -> Vec<(u64, i32)> {
        let mut out = Vec::new();
        unsafe {
            let mut head = (*self.cq_head).load(Ordering::Relaxed);
            let tail = (*self.cq_tail).load(Ordering::Acquire);
            while head != tail {
                let cqe = &*self.cqes.add((head & self.cq_mask) as usize);
                out.push((cqe.user_data, cqe.res));
                head = head.wrapping_add(1);
            }
            (*self.cq_head).store(head, Ordering::Release);
        }
        out
    }
}

impl Drop for Uring {
    fn drop(&mut self) {
        unsafe { libc::close(self.fd) };
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Read;

    #[test]
    fn write_then_linked_fsync_round_trips() {
        let path = std::env::temp_dir().join(format!("wo-uring-test-{}", std::process::id()));
        let _ = std::fs::remove_file(&path);
        let file = std::fs::OpenOptions::new().read(true).write(true).create(true).open(&path).unwrap();
        use std::os::unix::io::AsRawFd;

        let mut ring = Uring::new(8).expect("io_uring available");
        let buf = b"hello-from-the-ring".to_vec();
        ring.push_write(file.as_raw_fd(), &buf, 0, true, 1);
        ring.push_fsync(file.as_raw_fd(), 2);
        ring.submit().unwrap();

        // Poll the ring fd until both CQEs arrive.
        let mut got = Vec::new();
        for _ in 0..200 {
            got.extend(ring.pop_cqes());
            if got.len() >= 2 { break; }
            std::thread::sleep(std::time::Duration::from_millis(1));
        }
        assert_eq!(got.len(), 2, "write + fsync completions");
        assert_eq!(got[0], (1, buf.len() as i32), "write res = full length");
        assert_eq!(got[1].0, 2);
        assert!(got[1].1 >= 0, "fsync ok");

        let mut s = String::new();
        std::fs::File::open(&path).unwrap().read_to_string(&mut s).unwrap();
        assert_eq!(s, "hello-from-the-ring");
        let _ = std::fs::remove_file(&path);
    }
}
