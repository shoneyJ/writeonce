//! `epoll`-backed event loop. Single-threaded, edge-triggered.
//!
//! Ported from `reference/crates/wo-event/src/epoll.rs`. Differences:
//!   * `Token` is a newtype rather than a `u64` alias.
//!   * `Interest` is a struct exposing `READABLE`, `WRITABLE`, `READ_WRITE`
//!     constants, matching the API in `docs/plan/02-event-loop-epoll.md`.
//!   * Every registration sets `EPOLLET`; callers must read to `EAGAIN`.
//!   * `wait_once` reuses an internal event buffer instead of allocating
//!     a fresh `[epoll_event; 64]` per call.

use std::io;
use std::os::unix::io::RawFd;
use std::time::Duration;

/// Caller-assigned identifier for a registered file descriptor.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct Token(pub u64);

/// Interest flags for epoll registration.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Interest(u32);

impl Interest {
    pub const READABLE:   Interest = Interest(libc::EPOLLIN as u32);
    pub const WRITABLE:   Interest = Interest(libc::EPOLLOUT as u32);
    pub const READ_WRITE: Interest = Interest((libc::EPOLLIN | libc::EPOLLOUT) as u32);

    fn bits(self) -> u32 { self.0 }
}

/// One readiness event from the loop.
#[derive(Debug, Clone)]
pub struct Event {
    token:        Token,
    pub readable: bool,
    pub writable: bool,
    pub error:    bool,
    pub hangup:   bool,
}

impl Event {
    pub fn token(&self) -> Token { self.token }
}

const EVENT_BUF: usize = 64;

/// Single-threaded event loop built on `epoll_create1` + `epoll_wait`.
pub struct EventLoop {
    epoll_fd: RawFd,
    events:   Vec<libc::epoll_event>,
}

impl EventLoop {
    pub fn new() -> io::Result<Self> {
        let fd = unsafe { libc::epoll_create1(libc::EPOLL_CLOEXEC) };
        if fd < 0 { return Err(io::Error::last_os_error()); }
        Ok(Self {
            epoll_fd: fd,
            events:   vec![libc::epoll_event { events: 0, u64: 0 }; EVENT_BUF],
        })
    }

    pub fn register(&self, fd: RawFd, interest: Interest, token: Token) -> io::Result<()> {
        self.ctl(libc::EPOLL_CTL_ADD, fd, interest, token)
    }

    pub fn modify(&self, fd: RawFd, interest: Interest, token: Token) -> io::Result<()> {
        self.ctl(libc::EPOLL_CTL_MOD, fd, interest, token)
    }

    pub fn deregister(&self, fd: RawFd) -> io::Result<()> {
        let ret = unsafe {
            libc::epoll_ctl(self.epoll_fd, libc::EPOLL_CTL_DEL, fd, std::ptr::null_mut())
        };
        if ret < 0 { Err(io::Error::last_os_error()) } else { Ok(()) }
    }

    fn ctl(&self, op: libc::c_int, fd: RawFd, interest: Interest, token: Token) -> io::Result<()> {
        let mut ev = libc::epoll_event {
            events: interest.bits() | libc::EPOLLET as u32 | libc::EPOLLRDHUP as u32,
            u64:    token.0,
        };
        let ret = unsafe { libc::epoll_ctl(self.epoll_fd, op, fd, &mut ev) };
        if ret < 0 { Err(io::Error::last_os_error()) } else { Ok(()) }
    }

    /// Block until at least one fd is ready or `timeout` expires.
    /// `None` blocks indefinitely. Returns up to 64 events per call.
    pub fn wait_once(&mut self, timeout: Option<Duration>) -> io::Result<Vec<Event>> {
        let timeout_ms: i32 = match timeout {
            None    => -1,
            Some(d) => d.as_millis().min(i32::MAX as u128) as i32,
        };
        let n = unsafe {
            libc::epoll_wait(
                self.epoll_fd,
                self.events.as_mut_ptr(),
                self.events.len() as i32,
                timeout_ms,
            )
        };
        if n < 0 {
            let err = io::Error::last_os_error();
            if err.raw_os_error() == Some(libc::EINTR) { return Ok(vec![]); }
            return Err(err);
        }
        Ok((0..n as usize)
            .map(|i| {
                let bits = self.events[i].events;
                Event {
                    token:    Token(self.events[i].u64),
                    readable: bits & libc::EPOLLIN  as u32 != 0,
                    writable: bits & libc::EPOLLOUT as u32 != 0,
                    error:    bits & libc::EPOLLERR as u32 != 0,
                    hangup:   bits & (libc::EPOLLHUP | libc::EPOLLRDHUP) as u32 != 0,
                }
            })
            .collect())
    }

    pub fn fd(&self) -> RawFd { self.epoll_fd }
}

impl Drop for EventLoop {
    fn drop(&mut self) {
        unsafe { libc::close(self.epoll_fd) };
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::runtime::EventFd;
    use std::os::unix::io::AsRawFd;

    #[test]
    fn wait_once_returns_eventfd_token() {
        let mut eloop = EventLoop::new().unwrap();
        let efd       = EventFd::new().unwrap();

        eloop.register(efd.as_raw_fd(), Interest::READABLE, Token(42)).unwrap();
        efd.write(1).unwrap();

        let events = eloop.wait_once(Some(Duration::from_millis(100))).unwrap();
        assert_eq!(events.len(), 1);
        assert_eq!(events[0].token(), Token(42));
        assert!(events[0].readable);
        assert_eq!(efd.read().unwrap(), 1);
    }

    #[test]
    fn wait_once_times_out_with_no_events() {
        let mut eloop = EventLoop::new().unwrap();
        let events = eloop.wait_once(Some(Duration::from_millis(10))).unwrap();
        assert!(events.is_empty());
    }

    #[test]
    fn deregister_silences_fd() {
        let mut eloop = EventLoop::new().unwrap();
        let efd       = EventFd::new().unwrap();

        eloop.register(efd.as_raw_fd(), Interest::READABLE, Token(1)).unwrap();
        eloop.deregister(efd.as_raw_fd()).unwrap();

        efd.write(1).unwrap();
        let events = eloop.wait_once(Some(Duration::from_millis(10))).unwrap();
        assert!(events.is_empty());
    }
}
