//! Non-blocking TCP listener — `socket(2)` + `bind(2)` + `listen(2)` + `accept4(2)`.
//!
//! Adapted from `reference/crates/wo-http/src/listener.rs`. The v1 hand-rolled
//! IPv4 parser had a byte-order bug for non-localhost addresses; here we
//! defer to `std::net::SocketAddr` (stdlib, no extra crate) and convert the
//! resulting octets to a `sockaddr_in` correctly.

use std::io;
use std::net::SocketAddr;
use std::os::unix::io::{AsRawFd, RawFd};

pub struct Listener {
    fd:   RawFd,
    addr: SocketAddr,
}

impl Listener {
    /// Bind to `addr` (IPv4 only for now) and start listening with backlog 128.
    /// Socket is created `SOCK_NONBLOCK | SOCK_CLOEXEC`.
    pub fn bind(addr: &str) -> io::Result<Self> {
        let parsed: SocketAddr = addr.parse().map_err(|e| {
            io::Error::new(io::ErrorKind::InvalidInput, format!("bad addr {addr:?}: {e}"))
        })?;
        let SocketAddr::V4(v4) = parsed else {
            return Err(io::Error::new(io::ErrorKind::InvalidInput, "IPv4 only for now"));
        };

        let fd = unsafe {
            libc::socket(
                libc::AF_INET,
                libc::SOCK_STREAM | libc::SOCK_NONBLOCK | libc::SOCK_CLOEXEC,
                0,
            )
        };
        if fd < 0 { return Err(io::Error::last_os_error()); }

        // SO_REUSEADDR so the same port restarts cleanly between `wo run`s.
        let one: libc::c_int = 1;
        let ret = unsafe {
            libc::setsockopt(
                fd, libc::SOL_SOCKET, libc::SO_REUSEADDR,
                &one as *const _ as *const libc::c_void,
                std::mem::size_of::<libc::c_int>() as libc::socklen_t,
            )
        };
        if ret < 0 {
            let err = io::Error::last_os_error();
            unsafe { libc::close(fd); }
            return Err(err);
        }

        let s_addr = u32::from_be_bytes(v4.ip().octets()).to_be();
        let sock = libc::sockaddr_in {
            sin_family: libc::AF_INET as libc::sa_family_t,
            sin_port:   v4.port().to_be(),
            sin_addr:   libc::in_addr { s_addr },
            sin_zero:   [0; 8],
        };
        let ret = unsafe {
            libc::bind(
                fd,
                &sock as *const _ as *const libc::sockaddr,
                std::mem::size_of::<libc::sockaddr_in>() as libc::socklen_t,
            )
        };
        if ret < 0 {
            let err = io::Error::last_os_error();
            unsafe { libc::close(fd); }
            return Err(err);
        }

        if unsafe { libc::listen(fd, 128) } < 0 {
            let err = io::Error::last_os_error();
            unsafe { libc::close(fd); }
            return Err(err);
        }

        // Resolve the actual bound address — caller may have asked for port 0.
        let local = read_local_addr(fd)?;
        Ok(Self { fd, addr: local })
    }

    /// Accept the next pending connection. Returns `None` on `EAGAIN`.
    pub fn accept(&self) -> io::Result<Option<RawFd>> {
        let cfd = unsafe {
            libc::accept4(
                self.fd,
                std::ptr::null_mut(), std::ptr::null_mut(),
                libc::SOCK_NONBLOCK | libc::SOCK_CLOEXEC,
            )
        };
        if cfd < 0 {
            let err = io::Error::last_os_error();
            // On Linux EAGAIN == EWOULDBLOCK; one branch is enough.
            return match err.raw_os_error() {
                Some(libc::EAGAIN) => Ok(None),
                _ => Err(err),
            };
        }
        Ok(Some(cfd))
    }

    pub fn local_addr(&self) -> SocketAddr { self.addr }
}

impl AsRawFd for Listener {
    fn as_raw_fd(&self) -> RawFd { self.fd }
}

impl Drop for Listener {
    fn drop(&mut self) {
        unsafe { libc::close(self.fd); }
    }
}

fn read_local_addr(fd: RawFd) -> io::Result<SocketAddr> {
    let mut sock: libc::sockaddr_in = unsafe { std::mem::zeroed() };
    let mut len = std::mem::size_of::<libc::sockaddr_in>() as libc::socklen_t;
    let ret = unsafe {
        libc::getsockname(fd, &mut sock as *mut _ as *mut libc::sockaddr, &mut len)
    };
    if ret < 0 { return Err(io::Error::last_os_error()); }
    let ip = u32::from_be(sock.sin_addr.s_addr).to_be_bytes();
    let port = u16::from_be(sock.sin_port);
    Ok(SocketAddr::from(([ip[0], ip[1], ip[2], ip[3]], port)))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn binds_and_accepts_a_client() {
        let listener = Listener::bind("127.0.0.1:0").unwrap();
        let port = listener.local_addr().port();
        assert!(port > 0);
        assert!(listener.accept().unwrap().is_none(), "no clients pending yet");

        let stream = std::net::TcpStream::connect(("127.0.0.1", port)).unwrap();
        // Block briefly until the server-side accept sees it.
        let mut accepted = None;
        for _ in 0..50 {
            if let Some(fd) = listener.accept().unwrap() { accepted = Some(fd); break; }
            std::thread::sleep(std::time::Duration::from_millis(10));
        }
        let cfd = accepted.expect("accept produced a client fd");
        unsafe { libc::close(cfd); }
        drop(stream);
    }
}
