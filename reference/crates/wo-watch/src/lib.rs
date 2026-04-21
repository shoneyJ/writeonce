use std::collections::HashMap;
use std::io;
use std::os::unix::io::RawFd;
use std::path::{Path, PathBuf};

/// A content change detected by the watcher.
#[derive(Debug, Clone, PartialEq)]
pub enum ContentChange {
    Created(String),  // sys_title
    Modified(String), // sys_title
    Deleted(String),  // sys_title
}

/// Watches a content directory for article file changes using inotify.
///
/// Expects the directory structure:
/// ```text
/// content_dir/
///   {sys_title}/
///     {sys_title}.json
/// ```
pub struct ContentWatcher {
    inotify_fd: RawFd,
    /// Maps inotify watch descriptor → directory path.
    wd_to_path: HashMap<i32, PathBuf>,
    /// Maps directory path → watch descriptor.
    path_to_wd: HashMap<PathBuf, i32>,
    content_dir: PathBuf,
}

const EVENT_MASK: u32 =
    (libc::IN_CREATE | libc::IN_MODIFY | libc::IN_DELETE | libc::IN_MOVED_TO | libc::IN_MOVED_FROM)
        as u32;

impl ContentWatcher {
    /// Create a new watcher on the given content directory.
    ///
    /// Adds inotify watches on the content dir itself and all subdirectories.
    pub fn new(content_dir: &Path) -> io::Result<Self> {
        let fd = unsafe { libc::inotify_init1(libc::IN_NONBLOCK | libc::IN_CLOEXEC) };
        if fd < 0 {
            return Err(io::Error::last_os_error());
        }

        let mut watcher = Self {
            inotify_fd: fd,
            wd_to_path: HashMap::new(),
            path_to_wd: HashMap::new(),
            content_dir: content_dir.to_path_buf(),
        };

        // Watch the root content directory (for new article dirs).
        watcher.add_watch(content_dir)?;

        // Watch each existing subdirectory.
        if content_dir.exists() {
            for entry in std::fs::read_dir(content_dir)? {
                let entry = entry?;
                if entry.path().is_dir() {
                    watcher.add_watch(&entry.path())?;
                }
            }
        }

        Ok(watcher)
    }

    /// The inotify file descriptor, for registration on an event loop.
    pub fn fd(&self) -> RawFd {
        self.inotify_fd
    }

    /// Read and process pending inotify events.
    ///
    /// Returns a list of content changes. Call this when the event loop
    /// signals that the inotify fd is readable.
    pub fn process_events(&mut self) -> io::Result<Vec<ContentChange>> {
        let mut buf = [0u8; 4096];
        let n = unsafe {
            libc::read(
                self.inotify_fd,
                buf.as_mut_ptr() as *mut libc::c_void,
                buf.len(),
            )
        };

        if n < 0 {
            let err = io::Error::last_os_error();
            if err.raw_os_error() == Some(libc::EAGAIN) {
                return Ok(vec![]);
            }
            return Err(err);
        }

        let mut changes = Vec::new();
        let mut offset = 0usize;

        while offset < n as usize {
            let event = unsafe { &*(buf.as_ptr().add(offset) as *const libc::inotify_event) };
            let name_len = event.len as usize;
            let name = if name_len > 0 {
                let name_ptr = unsafe { buf.as_ptr().add(offset + std::mem::size_of::<libc::inotify_event>()) };
                let name_bytes = unsafe { std::slice::from_raw_parts(name_ptr, name_len) };
                let end = name_bytes.iter().position(|&b| b == 0).unwrap_or(name_len);
                Some(String::from_utf8_lossy(&name_bytes[..end]).to_string())
            } else {
                None
            };

            offset += std::mem::size_of::<libc::inotify_event>() + name_len;

            let Some(name) = name else { continue };
            let mask = event.mask;

            // Event on the root content directory: subdirectory created/deleted.
            if let Some(dir_path) = self.wd_to_path.get(&event.wd) {
                let dir_path = dir_path.clone();

                if dir_path == self.content_dir {
                    let sub_path = self.content_dir.join(&name);
                    if (mask & libc::IN_CREATE as u32) != 0 || (mask & libc::IN_MOVED_TO as u32) != 0 {
                        if sub_path.is_dir() {
                            let _ = self.add_watch(&sub_path);
                            changes.push(ContentChange::Created(name.clone()));
                        }
                    }
                    if (mask & libc::IN_DELETE as u32) != 0 || (mask & libc::IN_MOVED_FROM as u32) != 0 {
                        self.remove_watch(&sub_path);
                        changes.push(ContentChange::Deleted(name));
                    }
                } else {
                    // Event inside an article subdirectory.
                    if name.ends_with(".json") || name.ends_with(".md") {
                        let sys_title = dir_path
                            .file_name()
                            .unwrap()
                            .to_string_lossy()
                            .to_string();

                        if (mask & libc::IN_MODIFY as u32) != 0 {
                            changes.push(ContentChange::Modified(sys_title));
                        } else if (mask & libc::IN_CREATE as u32) != 0
                            || (mask & libc::IN_MOVED_TO as u32) != 0
                        {
                            changes.push(ContentChange::Created(sys_title));
                        } else if (mask & libc::IN_DELETE as u32) != 0
                            || (mask & libc::IN_MOVED_FROM as u32) != 0
                        {
                            changes.push(ContentChange::Deleted(sys_title));
                        }
                    }
                }
            }
        }

        // Deduplicate: keep only the last change per sys_title.
        let mut seen = HashMap::new();
        for change in changes {
            let key = match &change {
                ContentChange::Created(t) | ContentChange::Modified(t) | ContentChange::Deleted(t) => {
                    t.clone()
                }
            };
            seen.insert(key, change);
        }

        Ok(seen.into_values().collect())
    }

    fn add_watch(&mut self, path: &Path) -> io::Result<()> {
        let c_path = std::ffi::CString::new(path.to_str().unwrap_or("")).map_err(|_| {
            io::Error::new(io::ErrorKind::InvalidInput, "invalid path for inotify")
        })?;

        let wd = unsafe { libc::inotify_add_watch(self.inotify_fd, c_path.as_ptr(), EVENT_MASK) };
        if wd < 0 {
            return Err(io::Error::last_os_error());
        }

        self.wd_to_path.insert(wd, path.to_path_buf());
        self.path_to_wd.insert(path.to_path_buf(), wd);
        Ok(())
    }

    fn remove_watch(&mut self, path: &Path) {
        if let Some(wd) = self.path_to_wd.remove(path) {
            unsafe { libc::inotify_rm_watch(self.inotify_fd, wd) };
            self.wd_to_path.remove(&wd);
        }
    }
}

impl Drop for ContentWatcher {
    fn drop(&mut self) {
        unsafe { libc::close(self.inotify_fd) };
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;

    #[test]
    fn detect_json_modification() {
        let tmp = tempfile::tempdir().unwrap();
        let content = tmp.path().join("content");
        let art_dir = content.join("my-article");
        fs::create_dir_all(&art_dir).unwrap();
        fs::write(art_dir.join("my-article.json"), r#"{"test": true}"#).unwrap();

        let mut watcher = ContentWatcher::new(&content).unwrap();

        // Modify the file.
        fs::write(art_dir.join("my-article.json"), r#"{"test": false}"#).unwrap();

        // Give inotify a moment.
        std::thread::sleep(std::time::Duration::from_millis(50));

        let changes = watcher.process_events().unwrap();
        assert!(!changes.is_empty());
        assert!(changes.iter().any(|c| matches!(c, ContentChange::Modified(t) if t == "my-article")));
    }

    #[test]
    fn detect_new_directory() {
        let tmp = tempfile::tempdir().unwrap();
        let content = tmp.path().join("content");
        fs::create_dir_all(&content).unwrap();

        let mut watcher = ContentWatcher::new(&content).unwrap();

        // Create a new article directory.
        let art_dir = content.join("new-article");
        fs::create_dir_all(&art_dir).unwrap();

        std::thread::sleep(std::time::Duration::from_millis(50));

        let changes = watcher.process_events().unwrap();
        assert!(changes.iter().any(|c| matches!(c, ContentChange::Created(t) if t == "new-article")));
    }

    #[test]
    fn detect_json_delete() {
        let tmp = tempfile::tempdir().unwrap();
        let content = tmp.path().join("content");
        let art_dir = content.join("delete-me");
        fs::create_dir_all(&art_dir).unwrap();
        fs::write(art_dir.join("delete-me.json"), "{}").unwrap();

        let mut watcher = ContentWatcher::new(&content).unwrap();

        // Delete the JSON file.
        fs::remove_file(art_dir.join("delete-me.json")).unwrap();

        std::thread::sleep(std::time::Duration::from_millis(50));

        let changes = watcher.process_events().unwrap();
        assert!(changes.iter().any(|c| matches!(c, ContentChange::Deleted(t) if t == "delete-me")));
    }

    #[test]
    fn ignore_non_json_files() {
        let tmp = tempfile::tempdir().unwrap();
        let content = tmp.path().join("content");
        let art_dir = content.join("my-article");
        fs::create_dir_all(&art_dir).unwrap();

        let mut watcher = ContentWatcher::new(&content).unwrap();

        // Create a non-JSON file.
        fs::write(art_dir.join("notes.txt"), "not json").unwrap();

        std::thread::sleep(std::time::Duration::from_millis(50));

        let changes = watcher.process_events().unwrap();
        // Non-JSON files should not trigger Modified events.
        assert!(changes.iter().all(|c| !matches!(c, ContentChange::Modified(_))));
    }
}
