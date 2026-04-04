use std::fs;
use std::io;
use std::os::unix::io::RawFd;
use std::path::Path;

use wo_http::response::Response;

use crate::mime::content_type_for;
use crate::resolve::resolve_path;

/// Serve a static file. Returns a Response with the file contents.
///
/// For small files, reads into memory and returns a normal Response.
/// For zero-copy serving via sendfile, use `send_file_zero_copy` instead.
pub fn send_file(static_dir: &Path, request_path: &str) -> Response {
    let file_path = match resolve_path(static_dir, request_path) {
        Ok(p) => p,
        Err(e) => {
            return match e.kind() {
                io::ErrorKind::NotFound => Response::not_found(),
                io::ErrorKind::PermissionDenied => {
                    Response::new(403, "Forbidden")
                }
                _ => Response::internal_error(&e.to_string()),
            };
        }
    };

    let body = match fs::read(&file_path) {
        Ok(b) => b,
        Err(_) => return Response::internal_error("failed to read file"),
    };

    let ct = content_type_for(file_path.to_str().unwrap_or(""));
    let mut response = Response::ok(body, ct);
    response.header("Cache-Control", "public, max-age=3600");
    response
}

/// Zero-copy file serving using sendfile(2).
///
/// Sends the file directly from the page cache to the socket fd.
/// Returns the number of bytes sent, or an error.
pub fn sendfile_to_fd(socket_fd: RawFd, file_path: &Path) -> io::Result<usize> {
    let file = fs::File::open(file_path)?;
    let file_fd = {
        use std::os::unix::io::AsRawFd;
        file.as_raw_fd()
    };

    let metadata = file.metadata()?;
    let file_size = metadata.len() as usize;

    let mut offset: libc::off_t = 0;
    let mut sent = 0usize;

    while sent < file_size {
        let n = unsafe {
            libc::sendfile(
                socket_fd,
                file_fd,
                &mut offset,
                file_size - sent,
            )
        };
        if n < 0 {
            let err = io::Error::last_os_error();
            if err.raw_os_error() == Some(libc::EAGAIN) {
                break; // partial send, caller should retry on EPOLLOUT
            }
            return Err(err);
        }
        if n == 0 {
            break;
        }
        sent += n as usize;
    }

    Ok(sent)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn serve_existing_file() {
        let tmp = tempfile::tempdir().unwrap();
        fs::write(tmp.path().join("test.css"), "body { color: red; }").unwrap();

        let response = send_file(tmp.path(), "test.css");
        assert_eq!(response.status, 200);
        assert!(String::from_utf8_lossy(&response.body).contains("color: red"));
    }

    #[test]
    fn serve_nonexistent_file() {
        let tmp = tempfile::tempdir().unwrap();
        let response = send_file(tmp.path(), "nope.css");
        assert_eq!(response.status, 404);
    }

    #[test]
    fn serve_traversal_attempt() {
        let tmp = tempfile::tempdir().unwrap();
        let response = send_file(tmp.path(), "../etc/passwd");
        assert_eq!(response.status, 403);
    }
}
