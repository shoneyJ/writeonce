//! `eventfd(2)` wrapper — counter semaphore exposed as a file descriptor.
//!
//! Used as the cross-flow wake-up primitive: any code path that needs the
//! event loop to come back and run something writes a `1` to the eventfd,
//! which becomes readable on the loop's next `wait_once`.
//!
//! Ported from `reference/crates/wo-event/src/eventfd.rs` with an added
//! `AsRawFd` impl so callers can drop the fd straight into `EventLoop`.

use std::io;
use std::os::unix::io::{AsRawFd, RawFd};

pub struct EventFd {
    fd: RawFd,
}

impl EventFd {
    /// Create a non-blocking, close-on-exec eventfd with initial counter 0.
    pub fn new() -> io::Result<Self> {
        let fd = unsafe { libc::eventfd(0, libc::EFD_NONBLOCK | libc::EFD_CLOEXEC) };
        if fd < 0 { return Err(io::Error::last_os_error()); }
        Ok(Self { fd })
    }

    /// Add `val` to the counter. Wakes any waiter when the counter goes 0→non-zero.
    pub fn write(&self, val: u64) -> io::Result<()> {
        let buf = val.to_ne_bytes();
        let ret = unsafe {
            libc::write(self.fd, buf.as_ptr() as *const libc::c_void, 8)
        };
        if ret < 0 { Err(io::Error::last_os_error()) } else { Ok(()) }
    }

    /// Atomically read and zero the counter. Returns `EAGAIN` if the
    /// counter is already 0 (since we set `EFD_NONBLOCK`).
    pub fn read(&self) -> io::Result<u64> {
        let mut buf = [0u8; 8];
        let ret = unsafe {
            libc::read(self.fd, buf.as_mut_ptr() as *mut libc::c_void, 8)
        };
        if ret < 0 { Err(io::Error::last_os_error()) } else { Ok(u64::from_ne_bytes(buf)) }
    }
}

impl AsRawFd for EventFd {
    fn as_raw_fd(&self) -> RawFd { self.fd }
}

impl Drop for EventFd {
    fn drop(&mut self) {
        unsafe { libc::close(self.fd) };
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn writes_accumulate() {
        let efd = EventFd::new().unwrap();
        efd.write(5).unwrap();
        efd.write(3).unwrap();
        assert_eq!(efd.read().unwrap(), 8);
    }
}
