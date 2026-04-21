use std::collections::hash_map::DefaultHasher;
use std::hash::{Hash, Hasher};
use std::io;
use std::path::Path;

/// On-disk hash index mapping `sys_title` (string) to byte offset in a .seg file.
///
/// Uses open addressing with linear probing. The table is sized at 2x the entry
/// count (load factor 0.5) to keep collision chains short.
///
/// On-disk format:
/// ```text
/// [8 bytes]  table_size: u64 LE (number of slots)
/// [8 bytes]  entry_count: u64 LE
/// For each slot (table_size slots):
///   [8 bytes]  offset: u64 LE (0 = empty, otherwise seg offset + 1)
///   [2 bytes]  key_len: u16 LE
///   [128 bytes] key_data: zero-padded sys_title
/// Total slot size: 138 bytes
/// ```
const SLOT_SIZE: usize = 8 + 2 + 128; // offset + key_len + key_data
const HEADER_SIZE: usize = 16; // table_size + entry_count
const MAX_KEY_LEN: usize = 128;

pub struct TitleIndex {
    data: Vec<u8>,
    table_size: u64,
}

impl TitleIndex {
    /// Build a title index file from a list of (sys_title, seg_offset) pairs.
    pub fn build(path: &Path, entries: &[(&str, u64)]) -> io::Result<()> {
        let table_size = (entries.len() * 2).max(16) as u64;
        let file_size = HEADER_SIZE + (table_size as usize) * SLOT_SIZE;

        let mut data = vec![0u8; file_size];

        // Write header.
        data[0..8].copy_from_slice(&table_size.to_le_bytes());
        data[8..16].copy_from_slice(&(entries.len() as u64).to_le_bytes());

        // Insert entries.
        for &(key, offset) in entries {
            let slot = Self::find_empty_slot(&data, table_size, key);
            Self::write_slot(&mut data, slot, key, offset);
        }

        std::fs::write(path, &data)?;
        Ok(())
    }

    /// Open a title index from disk (loads into memory).
    pub fn open(path: &Path) -> io::Result<Self> {
        let data = std::fs::read(path)?;
        if data.len() < HEADER_SIZE {
            return Err(io::Error::new(io::ErrorKind::InvalidData, "title.idx too small"));
        }
        let table_size = u64::from_le_bytes(data[0..8].try_into().unwrap());
        Ok(Self { data, table_size })
    }

    /// Look up a sys_title, returning the seg file byte offset if found.
    pub fn get(&self, sys_title: &str) -> Option<u64> {
        let mut slot = Self::hash_key(sys_title) % self.table_size;

        for _ in 0..self.table_size {
            let (stored_offset, stored_key) = self.read_slot(slot as usize);

            if stored_offset == 0 {
                return None; // empty slot, key not found
            }

            if stored_key == sys_title {
                return Some(stored_offset - 1); // stored as offset + 1
            }

            slot = (slot + 1) % self.table_size;
        }

        None
    }

    /// Number of entries in the index.
    pub fn len(&self) -> usize {
        u64::from_le_bytes(self.data[8..16].try_into().unwrap()) as usize
    }

    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    fn hash_key(key: &str) -> u64 {
        let mut hasher = DefaultHasher::new();
        key.hash(&mut hasher);
        hasher.finish()
    }

    fn find_empty_slot(data: &[u8], table_size: u64, key: &str) -> usize {
        let mut slot = (Self::hash_key(key) % table_size) as usize;
        loop {
            let base = HEADER_SIZE + slot * SLOT_SIZE;
            let stored_offset = u64::from_le_bytes(data[base..base + 8].try_into().unwrap());
            if stored_offset == 0 {
                return slot;
            }
            slot = (slot + 1) % table_size as usize;
        }
    }

    fn write_slot(data: &mut [u8], slot: usize, key: &str, offset: u64) {
        let base = HEADER_SIZE + slot * SLOT_SIZE;
        let stored_offset = offset + 1; // +1 so that 0 means empty
        data[base..base + 8].copy_from_slice(&stored_offset.to_le_bytes());

        let key_bytes = key.as_bytes();
        let key_len = key_bytes.len().min(MAX_KEY_LEN) as u16;
        data[base + 8..base + 10].copy_from_slice(&key_len.to_le_bytes());
        data[base + 10..base + 10 + key_len as usize].copy_from_slice(&key_bytes[..key_len as usize]);
    }

    fn read_slot(&self, slot: usize) -> (u64, String) {
        let base = HEADER_SIZE + slot * SLOT_SIZE;
        let stored_offset = u64::from_le_bytes(self.data[base..base + 8].try_into().unwrap());
        let key_len = u16::from_le_bytes(self.data[base + 8..base + 10].try_into().unwrap()) as usize;
        let key = String::from_utf8_lossy(&self.data[base + 10..base + 10 + key_len]).to_string();
        (stored_offset, key)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn build_and_lookup() {
        let tmp = tempfile::tempdir().unwrap();
        let path = tmp.path().join("title.idx");

        let entries = vec![
            ("linux-misc", 32u64),
            ("aws-lambda-pulumi", 500),
            ("rust-patterns", 1200),
        ];

        TitleIndex::build(&path, &entries).unwrap();
        let idx = TitleIndex::open(&path).unwrap();

        assert_eq!(idx.len(), 3);
        assert_eq!(idx.get("linux-misc"), Some(32));
        assert_eq!(idx.get("aws-lambda-pulumi"), Some(500));
        assert_eq!(idx.get("rust-patterns"), Some(1200));
        assert_eq!(idx.get("nonexistent"), None);
    }

    #[test]
    fn empty_index() {
        let tmp = tempfile::tempdir().unwrap();
        let path = tmp.path().join("title.idx");

        TitleIndex::build(&path, &[]).unwrap();
        let idx = TitleIndex::open(&path).unwrap();

        assert!(idx.is_empty());
        assert_eq!(idx.get("anything"), None);
    }

    #[test]
    fn many_entries() {
        let tmp = tempfile::tempdir().unwrap();
        let path = tmp.path().join("title.idx");

        let keys: Vec<String> = (0..100).map(|i| format!("article-{}", i)).collect();
        let entries: Vec<(&str, u64)> = keys.iter().enumerate().map(|(i, k)| (k.as_str(), i as u64 * 100)).collect();

        TitleIndex::build(&path, &entries).unwrap();
        let idx = TitleIndex::open(&path).unwrap();

        assert_eq!(idx.len(), 100);
        for (i, key) in keys.iter().enumerate() {
            assert_eq!(idx.get(key), Some(i as u64 * 100));
        }
    }
}
