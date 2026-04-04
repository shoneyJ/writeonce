use std::io;
use std::os::unix::io::RawFd;
use std::time::Duration;

/// Wrapper around Linux timerfd_create(2) for timer-as-fd.
pub struct TimerFd {
    fd: RawFd,
}

impl TimerFd {
    /// Create a new monotonic timer fd.
    pub fn new() -> io::Result<Self> {
        let fd = unsafe {
            libc::timerfd_create(
                libc::CLOCK_MONOTONIC,
                libc::TFD_NONBLOCK | libc::TFD_CLOEXEC,
            )
        };
        if fd < 0 {
            return Err(io::Error::last_os_error());
        }
        Ok(Self { fd })
    }

    /// Arm the timer to fire once after `initial` and then repeat every `interval`.
    ///
    /// Pass `Duration::ZERO` for `interval` for a one-shot timer.
    pub fn set(&self, initial: Duration, interval: Duration) -> io::Result<()> {
        let spec = libc::itimerspec {
            it_interval: duration_to_timespec(interval),
            it_value: duration_to_timespec(initial),
        };
        let ret =
            unsafe { libc::timerfd_settime(self.fd, 0, &spec, std::ptr::null_mut()) };
        if ret < 0 {
            Err(io::Error::last_os_error())
        } else {
            Ok(())
        }
    }

    /// Read the number of expirations since last read.
    pub fn read(&self) -> io::Result<u64> {
        let mut buf = [0u8; 8];
        let ret = unsafe { libc::read(self.fd, buf.as_mut_ptr() as *mut libc::c_void, 8) };
        if ret < 0 {
            Err(io::Error::last_os_error())
        } else {
            Ok(u64::from_ne_bytes(buf))
        }
    }

    pub fn fd(&self) -> RawFd {
        self.fd
    }
}

impl Drop for TimerFd {
    fn drop(&mut self) {
        unsafe { libc::close(self.fd) };
    }
}

fn duration_to_timespec(d: Duration) -> libc::timespec {
    libc::timespec {
        tv_sec: d.as_secs() as libc::time_t,
        tv_nsec: d.subsec_nanos() as libc::c_long,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{EventLoop, Interest};

    #[test]
    fn timer_fires() {
        let eloop = EventLoop::new().unwrap();
        let timer = TimerFd::new().unwrap();

        timer.set(Duration::from_millis(20), Duration::ZERO).unwrap();
        eloop.register(timer.fd(), Interest::Readable, 99).unwrap();

        let events = eloop.poll(Some(Duration::from_millis(100))).unwrap();
        assert!(!events.is_empty());
        assert_eq!(events[0].token, 99);

        let expirations = timer.read().unwrap();
        assert!(expirations >= 1);
    }
}
