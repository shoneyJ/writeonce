//! `timerfd_create(2)` wrapper — timers as file descriptors.
//!
//! Both one-shot and periodic timers are armed with `timerfd_settime`. The
//! fd becomes readable when the timer expires; reading drains the
//! expiration count.
//!
//! Ported from `reference/crates/wo-event/src/timerfd.rs`. Adds `oneshot`
//! and `periodic` constructors that match the API in the phase-02 plan.

use std::io;
use std::os::unix::io::{AsRawFd, RawFd};
use std::time::Duration;

pub struct TimerFd {
    fd: RawFd,
}

impl TimerFd {
    /// Create a disarmed monotonic timer fd (non-blocking, close-on-exec).
    pub fn new() -> io::Result<Self> {
        let fd = unsafe {
            libc::timerfd_create(
                libc::CLOCK_MONOTONIC,
                libc::TFD_NONBLOCK | libc::TFD_CLOEXEC,
            )
        };
        if fd < 0 { return Err(io::Error::last_os_error()); }
        Ok(Self { fd })
    }

    /// Convenience: a fresh fd armed to fire once after `after`.
    pub fn oneshot(after: Duration) -> io::Result<Self> {
        let t = Self::new()?;
        t.set(after, Duration::ZERO)?;
        Ok(t)
    }

    /// Convenience: a fresh fd that fires every `every` (first tick at +`every`).
    pub fn periodic(every: Duration) -> io::Result<Self> {
        let t = Self::new()?;
        t.set(every, every)?;
        Ok(t)
    }

    /// Arm: fire once after `initial`, then repeat every `interval`.
    /// Pass `Duration::ZERO` for `interval` to make it one-shot.
    pub fn set(&self, initial: Duration, interval: Duration) -> io::Result<()> {
        let spec = libc::itimerspec {
            it_interval: timespec(interval),
            it_value:    timespec(initial),
        };
        let ret = unsafe {
            libc::timerfd_settime(self.fd, 0, &spec, std::ptr::null_mut())
        };
        if ret < 0 { Err(io::Error::last_os_error()) } else { Ok(()) }
    }

    /// Read the number of expirations since the previous read.
    pub fn read(&self) -> io::Result<u64> {
        let mut buf = [0u8; 8];
        let ret = unsafe {
            libc::read(self.fd, buf.as_mut_ptr() as *mut libc::c_void, 8)
        };
        if ret < 0 { Err(io::Error::last_os_error()) } else { Ok(u64::from_ne_bytes(buf)) }
    }
}

impl AsRawFd for TimerFd {
    fn as_raw_fd(&self) -> RawFd { self.fd }
}

impl Drop for TimerFd {
    fn drop(&mut self) {
        unsafe { libc::close(self.fd) };
    }
}

fn timespec(d: Duration) -> libc::timespec {
    libc::timespec {
        tv_sec:  d.as_secs() as libc::time_t,
        tv_nsec: d.subsec_nanos() as libc::c_long,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::runtime::{EventLoop, Interest, Token};

    #[test]
    fn oneshot_fires_within_window() {
        let mut eloop = EventLoop::new().unwrap();
        let timer     = TimerFd::oneshot(Duration::from_millis(100)).unwrap();

        eloop.register(timer.as_raw_fd(), Interest::READABLE, Token(99)).unwrap();

        let events = eloop.wait_once(Some(Duration::from_millis(500))).unwrap();
        assert!(!events.is_empty(), "expected timer event within 500ms");
        assert_eq!(events[0].token(), Token(99));
        assert!(timer.read().unwrap() >= 1);
    }
}
