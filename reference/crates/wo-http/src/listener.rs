use std::io;
use std::os::unix::io::RawFd;

/// A non-blocking TCP listener wrapping raw socket syscalls.
pub struct TcpListener {
    fd: RawFd,
}

impl TcpListener {
    /// Bind to the given address (e.g., "0.0.0.0:3000") and start listening.
    pub fn bind(addr: &str) -> io::Result<Self> {
        let (ip, port) = parse_addr(addr)?;

        let fd = unsafe {
            libc::socket(
                libc::AF_INET,
                libc::SOCK_STREAM | libc::SOCK_NONBLOCK | libc::SOCK_CLOEXEC,
                0,
            )
        };
        if fd < 0 {
            return Err(io::Error::last_os_error());
        }

        // SO_REUSEADDR to allow quick restart.
        let optval: libc::c_int = 1;
        let ret = unsafe {
            libc::setsockopt(
                fd,
                libc::SOL_SOCKET,
                libc::SO_REUSEADDR,
                &optval as *const _ as *const libc::c_void,
                std::mem::size_of::<libc::c_int>() as libc::socklen_t,
            )
        };
        if ret < 0 {
            unsafe { libc::close(fd) };
            return Err(io::Error::last_os_error());
        }

        let sockaddr = libc::sockaddr_in {
            sin_family: libc::AF_INET as libc::sa_family_t,
            sin_port: port.to_be(),
            sin_addr: libc::in_addr { s_addr: ip },
            sin_zero: [0; 8],
        };

        let ret = unsafe {
            libc::bind(
                fd,
                &sockaddr as *const _ as *const libc::sockaddr,
                std::mem::size_of::<libc::sockaddr_in>() as libc::socklen_t,
            )
        };
        if ret < 0 {
            unsafe { libc::close(fd) };
            return Err(io::Error::last_os_error());
        }

        let ret = unsafe { libc::listen(fd, 128) };
        if ret < 0 {
            unsafe { libc::close(fd) };
            return Err(io::Error::last_os_error());
        }

        Ok(Self { fd })
    }

    /// Accept a new connection. Returns the client fd or EAGAIN if none pending.
    pub fn accept(&self) -> io::Result<Option<RawFd>> {
        let client_fd = unsafe {
            libc::accept4(
                self.fd,
                std::ptr::null_mut(),
                std::ptr::null_mut(),
                libc::SOCK_NONBLOCK | libc::SOCK_CLOEXEC,
            )
        };
        if client_fd < 0 {
            let err = io::Error::last_os_error();
            if err.raw_os_error() == Some(libc::EAGAIN)
                || err.raw_os_error() == Some(libc::EWOULDBLOCK)
            {
                return Ok(None);
            }
            return Err(err);
        }
        Ok(Some(client_fd))
    }

    /// The listener file descriptor for epoll registration.
    pub fn fd(&self) -> RawFd {
        self.fd
    }

    /// Get the actual bound address (useful when binding to port 0).
    pub fn local_addr(&self) -> io::Result<(String, u16)> {
        let mut addr: libc::sockaddr_in = unsafe { std::mem::zeroed() };
        let mut len = std::mem::size_of::<libc::sockaddr_in>() as libc::socklen_t;
        let ret = unsafe {
            libc::getsockname(
                self.fd,
                &mut addr as *mut _ as *mut libc::sockaddr,
                &mut len,
            )
        };
        if ret < 0 {
            return Err(io::Error::last_os_error());
        }
        let ip = u32::from_be(addr.sin_addr.s_addr);
        let port = u16::from_be(addr.sin_port);
        let ip_str = format!(
            "{}.{}.{}.{}",
            (ip >> 24) & 0xFF,
            (ip >> 16) & 0xFF,
            (ip >> 8) & 0xFF,
            ip & 0xFF,
        );
        Ok((ip_str, port))
    }
}

impl Drop for TcpListener {
    fn drop(&mut self) {
        unsafe { libc::close(self.fd) };
    }
}

fn parse_addr(addr: &str) -> io::Result<(u32, u16)> {
    let parts: Vec<&str> = addr.rsplitn(2, ':').collect();
    if parts.len() != 2 {
        return Err(io::Error::new(io::ErrorKind::InvalidInput, "expected host:port"));
    }
    let port: u16 = parts[0]
        .parse()
        .map_err(|_| io::Error::new(io::ErrorKind::InvalidInput, "invalid port"))?;
    let ip_str = parts[1];

    let ip = if ip_str == "0.0.0.0" {
        0u32
    } else if ip_str == "127.0.0.1" {
        0x7F000001u32.to_be()
    } else {
        let octets: Vec<u8> = ip_str
            .split('.')
            .map(|s| s.parse().unwrap_or(0))
            .collect();
        if octets.len() != 4 {
            return Err(io::Error::new(io::ErrorKind::InvalidInput, "invalid IP"));
        }
        u32::from_ne_bytes([octets[0], octets[1], octets[2], octets[3]])
    };

    Ok((ip, port))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn bind_and_accept() {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let (_, port) = listener.local_addr().unwrap();
        assert!(port > 0);

        // No pending connections → None.
        assert!(listener.accept().unwrap().is_none());

        // Connect a client.
        let client_fd = unsafe {
            libc::socket(libc::AF_INET, libc::SOCK_STREAM, 0)
        };
        assert!(client_fd >= 0);

        let addr = libc::sockaddr_in {
            sin_family: libc::AF_INET as libc::sa_family_t,
            sin_port: port.to_be(),
            sin_addr: libc::in_addr {
                s_addr: 0x7F000001u32.to_be(),
            },
            sin_zero: [0; 8],
        };
        let ret = unsafe {
            libc::connect(
                client_fd,
                &addr as *const _ as *const libc::sockaddr,
                std::mem::size_of::<libc::sockaddr_in>() as libc::socklen_t,
            )
        };
        assert_eq!(ret, 0);

        // Now accept should return a fd.
        let accepted = listener.accept().unwrap();
        assert!(accepted.is_some());

        unsafe {
            libc::close(client_fd);
            libc::close(accepted.unwrap());
        }
    }
}
