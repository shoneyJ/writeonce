use std::io;
use std::os::unix::io::RawFd;
use std::time::Duration;

/// Caller-assigned identifier for a registered file descriptor.
pub type Token = u64;

/// Interest flags for epoll registration.
#[derive(Debug, Clone, Copy)]
pub enum Interest {
    Readable,
    Writable,
    ReadWrite,
}

impl Interest {
    fn to_epoll_flags(self) -> u32 {
        match self {
            Interest::Readable => libc::EPOLLIN as u32,
            Interest::Writable => libc::EPOLLOUT as u32,
            Interest::ReadWrite => (libc::EPOLLIN | libc::EPOLLOUT) as u32,
        }
    }
}

/// An event delivered by the event loop.
#[derive(Debug, Clone)]
pub struct Event {
    pub token: Token,
    pub readable: bool,
    pub writable: bool,
    pub error: bool,
    pub hangup: bool,
}

/// Single-threaded event loop built on epoll.
pub struct EventLoop {
    epoll_fd: RawFd,
}

impl EventLoop {
    /// Create a new event loop.
    pub fn new() -> io::Result<Self> {
        let fd = unsafe { libc::epoll_create1(libc::EPOLL_CLOEXEC) };
        if fd < 0 {
            return Err(io::Error::last_os_error());
        }
        Ok(Self { epoll_fd: fd })
    }

    /// Register a file descriptor for the given interest.
    pub fn register(&self, fd: RawFd, interest: Interest, token: Token) -> io::Result<()> {
        let mut event = libc::epoll_event {
            events: interest.to_epoll_flags() | libc::EPOLLRDHUP as u32,
            u64: token,
        };
        let ret = unsafe { libc::epoll_ctl(self.epoll_fd, libc::EPOLL_CTL_ADD, fd, &mut event) };
        if ret < 0 {
            Err(io::Error::last_os_error())
        } else {
            Ok(())
        }
    }

    /// Modify interest for an already-registered fd.
    pub fn modify(&self, fd: RawFd, interest: Interest, token: Token) -> io::Result<()> {
        let mut event = libc::epoll_event {
            events: interest.to_epoll_flags() | libc::EPOLLRDHUP as u32,
            u64: token,
        };
        let ret = unsafe { libc::epoll_ctl(self.epoll_fd, libc::EPOLL_CTL_MOD, fd, &mut event) };
        if ret < 0 {
            Err(io::Error::last_os_error())
        } else {
            Ok(())
        }
    }

    /// Remove a file descriptor from the event loop.
    pub fn deregister(&self, fd: RawFd) -> io::Result<()> {
        let ret = unsafe {
            libc::epoll_ctl(self.epoll_fd, libc::EPOLL_CTL_DEL, fd, std::ptr::null_mut())
        };
        if ret < 0 {
            Err(io::Error::last_os_error())
        } else {
            Ok(())
        }
    }

    /// Wait for events. Returns when at least one event is ready or timeout expires.
    ///
    /// `timeout`: `None` blocks indefinitely, `Some(duration)` sets a timeout.
    /// Returns up to 64 events per call.
    pub fn poll(&self, timeout: Option<Duration>) -> io::Result<Vec<Event>> {
        let timeout_ms = match timeout {
            None => -1i32,
            Some(d) => d.as_millis() as i32,
        };

        let mut events = [libc::epoll_event { events: 0, u64: 0 }; 64];
        let n = unsafe {
            libc::epoll_wait(self.epoll_fd, events.as_mut_ptr(), events.len() as i32, timeout_ms)
        };

        if n < 0 {
            let err = io::Error::last_os_error();
            // EINTR is not an error — just return empty.
            if err.raw_os_error() == Some(libc::EINTR) {
                return Ok(vec![]);
            }
            return Err(err);
        }

        let result = (0..n as usize)
            .map(|i| {
                let e = events[i].events;
                Event {
                    token: events[i].u64,
                    readable: (e & libc::EPOLLIN as u32) != 0,
                    writable: (e & libc::EPOLLOUT as u32) != 0,
                    error: (e & libc::EPOLLERR as u32) != 0,
                    hangup: (e & (libc::EPOLLHUP | libc::EPOLLRDHUP) as u32) != 0,
                }
            })
            .collect();

        Ok(result)
    }

    /// Get the raw epoll file descriptor.
    pub fn fd(&self) -> RawFd {
        self.epoll_fd
    }
}

impl Drop for EventLoop {
    fn drop(&mut self) {
        unsafe { libc::close(self.epoll_fd) };
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::EventFd;

    #[test]
    fn register_and_poll_eventfd() {
        let eloop = EventLoop::new().unwrap();
        let efd = EventFd::new().unwrap();

        eloop.register(efd.fd(), Interest::Readable, 42).unwrap();

        // Write to eventfd from the same thread.
        efd.write(1).unwrap();

        let events = eloop.poll(Some(Duration::from_millis(100))).unwrap();
        assert_eq!(events.len(), 1);
        assert_eq!(events[0].token, 42);
        assert!(events[0].readable);
    }

    #[test]
    fn poll_timeout_no_events() {
        let eloop = EventLoop::new().unwrap();
        let events = eloop.poll(Some(Duration::from_millis(10))).unwrap();
        assert!(events.is_empty());
    }

    #[test]
    fn deregister() {
        let eloop = EventLoop::new().unwrap();
        let efd = EventFd::new().unwrap();

        eloop.register(efd.fd(), Interest::Readable, 1).unwrap();
        eloop.deregister(efd.fd()).unwrap();

        efd.write(1).unwrap();
        let events = eloop.poll(Some(Duration::from_millis(10))).unwrap();
        assert!(events.is_empty());
    }
}
