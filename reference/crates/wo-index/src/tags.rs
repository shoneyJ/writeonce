use std::collections::HashMap;
use std::io;
use std::path::Path;

use serde::{Deserialize, Serialize};

/// Inverted index mapping tag strings to lists of .seg byte offsets.
///
/// Serialized to disk using bincode for simplicity (article count is
/// small enough that loading the full index into memory is fine).
#[derive(Debug, Serialize, Deserialize)]
struct TagIndexData {
    tags: HashMap<String, Vec<u64>>,
}

pub struct TagIndex {
    tags: HashMap<String, Vec<u64>>,
}

impl TagIndex {
    /// Build a tags index from a list of (tag, offsets) pairs.
    pub fn build(path: &Path, entries: &[(String, Vec<u64>)]) -> io::Result<()> {
        let tags: HashMap<String, Vec<u64>> = entries.iter().cloned().collect();
        let data = TagIndexData { tags };
        let bytes = bincode::serialize(&data).map_err(|e| {
            io::Error::new(io::ErrorKind::InvalidData, format!("bincode: {}", e))
        })?;
        std::fs::write(path, bytes)?;
        Ok(())
    }

    /// Open a tags index from disk.
    pub fn open(path: &Path) -> io::Result<Self> {
        let bytes = std::fs::read(path)?;
        let data: TagIndexData = bincode::deserialize(&bytes).map_err(|e| {
            io::Error::new(io::ErrorKind::InvalidData, format!("bincode: {}", e))
        })?;
        Ok(Self { tags: data.tags })
    }

    /// Get all offsets for articles tagged with the given tag.
    pub fn get(&self, tag: &str) -> Option<&[u64]> {
        self.tags.get(tag).map(|v| v.as_slice())
    }

    /// All tag names in the index.
    pub fn tags(&self) -> Vec<&str> {
        self.tags.keys().map(|s| s.as_str()).collect()
    }

    /// Number of distinct tags.
    pub fn len(&self) -> usize {
        self.tags.len()
    }

    pub fn is_empty(&self) -> bool {
        self.tags.is_empty()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn build_and_lookup() {
        let tmp = tempfile::tempdir().unwrap();
        let path = tmp.path().join("tags.idx");

        let entries = vec![
            ("rust".to_string(), vec![100u64, 200]),
            ("linux".to_string(), vec![300]),
            ("tutorial".to_string(), vec![100, 300, 400]),
        ];

        TagIndex::build(&path, &entries).unwrap();
        let idx = TagIndex::open(&path).unwrap();

        assert_eq!(idx.len(), 3);
        assert_eq!(idx.get("rust"), Some(vec![100u64, 200].as_slice()));
        assert_eq!(idx.get("linux"), Some(vec![300u64].as_slice()));
        assert_eq!(idx.get("tutorial"), Some(vec![100u64, 300, 400].as_slice()));
        assert_eq!(idx.get("nonexistent"), None);
    }

    #[test]
    fn empty_index() {
        let tmp = tempfile::tempdir().unwrap();
        let path = tmp.path().join("tags.idx");

        TagIndex::build(&path, &[]).unwrap();
        let idx = TagIndex::open(&path).unwrap();

        assert!(idx.is_empty());
        assert_eq!(idx.get("anything"), None);
    }
}
