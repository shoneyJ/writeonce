//! Per-connection state machine driven by the phase-02 [`EventLoop`].
//!
//! Lifecycle (close-after-response, no keep-alive yet):
//!   Reading → drain `read(2)` to `EAGAIN`, parse, dispatch through the
//!     `Router`, queue the response.
//!   Writing → drain `write(2)` to `EAGAIN`. If a write was partial, the
//!     loop re-arms the fd as `WRITABLE` and we continue on the next event.
//!   Done    → loop closes the fd.
//!
//! Adapted from `reference/crates/wo-http/src/connection.rs`. The owning
//! [`EventLoop`] supplies `read`/`write` readiness via edge-triggered
//! `epoll`; this struct is the per-fd part of the state.
//!
//! [`EventLoop`]: crate::runtime::EventLoop

use std::io;
use std::os::unix::io::{AsRawFd, RawFd};

use super::request::{self, ParseResult};
use super::response::Response;
use super::route::Router;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ConnState {
    Reading,
    Writing,
    Done,
}

pub struct Connection {
    fd:           RawFd,
    state:        ConnState,
    read_buf:     Vec<u8>,
    write_buf:    Vec<u8>,
    write_offset: usize,
}

impl Connection {
    pub fn new(fd: RawFd) -> Self {
        Self {
            fd,
            state: ConnState::Reading,
            read_buf: Vec::with_capacity(4096),
            write_buf: Vec::new(),
            write_offset: 0,
        }
    }

    pub fn state(&self) -> ConnState { self.state }
    pub fn is_done(&self) -> bool { self.state == ConnState::Done }

    /// Drain the socket into `read_buf` until `EAGAIN` or EOF.
    /// Returns `false` when the peer closed (connection should be torn down).
    fn drain_read(&mut self) -> io::Result<bool> {
        let mut tmp = [0u8; 4096];
        loop {
            let n = unsafe {
                libc::read(self.fd, tmp.as_mut_ptr() as *mut libc::c_void, tmp.len())
            };
            if n < 0 {
                let err = io::Error::last_os_error();
                if err.raw_os_error() == Some(libc::EAGAIN) {
                    return Ok(true);
                }
                return Err(err);
            }
            if n == 0 {
                return Ok(false);
            }
            self.read_buf.extend_from_slice(&tmp[..n as usize]);
            // Cap at MAX_BODY_BYTES + headers — refuse pathological requests.
            if self.read_buf.len() > 32 * 1024 * 1024 {
                return Err(io::Error::new(io::ErrorKind::InvalidData, "request too large"));
            }
        }
    }

    /// Drain `write_buf[write_offset..]` to the socket until `EAGAIN`.
    /// Returns `true` once everything has been flushed.
    fn drain_write(&mut self) -> io::Result<bool> {
        loop {
            let remaining = &self.write_buf[self.write_offset..];
            if remaining.is_empty() { return Ok(true); }
            let n = unsafe {
                libc::write(
                    self.fd,
                    remaining.as_ptr() as *const libc::c_void,
                    remaining.len(),
                )
            };
            if n < 0 {
                let err = io::Error::last_os_error();
                if err.raw_os_error() == Some(libc::EAGAIN) {
                    return Ok(false);
                }
                return Err(err);
            }
            if n == 0 { return Ok(false); }
            self.write_offset += n as usize;
        }
    }

    fn try_parse(&self) -> ParseResult {
        request::parse(&self.read_buf)
    }

    fn queue_response(&mut self, response: &Response) {
        self.write_buf    = response.to_bytes();
        self.write_offset = 0;
        self.state        = ConnState::Writing;
    }

    /// One step of the state machine, given a readiness event from the
    /// loop. Returns `true` if the connection now wants `WRITABLE` (the
    /// caller should switch interest from `READABLE`); `false` otherwise.
    pub fn drive(
        &mut self,
        readable: bool,
        writable: bool,
        hangup:   bool,
        error:    bool,
        router:   &Router,
    ) -> io::Result<bool> {
        if error {
            self.state = ConnState::Done;
            return Ok(false);
        }

        if readable && self.state == ConnState::Reading {
            let still_open = self.drain_read()?;
            match self.try_parse() {
                ParseResult::Complete(req) => {
                    let resp = router.dispatch(&req);
                    self.queue_response(&resp);
                }
                ParseResult::Incomplete => {
                    if !still_open {
                        self.state = ConnState::Done;
                        return Ok(false);
                    }
                }
                ParseResult::Error(msg) => {
                    let resp = Response::status(super::Status::BAD_REQUEST).text(msg);
                    self.queue_response(&resp);
                }
            }
        }

        if self.state == ConnState::Writing {
            let flushed = self.drain_write()?;
            if flushed {
                self.state = ConnState::Done;
                return Ok(false);
            } else if !writable {
                // We tried, EAGAIN'd; tell the caller to wait for WRITABLE.
                return Ok(true);
            }
        }

        if hangup && self.state != ConnState::Writing {
            self.state = ConnState::Done;
        }

        Ok(false)
    }
}

impl AsRawFd for Connection {
    fn as_raw_fd(&self) -> RawFd { self.fd }
}

impl Drop for Connection {
    fn drop(&mut self) {
        unsafe { libc::close(self.fd); }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::http::{Method, Response, Router};

    fn socketpair_nonblock() -> (RawFd, RawFd) {
        let mut fds = [0i32; 2];
        let r = unsafe {
            libc::socketpair(
                libc::AF_UNIX,
                libc::SOCK_STREAM | libc::SOCK_NONBLOCK | libc::SOCK_CLOEXEC,
                0,
                fds.as_mut_ptr(),
            )
        };
        assert_eq!(r, 0);
        (fds[0], fds[1])
    }

    #[test]
    fn connection_handles_a_request() {
        let (server_fd, client_fd) = socketpair_nonblock();

        let req = b"GET /healthz HTTP/1.1\r\nHost: localhost\r\n\r\n";
        let n = unsafe {
            libc::write(client_fd, req.as_ptr() as *const _, req.len())
        };
        assert_eq!(n, req.len() as isize);

        let router = Router::new()
            .route(Method::Get, "/healthz", |_, _| Response::ok().text("ok"));

        let mut conn = Connection::new(server_fd);
        let want_writable = conn.drive(true, false, false, false, &router).unwrap();
        assert!(!want_writable, "small response fits in one write");
        assert!(conn.is_done());

        let mut buf = [0u8; 4096];
        let n = unsafe {
            libc::read(client_fd, buf.as_mut_ptr() as *mut _, buf.len())
        };
        assert!(n > 0);
        let s = std::str::from_utf8(&buf[..n as usize]).unwrap();
        assert!(s.starts_with("HTTP/1.1 200 OK\r\n"), "got: {s}");
        assert!(s.ends_with("\r\n\r\nok"));

        unsafe { libc::close(client_fd); }
        // server_fd is closed by Connection::drop.
    }

    #[test]
    fn returns_404_for_unknown_path() {
        let (server_fd, client_fd) = socketpair_nonblock();

        let req = b"GET /missing HTTP/1.1\r\n\r\n";
        unsafe { libc::write(client_fd, req.as_ptr() as *const _, req.len()); }

        let router = Router::new()
            .route(Method::Get, "/healthz", |_, _| Response::ok().text("ok"));
        let mut conn = Connection::new(server_fd);
        conn.drive(true, false, false, false, &router).unwrap();

        let mut buf = [0u8; 4096];
        let n = unsafe { libc::read(client_fd, buf.as_mut_ptr() as *mut _, buf.len()) };
        let s = std::str::from_utf8(&buf[..n as usize]).unwrap();
        assert!(s.starts_with("HTTP/1.1 404 Not Found\r\n"), "got: {s}");

        unsafe { libc::close(client_fd); }
    }
}
