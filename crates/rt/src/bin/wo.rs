//! `wo` — the writeonce toolchain binary.
//!
//! Stage 2 scope:
//!   wo run <dir>   discover `.wo` files under <dir>, parse the type DSL,
//!                  compile a catalog, and serve REST CRUD on :8080.
//!   wo --help      print usage.
//!
//! After the phase-04 cutover this binary owns one event loop on one
//! thread. No tokio. The same pattern Redis and TigerBeetle use — see
//! `docs/runtime/database/02-wo-language.md § Concurrency Model`.

use std::collections::HashMap;
use std::os::unix::io::{AsRawFd, RawFd};
use std::path::PathBuf;
use std::process::ExitCode;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use rt::http::{Connection, Listener, Router};
use rt::runtime::{EventLoop, Interest, SignalFd, Token};

fn usage() {
    eprintln!(
        "\
wo — writeonce toolchain

USAGE:
    wo run <dir>     parse .wo files under <dir>, serve REST CRUD on :8080
    wo --help        print this message

ENV:
    WO_LISTEN        override the listen address (default: 127.0.0.1:8080)
"
    );
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let slice: Vec<&str> = args.iter().map(|s| s.as_str()).collect();
    match slice.as_slice() {
        [] | ["--help"] | ["-h"] => { usage(); ExitCode::from(0) }
        ["run"] => {
            eprintln!("wo run: directory argument required");
            usage();
            ExitCode::from(2)
        }
        ["run", dir] => match run(PathBuf::from(dir)) {
            Ok(c)  => c,
            Err(e) => { eprintln!("error: {e}"); ExitCode::from(1) }
        },
        _ => { usage(); ExitCode::from(2) }
    }
}

fn run(dir: PathBuf) -> anyhow::Result<ExitCode> {
    // 1. Discover .wo files.
    let files = rt::discover(&dir)?;
    if files.is_empty() {
        anyhow::bail!("no .wo files found under {}", dir.display());
    }
    println!("[wo] discovered {} .wo file{} under {}",
        files.len(),
        if files.len() == 1 { "" } else { "s" },
        dir.display(),
    );

    // 2. Parse each into a Schema.
    let mut schemas = Vec::new();
    for f in &files {
        match rt::parser::parse(&f.src) {
            Ok(s) => {
                let n = s.types.len();
                println!("  parsed {} — {} type{}", f.rel.display(), n, if n == 1 { "" } else { "s" });
                schemas.push(s);
            }
            Err(e) => {
                eprintln!("  parse error in {}: {}", f.rel.display(), e);
                return Ok(ExitCode::from(1));
            }
        }
    }

    // 3. Compile the catalog.
    let catalog = rt::compile::Catalog::from_schemas(schemas)?;
    println!("[wo] compiled catalog — {} type{}", catalog.order.len(),
        if catalog.order.len() == 1 { "" } else { "s" });

    // 4. Boot the engine + the router.
    let engine = Arc::new(Mutex::new(rt::engine::Engine::new(catalog.clone())));
    println!();
    println!("[wo] routes:");
    {
        let e = engine.lock().unwrap();
        print!("{}", rt::server::describe_routes(&e));
    }
    let router = rt::server::router(engine.clone(), &catalog);

    // 5. Bind and serve.
    let addr = std::env::var("WO_LISTEN").unwrap_or_else(|_| "127.0.0.1:8080".to_string());
    let listener = Listener::bind(&addr)
        .map_err(|e| anyhow::anyhow!("bind {addr}: {e}"))?;
    println!();
    println!("[wo] listening on http://{}", listener.local_addr());
    println!("[wo] ctrl-C to stop");

    serve_loop(listener, router)?;
    Ok(ExitCode::from(0))
}

fn serve_loop(listener: Listener, router: Router) -> anyhow::Result<()> {
    let mut eloop  = EventLoop::new()?;
    let signals    = SignalFd::new()?;
    let listen_fd  = listener.as_raw_fd();
    let signal_fd  = signals.as_raw_fd();

    // Tokens: connection fds carry their own raw fd as the token; the
    // listener and signalfd use their fds too — they're disjoint by
    // construction (different fds).
    eloop.register(listen_fd, Interest::READABLE, Token(listen_fd as u64))?;
    eloop.register(signal_fd, Interest::READABLE, Token(signal_fd as u64))?;

    let mut conns: HashMap<RawFd, Connection> = HashMap::new();

    'outer: loop {
        let events = match eloop.wait_once(Some(Duration::from_secs(60))) {
            Ok(evs) => evs,
            Err(e)  => {
                eprintln!("[wo] event loop error: {e}");
                continue;
            }
        };

        for ev in events {
            let fd = ev.token().0 as RawFd;

            if fd == listen_fd {
                // Drain accept queue (edge-triggered).
                while let Some(cfd) = listener.accept()? {
                    eloop.register(cfd, Interest::READABLE, Token(cfd as u64))?;
                    conns.insert(cfd, Connection::new(cfd));
                }
                continue;
            }

            if fd == signal_fd {
                let sig = signals.read().unwrap_or(0);
                println!();
                println!("[wo] received signal {sig} — shutting down");
                break 'outer;
            }

            // Connection event.
            let Some(conn) = conns.get_mut(&fd) else { continue };
            let want_writable = match conn.drive(ev.readable, ev.writable, ev.hangup, ev.error, &router) {
                Ok(w)  => w,
                Err(_) => { conns.remove(&fd); continue; }
            };

            if conn.is_done() {
                eloop.deregister(fd).ok();
                conns.remove(&fd); // Drop closes the fd.
            } else if want_writable {
                let _ = eloop.modify(fd, Interest::READ_WRITE, Token(fd as u64));
            }
        }
    }

    // Tear down outstanding connections cleanly. Dropping Connection closes
    // each fd; deregistering from the loop is optional (close auto-removes).
    for (fd, _) in conns.drain() {
        let _ = eloop.deregister(fd);
    }
    Ok(())
}
