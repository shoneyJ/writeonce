use std::path::PathBuf;

/// Runtime configuration.
#[derive(Debug, Clone)]
pub struct Config {
    /// Path to the content directory (JSON + MD source files).
    pub content_dir: PathBuf,
    /// Path to the data directory (.seg + .idx derived files).
    pub data_dir: PathBuf,
    /// Path to the templates directory (.htmlx files).
    pub templates_dir: PathBuf,
    /// Path to the static assets directory (CSS, images).
    pub static_dir: PathBuf,
    /// HTTP bind address (e.g., "0.0.0.0:3000").
    pub bind_addr: String,
    /// Force a full rebuild on startup (ignore existing data/).
    pub rebuild_on_start: bool,
}

impl Config {
    pub fn new(content_dir: impl Into<PathBuf>, data_dir: impl Into<PathBuf>) -> Self {
        Self {
            content_dir: content_dir.into(),
            data_dir: data_dir.into(),
            templates_dir: PathBuf::from("templates"),
            static_dir: PathBuf::from("static"),
            bind_addr: "0.0.0.0:3000".into(),
            rebuild_on_start: false,
        }
    }
}
