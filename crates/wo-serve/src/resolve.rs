use std::io;
use std::path::{Path, PathBuf};

/// Resolve a request path relative to a static directory.
///
/// Returns the absolute path to the file, or an error if:
/// - The path contains `..` (directory traversal)
/// - The resolved path escapes the base directory
/// - The file doesn't exist
pub fn resolve_path(base: &Path, request_path: &str) -> io::Result<PathBuf> {
    // Reject any path with `..`
    if request_path.contains("..") {
        return Err(io::Error::new(
            io::ErrorKind::PermissionDenied,
            "path traversal rejected",
        ));
    }

    let clean = request_path.trim_start_matches('/');
    let resolved = base.join(clean);

    // Canonicalize and verify it's still under base.
    let canonical = resolved.canonicalize().map_err(|_| {
        io::Error::new(io::ErrorKind::NotFound, "file not found")
    })?;

    let canonical_base = base.canonicalize().map_err(|_| {
        io::Error::new(io::ErrorKind::NotFound, "base directory not found")
    })?;

    if !canonical.starts_with(&canonical_base) {
        return Err(io::Error::new(
            io::ErrorKind::PermissionDenied,
            "path traversal rejected",
        ));
    }

    if !canonical.is_file() {
        return Err(io::Error::new(io::ErrorKind::NotFound, "not a file"));
    }

    Ok(canonical)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;

    #[test]
    fn valid_path() {
        let tmp = tempfile::tempdir().unwrap();
        fs::write(tmp.path().join("test.css"), "body {}").unwrap();

        let path = resolve_path(tmp.path(), "test.css").unwrap();
        assert!(path.ends_with("test.css"));
    }

    #[test]
    fn nested_path() {
        let tmp = tempfile::tempdir().unwrap();
        fs::create_dir_all(tmp.path().join("styles")).unwrap();
        fs::write(tmp.path().join("styles/main.css"), "body {}").unwrap();

        let path = resolve_path(tmp.path(), "styles/main.css").unwrap();
        assert!(path.ends_with("styles/main.css"));
    }

    #[test]
    fn reject_traversal() {
        let tmp = tempfile::tempdir().unwrap();
        assert!(resolve_path(tmp.path(), "../etc/passwd").is_err());
    }

    #[test]
    fn reject_nonexistent() {
        let tmp = tempfile::tempdir().unwrap();
        assert!(resolve_path(tmp.path(), "nonexistent.css").is_err());
    }
}
