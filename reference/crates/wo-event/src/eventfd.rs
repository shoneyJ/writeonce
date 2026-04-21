use std::io;
use std::os::unix::io::RawFd;

/// Wrapper around Linux eventfd(2) for lightweight signaling.
pub struct EventFd {
    fd: RawFd,
}

impl EventFd {
    /// Create a new eventfd with initial value 0.
    pub fn new() -> io::Result<Self> {
        let fd = unsafe { libc::eventfd(0, libc::EFD_NONBLOCK | libc::EFD_CLOEXEC) };
        if fd < 0 {
            return Err(io::Error::last_os_error());
        }
        Ok(Self { fd })
    }

    /// Write a value to the eventfd (signals waiters).
    pub fn write(&self, val: u64) -> io::Result<()> {
        let buf = val.to_ne_bytes();
        let ret = unsafe { libc::write(self.fd, buf.as_ptr() as *const libc::c_void, 8) };
        if ret < 0 {
            Err(io::Error::last_os_error())
        } else {
            Ok(())
        }
    }

    /// Read the current value (resets to 0).
    pub fn read(&self) -> io::Result<u64> {
        let mut buf = [0u8; 8];
        let ret = unsafe { libc::read(self.fd, buf.as_mut_ptr() as *mut libc::c_void, 8) };
        if ret < 0 {
            Err(io::Error::last_os_error())
        } else {
            Ok(u64::from_ne_bytes(buf))
        }
    }

    /// Get the raw file descriptor for epoll registration.
    pub fn fd(&self) -> RawFd {
        self.fd
    }
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
    fn write_and_read() {
        let efd = EventFd::new().unwrap();
        efd.write(5).unwrap();
        efd.write(3).unwrap();
        // eventfd accumulates writes.
        let val = efd.read().unwrap();
        assert_eq!(val, 8);
    }
}
