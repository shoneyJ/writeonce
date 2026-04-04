use std::collections::HashMap;
use std::io;
use std::os::unix::io::RawFd;

use serde::{Deserialize, Serialize};
use wo_event::EventFd;

/// A subscription query pattern.
#[derive(Debug, Clone, PartialEq)]
pub enum Subscription {
    /// Subscribe to changes for a single article by sys_title.
    ByTitle(String),
    /// Subscribe to changes for all articles with a given tag.
    ByTag(String),
    /// Subscribe to all content changes.
    All,
}

/// The type of content change.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub enum ChangeType {
    Created,
    Updated,
    Deleted,
}

/// Notification payload written to subscriber fds.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct Notification {
    pub change_type: ChangeType,
    pub sys_title: String,
    pub version: u64,
}

/// Wire format for notification payloads.
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum FdFormat {
    /// Length-prefixed bincode (for internal subscribers).
    Binary,
    /// Length-prefixed JSON (for browser socket fds).
    Json,
}

/// Manages subscriptions and delivers notifications to subscriber fds.
///
/// Subscribers register interest via `subscribe()` with a query pattern.
/// When content changes, `notify()` evaluates which subscriptions match
/// and writes a length-prefixed notification to each matching fd.
pub struct SubscriptionManager {
    /// sys_title → list of subscriber fds.
    by_title: HashMap<String, Vec<RawFd>>,
    /// tag → list of subscriber fds.
    by_tag: HashMap<String, Vec<RawFd>>,
    /// Fds subscribed to all changes.
    global: Vec<RawFd>,
    /// fd → list of subscriptions (for cleanup on disconnect).
    fd_registry: HashMap<RawFd, Vec<Subscription>>,
    /// fd → wire format.
    fd_format: HashMap<RawFd, FdFormat>,
    /// EventFd for signaling the event loop that notifications are pending.
    notify_efd: EventFd,
}

impl SubscriptionManager {
    /// Create a new subscription manager.
    pub fn new() -> io::Result<Self> {
        Ok(Self {
            by_title: HashMap::new(),
            by_tag: HashMap::new(),
            global: Vec::new(),
            fd_registry: HashMap::new(),
            fd_format: HashMap::new(),
            notify_efd: EventFd::new()?,
        })
    }

    /// The eventfd for signaling the event loop.
    pub fn notify_fd(&self) -> RawFd {
        self.notify_efd.fd()
    }

    /// Register a subscription for the given fd (binary format).
    pub fn subscribe(&mut self, fd: RawFd, query: Subscription) {
        self.subscribe_with_format(fd, query, FdFormat::Binary);
    }

    /// Register a subscription for the given fd (JSON format for browsers).
    pub fn subscribe_json(&mut self, fd: RawFd, query: Subscription) {
        self.subscribe_with_format(fd, query, FdFormat::Json);
    }

    fn subscribe_with_format(&mut self, fd: RawFd, query: Subscription, format: FdFormat) {
        match &query {
            Subscription::ByTitle(title) => {
                self.by_title.entry(title.clone()).or_default().push(fd);
            }
            Subscription::ByTag(tag) => {
                self.by_tag.entry(tag.clone()).or_default().push(fd);
            }
            Subscription::All => {
                self.global.push(fd);
            }
        }
        self.fd_registry.entry(fd).or_default().push(query);
        self.fd_format.insert(fd, format);
    }

    /// Remove all subscriptions for a fd (e.g., on EPOLLHUP).
    pub fn unsubscribe(&mut self, fd: RawFd) {
        self.fd_format.remove(&fd);
        if let Some(subs) = self.fd_registry.remove(&fd) {
            for sub in subs {
                match sub {
                    Subscription::ByTitle(title) => {
                        if let Some(fds) = self.by_title.get_mut(&title) {
                            fds.retain(|&f| f != fd);
                        }
                    }
                    Subscription::ByTag(tag) => {
                        if let Some(fds) = self.by_tag.get_mut(&tag) {
                            fds.retain(|&f| f != fd);
                        }
                    }
                    Subscription::All => {
                        self.global.retain(|&f| f != fd);
                    }
                }
            }
        }
    }

    /// Notify subscribers about a content change.
    ///
    /// Evaluates which subscriptions match the changed sys_title and tags,
    /// then writes a length-prefixed notification to each matching fd.
    pub fn notify(
        &self,
        sys_title: &str,
        tags: &[String],
        change_type: ChangeType,
        version: u64,
    ) -> io::Result<usize> {
        let notification = Notification {
            change_type,
            sys_title: sys_title.to_string(),
            version,
        };

        // Pre-serialize both formats (only actually used if there are subscribers).
        let bincode_payload = bincode::serialize(&notification).map_err(|e| {
            io::Error::new(io::ErrorKind::InvalidData, format!("bincode: {}", e))
        })?;
        let json_payload = serde_json::to_vec(&notification).map_err(|e| {
            io::Error::new(io::ErrorKind::InvalidData, format!("json: {}", e))
        })?;

        // Build wire formats: [u32 length][payload]
        let bincode_wire = build_wire(&bincode_payload);
        let json_wire = build_wire(&json_payload);

        // Collect all fds that should receive this notification.
        let mut target_fds: Vec<RawFd> = Vec::new();

        // Title subscribers.
        if let Some(fds) = self.by_title.get(sys_title) {
            target_fds.extend(fds);
        }

        // Tag subscribers.
        for tag in tags {
            if let Some(fds) = self.by_tag.get(tag) {
                target_fds.extend(fds);
            }
        }

        // Global subscribers.
        target_fds.extend(&self.global);

        // Deduplicate.
        target_fds.sort_unstable();
        target_fds.dedup();

        let mut sent = 0;
        for &fd in &target_fds {
            let format = self.fd_format.get(&fd).copied().unwrap_or(FdFormat::Binary);
            let wire = match format {
                FdFormat::Binary => &bincode_wire,
                FdFormat::Json => &json_wire,
            };
            if write_all_fd(fd, wire).is_ok() {
                sent += 1;
            }
        }

        // Signal the event loop.
        if sent > 0 {
            let _ = self.notify_efd.write(1);
        }

        Ok(sent)
    }

    /// Number of active subscriber fds.
    pub fn subscriber_count(&self) -> usize {
        self.fd_registry.len()
    }
}

fn build_wire(payload: &[u8]) -> Vec<u8> {
    let len = payload.len() as u32;
    let mut wire = Vec::with_capacity(4 + payload.len());
    wire.extend_from_slice(&len.to_le_bytes());
    wire.extend_from_slice(payload);
    wire
}

/// Write all bytes to a raw fd.
fn write_all_fd(fd: RawFd, buf: &[u8]) -> io::Result<()> {
    let mut written = 0;
    while written < buf.len() {
        let ret = unsafe {
            libc::write(
                fd,
                buf[written..].as_ptr() as *const libc::c_void,
                buf.len() - written,
            )
        };
        if ret < 0 {
            return Err(io::Error::last_os_error());
        }
        if ret == 0 {
            return Err(io::Error::new(io::ErrorKind::WriteZero, "write returned 0"));
        }
        written += ret as usize;
    }
    Ok(())
}

/// Convenience macro for compile-time subscription wiring.
///
/// ```ignore
/// register!(sub_manager, fd, ByTitle("linux-misc"));
/// register!(sub_manager, fd, ByTag("rust"));
/// register!(sub_manager, fd, All);
/// ```
#[macro_export]
macro_rules! register {
    ($mgr:expr, $fd:expr, ByTitle($title:expr)) => {
        $mgr.subscribe($fd, $crate::Subscription::ByTitle($title.into()))
    };
    ($mgr:expr, $fd:expr, ByTag($tag:expr)) => {
        $mgr.subscribe($fd, $crate::Subscription::ByTag($tag.into()))
    };
    ($mgr:expr, $fd:expr, All) => {
        $mgr.subscribe($fd, $crate::Subscription::All)
    };
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Create a pipe and return (read_fd, write_fd).
    fn make_pipe() -> (RawFd, RawFd) {
        let mut fds = [0i32; 2];
        assert_eq!(unsafe { libc::pipe2(fds.as_mut_ptr(), libc::O_NONBLOCK) }, 0);
        (fds[0], fds[1])
    }

    fn read_notification(fd: RawFd) -> Notification {
        let mut len_buf = [0u8; 4];
        let ret = unsafe { libc::read(fd, len_buf.as_mut_ptr() as *mut libc::c_void, 4) };
        assert!(ret == 4, "failed to read length prefix");
        let len = u32::from_le_bytes(len_buf) as usize;

        let mut payload = vec![0u8; len];
        let ret = unsafe { libc::read(fd, payload.as_mut_ptr() as *mut libc::c_void, len) };
        assert_eq!(ret as usize, len);

        bincode::deserialize(&payload).unwrap()
    }

    fn close_fd(fd: RawFd) {
        unsafe { libc::close(fd) };
    }

    #[test]
    fn title_subscription() {
        let mut mgr = SubscriptionManager::new().unwrap();

        let (r1, w1) = make_pipe();
        let (r2, w2) = make_pipe();

        mgr.subscribe(w1, Subscription::ByTitle("art-one".into()));
        mgr.subscribe(w2, Subscription::ByTitle("art-one".into()));

        let sent = mgr
            .notify("art-one", &[], ChangeType::Updated, 1)
            .unwrap();
        assert_eq!(sent, 2);

        let n1 = read_notification(r1);
        assert_eq!(n1.sys_title, "art-one");
        assert_eq!(n1.change_type, ChangeType::Updated);
        assert_eq!(n1.version, 1);

        let n2 = read_notification(r2);
        assert_eq!(n2.sys_title, "art-one");

        // Unrelated title: no notifications.
        let sent = mgr
            .notify("art-two", &[], ChangeType::Created, 2)
            .unwrap();
        assert_eq!(sent, 0);

        close_fd(r1); close_fd(w1);
        close_fd(r2); close_fd(w2);
    }

    #[test]
    fn tag_subscription() {
        let mut mgr = SubscriptionManager::new().unwrap();

        let (r, w) = make_pipe();
        mgr.subscribe(w, Subscription::ByTag("rust".into()));

        let sent = mgr
            .notify("art-one", &["rust".into(), "linux".into()], ChangeType::Updated, 1)
            .unwrap();
        assert_eq!(sent, 1);

        let n = read_notification(r);
        assert_eq!(n.sys_title, "art-one");

        // No "rust" tag: no notification.
        let sent = mgr
            .notify("art-two", &["python".into()], ChangeType::Created, 2)
            .unwrap();
        assert_eq!(sent, 0);

        close_fd(r); close_fd(w);
    }

    #[test]
    fn global_subscription() {
        let mut mgr = SubscriptionManager::new().unwrap();

        let (r, w) = make_pipe();
        mgr.subscribe(w, Subscription::All);

        let sent = mgr.notify("anything", &[], ChangeType::Deleted, 5).unwrap();
        assert_eq!(sent, 1);

        let n = read_notification(r);
        assert_eq!(n.change_type, ChangeType::Deleted);

        close_fd(r); close_fd(w);
    }

    #[test]
    fn unsubscribe_removes_fd() {
        let mut mgr = SubscriptionManager::new().unwrap();

        let (r, w) = make_pipe();
        mgr.subscribe(w, Subscription::ByTitle("art-one".into()));
        mgr.subscribe(w, Subscription::All);
        assert_eq!(mgr.subscriber_count(), 1);

        mgr.unsubscribe(w);
        assert_eq!(mgr.subscriber_count(), 0);

        let sent = mgr.notify("art-one", &[], ChangeType::Updated, 1).unwrap();
        assert_eq!(sent, 0);

        close_fd(r); close_fd(w);
    }

    #[test]
    fn dedup_notifications() {
        let mut mgr = SubscriptionManager::new().unwrap();

        let (r, w) = make_pipe();
        // Same fd subscribed to both title AND tag — should only get one notification.
        mgr.subscribe(w, Subscription::ByTitle("art-one".into()));
        mgr.subscribe(w, Subscription::ByTag("rust".into()));

        let sent = mgr
            .notify("art-one", &["rust".into()], ChangeType::Updated, 1)
            .unwrap();
        assert_eq!(sent, 1); // deduped

        let n = read_notification(r);
        assert_eq!(n.sys_title, "art-one");

        close_fd(r); close_fd(w);
    }

    #[test]
    fn json_format_subscription() {
        let mut mgr = SubscriptionManager::new().unwrap();

        let (r, w) = make_pipe();
        mgr.subscribe_json(w, Subscription::ByTitle("art-one".into()));

        mgr.notify("art-one", &[], ChangeType::Updated, 1).unwrap();

        // Read length-prefixed JSON.
        let mut len_buf = [0u8; 4];
        let ret = unsafe { libc::read(r, len_buf.as_mut_ptr() as *mut libc::c_void, 4) };
        assert_eq!(ret, 4);
        let len = u32::from_le_bytes(len_buf) as usize;

        let mut payload = vec![0u8; len];
        let ret = unsafe { libc::read(r, payload.as_mut_ptr() as *mut libc::c_void, len) };
        assert_eq!(ret as usize, len);

        // Should be valid JSON.
        let json: serde_json::Value = serde_json::from_slice(&payload).unwrap();
        assert_eq!(json["sys_title"], "art-one");
        assert_eq!(json["change_type"], "Updated");
        assert_eq!(json["version"], 1);

        close_fd(r); close_fd(w);
    }

    #[test]
    fn mixed_format_subscribers() {
        let mut mgr = SubscriptionManager::new().unwrap();

        let (r_bin, w_bin) = make_pipe();
        let (r_json, w_json) = make_pipe();

        mgr.subscribe(w_bin, Subscription::ByTitle("art-one".into()));
        mgr.subscribe_json(w_json, Subscription::ByTitle("art-one".into()));

        let sent = mgr.notify("art-one", &[], ChangeType::Created, 5).unwrap();
        assert_eq!(sent, 2);

        // Binary subscriber gets bincode.
        let n = read_notification(r_bin);
        assert_eq!(n.sys_title, "art-one");
        assert_eq!(n.version, 5);

        // JSON subscriber gets JSON.
        let mut len_buf = [0u8; 4];
        unsafe { libc::read(r_json, len_buf.as_mut_ptr() as *mut libc::c_void, 4) };
        let len = u32::from_le_bytes(len_buf) as usize;
        let mut payload = vec![0u8; len];
        unsafe { libc::read(r_json, payload.as_mut_ptr() as *mut libc::c_void, len) };
        let json: serde_json::Value = serde_json::from_slice(&payload).unwrap();
        assert_eq!(json["sys_title"], "art-one");

        close_fd(r_bin); close_fd(w_bin);
        close_fd(r_json); close_fd(w_json);
    }

    #[test]
    fn register_macro() {
        let mut mgr = SubscriptionManager::new().unwrap();
        let (r, w) = make_pipe();

        register!(mgr, w, ByTitle("test-article"));
        register!(mgr, w, ByTag("rust"));
        register!(mgr, w, All);

        assert_eq!(mgr.subscriber_count(), 1);

        close_fd(r); close_fd(w);
    }
}
