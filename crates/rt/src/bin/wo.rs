//! `wo` — the writeonce toolchain binary.
//!
//! Stage 2 scope:
//!   wo run <dir>   discover `.wo` files under <dir>, parse the type DSL,
//!                  compile a catalog, and serve REST CRUD on :8080.
//!   wo --help      print usage.
//!
//! After the phase-04 cutover this binary owned one event loop on one
//! thread; plan 09a upgrades that to thread-per-core — `WO_THREADS` pinned
//! workers, each with its own event loop and `SO_REUSEPORT` listener
//! (`rt::runtime::scheduler`). Engine state is still globally shared until
//! 09b. No tokio. See `docs/plan/09-concurrency-scaleout.md`.

use std::path::PathBuf;
use std::process::ExitCode;

fn usage() {
    eprintln!(
        "\
wo — writeonce toolchain

USAGE:
    wo run <dir>     parse .wo files under <dir>, serve REST CRUD on :8080
    wo --help        print this message

ENV:
    WO_LISTEN        override the listen address (default: 127.0.0.1:8080)
    WO_PG            postgres://user[:pass]@host[:port]/db — mirror every
                     committed write to Postgres as a backup (reads stay in
                     RAM; see docs/plan/16-postgres-mirror.md)
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

    // 4. Print the route banner.
    println!();
    println!("[wo] routes:");
    print!("{}", rt::server::describe_routes(&catalog));

    // 5. Serve — thread-per-core with a SHARDED engine (plan 09b): each
    //    worker owns its own Engine (interleaved ids) and its own router;
    //    cross-shard operations travel the shard bus (mailbox + eventfd).
    //    No Arc<Mutex<Engine>> anywhere.
    let addr = std::env::var("WO_LISTEN").unwrap_or_else(|_| "127.0.0.1:8080".to_string());
    let n = rt::runtime::scheduler::thread_count();
    println!();
    println!("[wo] listening on http://{addr} — {n} shard{} (thread-per-core, SO_REUSEPORT, sharded engine)",
        if n == 1 { "" } else { "s" });
    println!("[wo] ctrl-C to stop");

    // Durability (plan 09c): per-shard WAL under WO_DATA (default ./wo-data;
    // WO_DATA=off disables). A `meta` file pins the shard count — replaying
    // a 4-shard data dir with WO_THREADS=8 would strand logs and break the
    // id interleave, so a mismatch refuses to boot (resharding is 09f).
    let data_dir = std::env::var("WO_DATA").unwrap_or_else(|_| "./wo-data".to_string());
    let durable = data_dir != "off";
    if durable {
        std::fs::create_dir_all(&data_dir)?;
        let meta = std::path::Path::new(&data_dir).join("meta");
        match std::fs::read_to_string(&meta) {
            Ok(prev) => {
                let prev: usize = prev.trim().parse().unwrap_or(0);
                if prev != 0 && prev != n {
                    anyhow::bail!("{data_dir} was written with WO_THREADS={prev} — restart with that, or wipe the dir");
                }
            }
            Err(_) => std::fs::write(&meta, format!("{n}\n"))?,
        }
    }

    // Postgres backup mirror (plan 16b): RAM stays authoritative — the
    // `wo-pg` thread receives every committed mutation on a bounded channel
    // and upserts it as JSONB. Never in the ack path; off unless WO_PG set.
    let mirror_tx = match std::env::var("WO_PG") {
        Ok(url) => {
            let cfg = rt::pg::PgConfig::from_url(&url)
                .map_err(|e| anyhow::anyhow!("WO_PG: {e}"))?;
            let tables: Vec<(String, String)> = catalog.order.iter()
                .map(|name| (name.clone(), catalog.get(name).unwrap().storage_name.clone()))
                .collect();
            let (tx, rx) = std::sync::mpsc::sync_channel(rt::mirror::QUEUE_CAP);
            rt::mirror::spawn(cfg.clone(), rx, tables);
            println!("[wo] postgres mirror: {}:{}/{} (backup only — reads stay in RAM)",
                cfg.host, cfg.port, cfg.database);
            Some(tx)
        }
        Err(_) => None,
    };

    let bus = rt::shard::ShardBus::new(n)?;
    let catalog_for_workers = catalog.clone();
    rt::runtime::scheduler::serve(&addr, move |id| {
        use std::os::unix::io::AsRawFd;
        let mut engine = rt::engine::Engine::for_shard(catalog_for_workers.clone(), id, n);
        let mut has_group_wal = false;
        if durable {
            let path = std::path::Path::new(&data_dir).join(format!("shard-{id}.rwal"));
            let t0 = std::time::Instant::now();
            match rt::wal::Wal::open_and_replay(&path, &mut engine) {
                Ok((wal, recs)) => {
                    if recs > 0 {
                        println!("[wo] shard {id}: replayed {recs} wal records in {:?}", t0.elapsed());
                    }
                    // Group commit (io_uring): batch the tick's frames into
                    // one WRITE→FSYNC pair; acks ride the fsync CQE. Falls
                    // back to per-commit fsync if the ring is unavailable
                    // (or WO_GROUP_COMMIT=off, kept for A/B measurement).
                    let group_enabled = std::env::var("WO_GROUP_COMMIT").map(|v| v != "off").unwrap_or(true);
                    match (if group_enabled { rt::runtime::Uring::new(256) } else { Err(std::io::Error::other("disabled")) })
                        .and_then(|ring| rt::wal::WalGroup::new(wal, ring))
                    {
                        Ok(group) => { engine.attach_wal_group(group); has_group_wal = true; }
                        Err(e) => {
                            eprintln!("[wo] shard {id}: io_uring unavailable ({e}) — per-commit fsync");
                            // wal moved; reopen in per-commit mode
                            let mut scratch = rt::engine::Engine::for_shard(catalog_for_workers.clone(), id, n);
                            if let Ok((w2, _)) = rt::wal::Wal::open_and_replay(&path, &mut scratch) {
                                engine.attach_wal(w2);
                            }
                        }
                    }
                }
                Err(e) => eprintln!("[wo] shard {id}: WAL unavailable ({e}) — running non-durable"),
            }
        }
        // Mirror attaches AFTER replay: the replayed state goes to Postgres
        // once, as a boot-time bulk sync, then live mutations stream.
        if let Some(tx) = &mirror_tx {
            engine.attach_mirror(tx.clone());
            engine.mirror_sync_all();
        }
        let ctx     = rt::shard::ShardCtx::new(id, n, engine, bus.clone());
        let router  = rt::server::router(ctx.clone(), &catalog_for_workers);
        let mail_fd = bus.mail_fd(id).as_raw_fd();
        let wal_hooks = if has_group_wal {
            ctx.engine.borrow().wal_ring_fd().map(|rfd| {
                let pump_ctx   = ctx.clone();
                let unpark_ctx = ctx.clone();
                let park_ctx   = ctx.clone();
                rt::runtime::scheduler::WalHooks {
                    ring_fd:   rfd,
                    pump:      Box::new(move || pump_ctx.wal_pump()),
                    unparks:   Box::new(move || unpark_ctx.take_unparks()),
                    park_conn: Box::new(move |fd, gen| park_ctx.engine.borrow_mut().park_conn(fd, gen)),
                }
            })
        } else { None };
        let mail_ctx = ctx.clone();
        rt::runtime::scheduler::Worker {
            router,
            mail: Some((mail_fd, Box::new(move || mail_ctx.drain_inbox()))),
            wal:  wal_hooks,
        }
    })?;

    println!("[wo] all {n} shards joined — bye");
    Ok(ExitCode::from(0))
}
