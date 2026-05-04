//! writeonce runtime — `.wo` language engine.
//!
//! Stage 1: file discovery.       ← src/lib.rs::discover()
//! Stage 2: parser + engine + server.
//! Stage 3: LIVE subscriptions over WebSocket.

pub mod ast;
pub mod compile;
pub mod engine;
pub mod http;
pub mod lexer;
pub mod parser;
pub mod runtime;
pub mod server;
pub mod token;

use std::fs;
use std::path::{Path, PathBuf};

pub use ast::Schema;
pub use compile::Catalog;
pub use engine::Engine;

/// A discovered `.wo` source file, resolved to an absolute path with its
/// contents slurped into memory.
#[derive(Debug, Clone)]
pub struct WoFile {
    pub path: PathBuf,
    pub rel:  PathBuf,
    pub src:  String,
}

/// Discover every `.wo` file rooted at `dir`, returning them in stable
/// (sorted-by-relative-path) order. Recursive.
pub fn discover(dir: &Path) -> anyhow::Result<Vec<WoFile>> {
    let root = dir.canonicalize().map_err(|e| {
        anyhow::anyhow!("cannot resolve {}: {}", dir.display(), e)
    })?;
    let mut out = Vec::new();
    walk(&root, &root, &mut out)?;
    out.sort_by(|a, b| a.rel.cmp(&b.rel));
    Ok(out)
}

fn walk(root: &Path, dir: &Path, out: &mut Vec<WoFile>) -> anyhow::Result<()> {
    for entry in fs::read_dir(dir)? {
        let entry = entry?;
        let path  = entry.path();
        let ty    = entry.file_type()?;

        let name = entry.file_name();
        let name = name.to_string_lossy();
        if name.starts_with('.') || name == "target" || name == "data" || name == "node_modules" {
            continue;
        }

        if ty.is_dir() {
            walk(root, &path, out)?;
        } else if ty.is_file()
            && path.extension().and_then(|s| s.to_str()) == Some("wo")
        {
            let src = fs::read_to_string(&path)?;
            let rel = path.strip_prefix(root).unwrap_or(&path).to_path_buf();
            out.push(WoFile { path: path.clone(), rel, src });
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;

    #[test]
    fn discovers_wo_files_recursively() {
        let tmp = tempfile::tempdir().unwrap();
        let root = tmp.path();

        fs::write(root.join("app.wo"), "-- app").unwrap();
        fs::create_dir(root.join("types")).unwrap();
        fs::write(root.join("types/article.wo"), "-- article").unwrap();
        fs::write(root.join("types/README.md"), "not a wo file").unwrap();

        fs::create_dir(root.join(".hidden")).unwrap();
        fs::write(root.join(".hidden/x.wo"), "").unwrap();
        fs::create_dir(root.join("target")).unwrap();
        fs::write(root.join("target/built.wo"), "").unwrap();
        fs::create_dir(root.join("data")).unwrap();
        fs::write(root.join("data/runtime.wo"), "").unwrap();

        let files = discover(root).unwrap();
        assert_eq!(files.len(), 2, "expected 2 .wo files, got {:?}",
            files.iter().map(|f| &f.rel).collect::<Vec<_>>());
        assert!(files.iter().any(|f| f.rel == PathBuf::from("app.wo")));
        assert!(files.iter().any(|f| f.rel == PathBuf::from("types/article.wo")));
    }
}
