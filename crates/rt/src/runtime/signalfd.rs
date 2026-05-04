//! `signalfd(2)` wrapper — POSIX signals delivered as fd reads.
//!
//! Replaces `tokio::signal::unix::signal` for graceful shutdown: the loop
//! gets `SIGINT` / `SIGTERM` as just another readable fd it can poll.
//!
//! Blocks the captured signals in the calling thread's mask, so the
//! kernel routes them to the signalfd instead of running default handlers.
//! Ported from `reference/crates/wo-event/src/signalfd.rs`.

use std::io;
use std::os::unix::io::{AsRawFd, RawFd};

pub struct SignalFd {
    fd: RawFd,
}

impl SignalFd {
    /// Capture `SIGINT` and `SIGTERM`. Both are blocked process-wide.
    pub fn new() -> io::Result<Self> {
        let mut mask: libc::sigset_t = unsafe { std::mem::zeroed() };
        unsafe {
            libc::sigemptyset(&mut mask);
            libc::sigaddset(&mut mask, libc::SIGINT);
            libc::sigaddset(&mut mask, libc::SIGTERM);
            let ret = libc::pthread_sigmask(libc::SIG_BLOCK, &mask, std::ptr::null_mut());
            if ret != 0 { return Err(io::Error::from_raw_os_error(ret)); }
        }
        let fd = unsafe { libc::signalfd(-1, &mask, libc::SFD_NONBLOCK | libc::SFD_CLOEXEC) };
        if fd < 0 { return Err(io::Error::last_os_error()); }
        Ok(Self { fd })
    }

    /// Read one pending signal. Returns the signal number (e.g. `SIGINT = 2`).
    pub fn read(&self) -> io::Result<i32> {
        let mut info: libc::signalfd_siginfo = unsafe { std::mem::zeroed() };
        let size = std::mem::size_of::<libc::signalfd_siginfo>();
        let ret = unsafe {
            libc::read(self.fd, &mut info as *mut _ as *mut libc::c_void, size)
        };
        if ret < 0 { Err(io::Error::last_os_error()) } else { Ok(info.ssi_signo as i32) }
    }
}

impl AsRawFd for SignalFd {
    fn as_raw_fd(&self) -> RawFd { self.fd }
}

impl Drop for SignalFd {
    fn drop(&mut self) {
        unsafe { libc::close(self.fd) };
    }
}
