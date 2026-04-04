use std::collections::HashMap;
use std::io;
use std::os::unix::io::RawFd;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::Duration;

use wo_event::{EventLoop, Interest, SignalFd, TimerFd, Token};
use wo_htmlx::TemplateRegistry;
use wo_http::connection::{Connection, State};
use wo_http::listener::TcpListener;
use wo_http::request::Method;
use wo_http::response::Response;
use wo_route::{RouteParams, Router};
use wo_store::Store;
use wo_sub::{ChangeType, Subscription, SubscriptionManager};
use wo_watch::{ContentChange, ContentWatcher};

use crate::handlers;
use crate::Config;

// Well-known tokens.
const TOKEN_WATCHER: Token = 1;
const TOKEN_SIGNAL: Token = 2;
const TOKEN_TIMER: Token = 3;
const TOKEN_NOTIFY: Token = 4;
const TOKEN_HTTP_LISTENER: Token = 5;
// Token ranges.
const TOKEN_HTTP_BASE: Token = 10000;
const TOKEN_HTTP_MAX: Token = 19999;
const TOKEN_SUB_BASE: Token = 20000;

/// The writeonce runtime: single process, single event loop, all fds on one epoll.
pub struct Runtime {
    event_loop: EventLoop,
    store: Store,
    watcher: ContentWatcher,
    subscriptions: SubscriptionManager,
    signal_fd: SignalFd,
    timer_fd: TimerFd,
    running: Arc<AtomicBool>,
    // HTTP
    listener: Option<TcpListener>,
    connections: HashMap<Token, Connection>,
    next_http_token: Token,
    router: Router,
    templates: TemplateRegistry,
    static_dir: std::path::PathBuf,
    // Subscribed browser connections.
    next_sub_token: Token,
    sub_token_to_fd: HashMap<Token, RawFd>,
}

/// Handle returned by `Runtime::start()` for external interaction.
pub struct RuntimeHandle {
    running: Arc<AtomicBool>,
}

impl RuntimeHandle {
    pub fn shutdown(&self) {
        self.running.store(false, Ordering::SeqCst);
    }
}

impl Runtime {
    pub fn new(config: &Config) -> io::Result<Self> {
        let event_loop = EventLoop::new()?;

        let store = if config.rebuild_on_start {
            let mut s = Store::open(&config.content_dir, &config.data_dir)?;
            s.rebuild()?;
            s
        } else {
            Store::open(&config.content_dir, &config.data_dir)?
        };

        let watcher = ContentWatcher::new(&config.content_dir)?;
        let subscriptions = SubscriptionManager::new()?;
        let signal_fd = SignalFd::new()?;
        let timer_fd = TimerFd::new()?;
        timer_fd.set(Duration::from_secs(60), Duration::from_secs(60))?;

        // Register system fds.
        event_loop.register(watcher.fd(), Interest::Readable, TOKEN_WATCHER)?;
        event_loop.register(signal_fd.fd(), Interest::Readable, TOKEN_SIGNAL)?;
        event_loop.register(timer_fd.fd(), Interest::Readable, TOKEN_TIMER)?;
        event_loop.register(subscriptions.notify_fd(), Interest::Readable, TOKEN_NOTIFY)?;

        // HTTP listener (optional — may not bind in test mode).
        let listener = if !config.bind_addr.is_empty() {
            match TcpListener::bind(&config.bind_addr) {
                Ok(l) => {
                    event_loop.register(l.fd(), Interest::Readable, TOKEN_HTTP_LISTENER)?;
                    Some(l)
                }
                Err(e) => {
                    eprintln!("wo-rt: failed to bind {}: {}", config.bind_addr, e);
                    None
                }
            }
        } else {
            None
        };

        // Load templates.
        let templates = TemplateRegistry::load(&config.templates_dir)?;

        // Build router.
        let mut router = Router::new();
        router.add(Method::Get, "/", "home");
        router.add(Method::Get, "/blog/:sys_title", "article");
        router.add(Method::Get, "/about", "about");
        router.add(Method::Get, "/contact", "contact");
        router.add(Method::Get, "/tag/:tag", "tag_listing");
        router.add(Method::Get, "/static/*path", "static_file");

        Ok(Self {
            event_loop,
            store,
            watcher,
            subscriptions,
            signal_fd,
            timer_fd,
            running: Arc::new(AtomicBool::new(false)),
            listener,
            connections: HashMap::new(),
            next_http_token: TOKEN_HTTP_BASE,
            router,
            templates,
            static_dir: config.static_dir.clone(),
            next_sub_token: TOKEN_SUB_BASE,
            sub_token_to_fd: HashMap::new(),
        })
    }

    pub fn run(&mut self) -> io::Result<()> {
        self.running.store(true, Ordering::SeqCst);

        if let Some(ref listener) = self.listener {
            let (_, port) = listener.local_addr()?;
            eprintln!("writeonce listening on port {}", port);
        }

        while self.running.load(Ordering::SeqCst) {
            let events = self.event_loop.poll(Some(Duration::from_millis(500)))?;

            for event in events {
                match event.token {
                    TOKEN_WATCHER if event.readable => {
                        self.handle_watcher_event()?;
                    }
                    TOKEN_SIGNAL if event.readable => {
                        let _ = self.signal_fd.read();
                        self.running.store(false, Ordering::SeqCst);
                    }
                    TOKEN_TIMER if event.readable => {
                        let _ = self.timer_fd.read();
                    }
                    TOKEN_NOTIFY if event.readable => {
                        // Notifications already written by SubscriptionManager.
                    }
                    TOKEN_HTTP_LISTENER if event.readable => {
                        self.handle_accept()?;
                    }
                    token if token >= TOKEN_SUB_BASE => {
                        // Subscribed connection.
                        if event.hangup || event.error {
                            if let Some(&fd) = self.sub_token_to_fd.get(&token) {
                                self.subscriptions.unsubscribe(fd);
                                let _ = self.event_loop.deregister(fd);
                                self.sub_token_to_fd.remove(&token);
                                // Don't close fd here — Connection owns it.
                            }
                        }
                    }
                    token if token >= TOKEN_HTTP_BASE => {
                        self.handle_http_event(token, &event)?;
                    }
                    _ => {}
                }
            }

            // Clean up completed connections.
            let done_tokens: Vec<Token> = self
                .connections
                .iter()
                .filter(|(_, c)| c.state == State::Done)
                .map(|(&t, _)| t)
                .collect();
            for token in done_tokens {
                if let Some(conn) = self.connections.remove(&token) {
                    let _ = self.event_loop.deregister(conn.fd);
                }
            }
        }

        Ok(())
    }

    pub fn handle(&self) -> RuntimeHandle {
        RuntimeHandle {
            running: self.running.clone(),
        }
    }

    pub fn store(&self) -> &Store {
        &self.store
    }

    pub fn store_mut(&mut self) -> &mut Store {
        &mut self.store
    }

    /// Subscribe a fd (binary format, for internal use).
    pub fn subscribe(&mut self, fd: RawFd, query: Subscription) -> io::Result<Token> {
        let token = self.next_sub_token;
        self.next_sub_token += 1;
        self.event_loop.register(fd, Interest::Readable, token)?;
        self.subscriptions.subscribe(fd, query);
        self.sub_token_to_fd.insert(token, fd);
        Ok(token)
    }

    fn handle_accept(&mut self) -> io::Result<()> {
        let listener = match &self.listener {
            Some(l) => l,
            None => return Ok(()),
        };

        // Accept all pending connections.
        while let Some(client_fd) = listener.accept()? {
            if self.next_http_token > TOKEN_HTTP_MAX {
                self.next_http_token = TOKEN_HTTP_BASE; // recycle tokens
            }
            let token = self.next_http_token;
            self.next_http_token += 1;

            self.event_loop
                .register(client_fd, Interest::Readable, token)?;
            self.connections.insert(token, Connection::new(client_fd));
        }

        Ok(())
    }

    fn handle_http_event(&mut self, token: Token, event: &wo_event::Event) -> io::Result<()> {
        if event.hangup || event.error {
            if let Some(conn) = self.connections.remove(&token) {
                let _ = self.event_loop.deregister(conn.fd);
            }
            return Ok(());
        }

        // Read phase: parse the request without holding mutable borrow during dispatch.
        let conn = match self.connections.get_mut(&token) {
            Some(c) => c,
            None => return Ok(()),
        };

        match conn.state {
            State::ReadingRequest if event.readable => {
                let open = conn.read()?;
                if !open {
                    conn.set_done();
                    return Ok(());
                }

                if let Some(request) = conn.try_parse() {
                    // Dispatch the handler (borrows self immutably via store/templates).
                    let dispatch_result = self.router.dispatch(&request);
                    let response = match dispatch_result {
                        Some((ref handler, ref params)) => {
                            dispatch_handler(handler, params, &self.store, &self.templates, &self.static_dir)
                        }
                        None => Response::not_found(),
                    };

                    // Now get the connection again to set the response.
                    let conn = self.connections.get_mut(&token).unwrap();
                    conn.set_response(&response);
                    if conn.write()? {
                        conn.set_done();
                    } else {
                        let fd = conn.fd;
                        self.event_loop.modify(fd, Interest::Writable, token)?;
                    }
                }
            }
            State::WritingResponse if event.writable => {
                if conn.write()? {
                    conn.set_done();
                }
            }
            _ => {}
        }

        Ok(())
    }

    fn handle_watcher_event(&mut self) -> io::Result<()> {
        let changes = self.watcher.process_events()?;

        for change in &changes {
            match change {
                ContentChange::Created(sys_title) | ContentChange::Modified(sys_title) => {
                    self.store.rebuild()?;
                    let version = self.store.article_version(sys_title).unwrap_or(0);
                    let tags = self
                        .store
                        .get_by_title(sys_title)?
                        .map(|a| a.tags.clone())
                        .unwrap_or_default();
                    let change_type = match change {
                        ContentChange::Created(_) => ChangeType::Created,
                        _ => ChangeType::Updated,
                    };
                    self.subscriptions.notify(sys_title, &tags, change_type, version)?;
                }
                ContentChange::Deleted(sys_title) => {
                    let version = self.store.article_version(sys_title).unwrap_or(0);
                    self.subscriptions.notify(sys_title, &[], ChangeType::Deleted, version)?;
                    self.store.rebuild()?;
                }
            }
        }

        Ok(())
    }
}

fn dispatch_handler(
    handler: &str,
    params: &RouteParams,
    store: &Store,
    templates: &TemplateRegistry,
    static_dir: &std::path::Path,
) -> Response {
    match handler {
        "home" => handlers::handle_home(store, templates),
        "article" => handlers::handle_article(params, store, templates),
        "tag_listing" => handlers::handle_tag(params, store, templates),
        "about" => handlers::handle_static_page("about", templates),
        "contact" => handlers::handle_static_page("contact", templates),
        "static_file" => {
            let path = params.get("path").unwrap_or("");
            wo_serve::send_file(static_dir, path)
        }
        _ => Response::not_found(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;

    fn create_test_article(base: &std::path::Path, sys_title: &str, published: bool) {
        let dir = base.join(sys_title);
        fs::create_dir_all(&dir).unwrap();
        let json = format!(
            r#"{{
                "title": "{}",
                "sys_title": "{}",
                "published": {},
                "content": {{
                    "author": "Author",
                    "content": {{
                        "sections": [{{ "heading": "Intro", "paragraphs": ["Hello."] }}],
                        "codes": [],
                        "images": []
                    }},
                    "tags": ["test"],
                    "publishedOn": 1000
                }},
                "published_on": 1000
            }}"#,
            sys_title, sys_title, published
        );
        fs::write(dir.join(format!("{}.json", sys_title)), json).unwrap();
    }

    fn test_config(content: &std::path::Path, data: &std::path::Path) -> Config {
        Config {
            bind_addr: String::new(), // no HTTP in tests
            ..Config::new(content, data)
        }
    }

    #[test]
    fn runtime_creates_and_queries() {
        let tmp = tempfile::tempdir().unwrap();
        let content = tmp.path().join("content");
        let data = tmp.path().join("data");

        create_test_article(&content, "art-one", true);
        create_test_article(&content, "art-two", true);

        let config = test_config(&content, &data);
        let rt = Runtime::new(&config).unwrap();

        let art = rt.store().get_by_title("art-one").unwrap().unwrap();
        assert_eq!(art.sys_title, "art-one");
        assert_eq!(rt.store().count_published().unwrap(), 2);
    }

    #[test]
    fn runtime_subscribe_and_notify() {
        let tmp = tempfile::tempdir().unwrap();
        let content = tmp.path().join("content");
        let data = tmp.path().join("data");

        create_test_article(&content, "art-one", true);

        let config = test_config(&content, &data);
        let mut rt = Runtime::new(&config).unwrap();

        let mut fds = [0i32; 2];
        assert_eq!(unsafe { libc::pipe2(fds.as_mut_ptr(), libc::O_NONBLOCK) }, 0);
        let (read_fd, write_fd) = (fds[0], fds[1]);

        rt.subscribe(write_fd, Subscription::ByTitle("art-one".into())).unwrap();

        rt.subscriptions
            .notify("art-one", &["test".into()], ChangeType::Updated, 1)
            .unwrap();

        let mut len_buf = [0u8; 4];
        let ret = unsafe { libc::read(read_fd, len_buf.as_mut_ptr() as *mut libc::c_void, 4) };
        assert_eq!(ret, 4);
        let len = u32::from_le_bytes(len_buf) as usize;

        let mut payload = vec![0u8; len];
        let ret = unsafe { libc::read(read_fd, payload.as_mut_ptr() as *mut libc::c_void, len) };
        assert_eq!(ret as usize, len);

        let notification: wo_sub::Notification = bincode::deserialize(&payload).unwrap();
        assert_eq!(notification.sys_title, "art-one");

        unsafe { libc::close(read_fd); libc::close(write_fd); }
    }

    #[test]
    fn runtime_shutdown_via_handle() {
        let tmp = tempfile::tempdir().unwrap();
        let content = tmp.path().join("content");
        let data = tmp.path().join("data");
        fs::create_dir_all(&content).unwrap();

        let config = test_config(&content, &data);
        let mut rt = Runtime::new(&config).unwrap();
        let handle = rt.handle();

        std::thread::spawn(move || {
            std::thread::sleep(Duration::from_millis(100));
            handle.shutdown();
        });

        rt.run().unwrap();
    }

    #[test]
    fn runtime_rebuild_on_start() {
        let tmp = tempfile::tempdir().unwrap();
        let content = tmp.path().join("content");
        let data = tmp.path().join("data");

        create_test_article(&content, "art-one", true);

        let config = test_config(&content, &data);
        let rt = Runtime::new(&config).unwrap();
        assert_eq!(rt.store().count_published().unwrap(), 1);
        drop(rt);

        let config = Config {
            rebuild_on_start: true,
            ..test_config(&content, &data)
        };
        let rt = Runtime::new(&config).unwrap();
        assert_eq!(rt.store().count_published().unwrap(), 1);
    }
}
