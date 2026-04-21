use std::io;
use std::os::unix::io::RawFd;

use crate::request::{self, ParseResult, Request};
use crate::response::Response;

/// Connection state machine.
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum State {
    /// Accumulating request bytes.
    ReadingRequest,
    /// Writing response bytes.
    WritingResponse,
    /// Initial response sent, fd handed to subscription manager.
    /// Connection stays open for diff payloads.
    Subscribed,
    /// Ready to close.
    Done,
}

/// A single HTTP connection.
pub struct Connection {
    pub fd: RawFd,
    pub state: State,
    read_buf: Vec<u8>,
    write_buf: Vec<u8>,
    write_offset: usize,
    parsed_request: Option<Request>,
}

impl Connection {
    pub fn new(fd: RawFd) -> Self {
        Self {
            fd,
            state: State::ReadingRequest,
            read_buf: Vec::with_capacity(4096),
            write_buf: Vec::new(),
            write_offset: 0,
            parsed_request: None,
        }
    }

    /// Read available data from the socket into the read buffer.
    /// Returns Ok(true) if data was read, Ok(false) if connection closed.
    pub fn read(&mut self) -> io::Result<bool> {
        let mut buf = [0u8; 4096];
        let n = unsafe {
            libc::read(self.fd, buf.as_mut_ptr() as *mut libc::c_void, buf.len())
        };
        if n < 0 {
            let err = io::Error::last_os_error();
            if err.raw_os_error() == Some(libc::EAGAIN) {
                return Ok(true); // no data yet, but connection still open
            }
            return Err(err);
        }
        if n == 0 {
            return Ok(false); // connection closed
        }
        self.read_buf.extend_from_slice(&buf[..n as usize]);
        Ok(true)
    }

    /// Try to parse the request from the read buffer.
    /// Returns Some(Request) if a full request was parsed.
    pub fn try_parse(&mut self) -> Option<Request> {
        if self.parsed_request.is_some() {
            return self.parsed_request.clone();
        }
        match request::parse(&self.read_buf) {
            ParseResult::Complete(req, _consumed) => {
                self.parsed_request = Some(req.clone());
                Some(req)
            }
            ParseResult::Incomplete => None,
            ParseResult::Error(_) => {
                self.state = State::Done;
                None
            }
        }
    }

    /// Set the response to write back to the client.
    pub fn set_response(&mut self, response: &Response) {
        self.write_buf = response.to_bytes();
        self.write_offset = 0;
        self.state = State::WritingResponse;
    }

    /// Write pending response bytes to the socket.
    /// Returns Ok(true) if all bytes written, Ok(false) if partial (need EPOLLOUT).
    pub fn write(&mut self) -> io::Result<bool> {
        let remaining = &self.write_buf[self.write_offset..];
        if remaining.is_empty() {
            return Ok(true);
        }

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
        self.write_offset += n as usize;

        if self.write_offset >= self.write_buf.len() {
            Ok(true) // all written
        } else {
            Ok(false) // partial write
        }
    }

    /// Transition to subscribed state (connection stays open).
    pub fn set_subscribed(&mut self) {
        self.state = State::Subscribed;
    }

    /// Mark connection as done (will be closed).
    pub fn set_done(&mut self) {
        self.state = State::Done;
    }

    /// Take the parsed request out of the connection.
    pub fn take_request(&mut self) -> Option<Request> {
        self.parsed_request.take()
    }
}

impl Drop for Connection {
    fn drop(&mut self) {
        unsafe { libc::close(self.fd) };
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn connection_lifecycle() {
        // Use a socketpair to simulate a connection.
        let mut fds = [0i32; 2];
        assert_eq!(
            unsafe { libc::socketpair(libc::AF_UNIX, libc::SOCK_STREAM | libc::SOCK_NONBLOCK, 0, fds.as_mut_ptr()) },
            0
        );
        let (server_fd, client_fd) = (fds[0], fds[1]);

        // Write a request from the "client" side.
        let request_bytes = b"GET /blog/test HTTP/1.1\r\nHost: localhost\r\n\r\n";
        unsafe {
            libc::write(
                client_fd,
                request_bytes.as_ptr() as *const libc::c_void,
                request_bytes.len(),
            );
        }

        // Create connection on the "server" side.
        // We need to manage the fd manually since Connection will close it on drop.
        let dup_fd = unsafe { libc::dup(server_fd) };
        let mut conn = Connection::new(dup_fd);
        assert_eq!(conn.state, State::ReadingRequest);

        // Read and parse.
        assert!(conn.read().unwrap());
        let req = conn.try_parse().unwrap();
        assert_eq!(req.path, "/blog/test");

        // Set response.
        let response = Response::html("<h1>Test</h1>".into());
        conn.set_response(&response);
        assert_eq!(conn.state, State::WritingResponse);

        // Write response.
        let complete = conn.write().unwrap();
        assert!(complete);

        // Read from client side to verify.
        let mut buf = [0u8; 4096];
        let n = unsafe {
            libc::read(client_fd, buf.as_mut_ptr() as *mut libc::c_void, buf.len())
        };
        assert!(n > 0);
        let response_str = std::str::from_utf8(&buf[..n as usize]).unwrap();
        assert!(response_str.contains("HTTP/1.1 200 OK"));
        assert!(response_str.contains("<h1>Test</h1>"));

        unsafe {
            libc::close(server_fd);
            libc::close(client_fd);
        }
    }
}
