use std::path::PathBuf;
use wo_rt::{Config, Runtime};

fn main() {
    let content_dir = PathBuf::from("content");
    let data_dir = PathBuf::from("data");

    let config = Config {
        content_dir,
        data_dir,
        templates_dir: PathBuf::from("templates"),
        static_dir: PathBuf::from("static"),
        bind_addr: "0.0.0.0:3000".into(),
        rebuild_on_start: true,
    };

    let mut rt = match Runtime::new(&config) {
        Ok(rt) => rt,
        Err(e) => {
            eprintln!("failed to start: {}", e);
            std::process::exit(1);
        }
    };

    eprintln!("writeonce started — content={} data={}",
        config.content_dir.display(), config.data_dir.display());

    if let Err(e) = rt.run() {
        eprintln!("runtime error: {}", e);
        std::process::exit(1);
    }

    eprintln!("writeonce shut down");
}
