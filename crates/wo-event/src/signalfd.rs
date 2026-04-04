use std::io;
use std::os::unix::io::RawFd;

/// Wrapper around Linux signalfd(2) for handling signals via fd.
///
/// Blocks SIGINT and SIGTERM in the process signal mask and delivers
/// them as readable events on the fd instead.
pub struct SignalFd {
    fd: RawFd,
}

impl SignalFd {
    /// Create a signalfd that catches SIGINT and SIGTERM.
    ///
    /// Also blocks these signals in the process signal mask so they
    /// don't trigger default handlers.
    pub fn new() -> io::Result<Self> {
        let mut mask: libc::sigset_t = unsafe { std::mem::zeroed() };
        unsafe {
            libc::sigemptyset(&mut mask);
            libc::sigaddset(&mut mask, libc::SIGINT);
            libc::sigaddset(&mut mask, libc::SIGTERM);

            // Block these signals so they go to signalfd instead.
            let ret = libc::pthread_sigmask(libc::SIG_BLOCK, &mask, std::ptr::null_mut());
            if ret != 0 {
                return Err(io::Error::from_raw_os_error(ret));
            }
        }

        let fd = unsafe { libc::signalfd(-1, &mask, libc::SFD_NONBLOCK | libc::SFD_CLOEXEC) };
        if fd < 0 {
            return Err(io::Error::last_os_error());
        }

        Ok(Self { fd })
    }

    /// Read a pending signal. Returns the signal number (e.g., SIGINT = 2).
    pub fn read(&self) -> io::Result<i32> {
        let mut info: libc::signalfd_siginfo = unsafe { std::mem::zeroed() };
        let size = std::mem::size_of::<libc::signalfd_siginfo>();
        let ret = unsafe {
            libc::read(self.fd, &mut info as *mut _ as *mut libc::c_void, size)
        };
        if ret < 0 {
            Err(io::Error::last_os_error())
        } else {
            Ok(info.ssi_signo as i32)
        }
    }

    pub fn fd(&self) -> RawFd {
        self.fd
    }
}

impl Drop for SignalFd {
    fn drop(&mut self) {
        unsafe { libc::close(self.fd) };
    }
}
