//! `wo` — the writeonce toolchain binary.
//!
//! Stage 2 scope:
//!   wo run <dir>   — discover `.wo` files under <dir>, parse the type DSL,
//!                    compile a catalog, and serve REST CRUD on :8080.
//!   wo --help      — print usage.

use std::path::PathBuf;
use std::process::ExitCode;
use std::sync::Arc;

use tokio::sync::Mutex;

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
        [] | ["--help"] | ["-h"] => {
            usage();
            ExitCode::from(0)
        }
        ["run"] => {
            eprintln!("wo run: directory argument required");
            usage();
            ExitCode::from(2)
        }
        ["run", dir] => run(PathBuf::from(dir)),
        _ => {
            usage();
            ExitCode::from(2)
        }
    }
}

fn run(dir: PathBuf) -> ExitCode {
    // Tokio current-thread runtime — matches the single-threaded engine design.
    let rt = match tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
    {
        Ok(r) => r,
        Err(e) => { eprintln!("error: build runtime: {e}"); return ExitCode::from(1); }
    };
    rt.block_on(async move { serve(dir).await })
        .unwrap_or_else(|e| { eprintln!("error: {e}"); ExitCode::from(1) })
}

async fn serve(dir: PathBuf) -> anyhow::Result<ExitCode> {
    // 1. Discover
    let files = rt::discover(&dir)?;
    if files.is_empty() {
        anyhow::bail!("no .wo files found under {}", dir.display());
    }
    println!("[wo] discovered {} .wo file{} under {}",
        files.len(),
        if files.len() == 1 { "" } else { "s" },
        dir.display(),
    );

    // 2. Parse each file into a Schema
    let mut schemas = Vec::new();
    for f in &files {
        match rt::parser::parse(&f.src) {
            Ok(s)  => {
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

    // 3. Compile a catalog
    let catalog = rt::compile::Catalog::from_schemas(schemas)?;
    println!("[wo] compiled catalog — {} type{}", catalog.order.len(),
        if catalog.order.len() == 1 { "" } else { "s" });

    // 4. Boot the engine
    let engine = Arc::new(Mutex::new(rt::engine::Engine::new(catalog.clone())));
    {
        let e = engine.lock().await;
        println!();
        println!("[wo] routes:");
        print!("{}", rt::server::describe_routes(&e));
    }

    // 5. Bind and serve
    let addr = std::env::var("WO_LISTEN").unwrap_or_else(|_| "127.0.0.1:8080".to_string());
    let listener = match tokio::net::TcpListener::bind(&addr).await {
        Ok(l) => l,
        Err(e) => anyhow::bail!("bind {addr}: {e}"),
    };
    println!();
    println!("[wo] listening on http://{addr}");
    println!("[wo] ctrl-C to stop");

    let app = rt::server::router(engine.clone(), &catalog);

    // Graceful shutdown on ctrl-C
    axum::serve(listener, app)
        .with_graceful_shutdown(async {
            let _ = tokio::signal::ctrl_c().await;
            println!("\n[wo] shutting down");
        })
        .await?;

    Ok(ExitCode::from(0))
}
