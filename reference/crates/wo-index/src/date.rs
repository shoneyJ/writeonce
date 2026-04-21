use std::io;
use std::path::Path;

/// Sorted index mapping timestamps to byte offsets in a .seg file.
///
/// On-disk format:
/// ```text
/// [8 bytes]  entry_count: u64 LE
/// For each entry (sorted by timestamp ascending):
///   [8 bytes]  timestamp: i64 LE
///   [8 bytes]  offset: u64 LE
/// ```
const ENTRY_SIZE: usize = 16; // i64 + u64
const HEADER_SIZE: usize = 8;

pub struct DateIndex {
    data: Vec<u8>,
    count: usize,
}

impl DateIndex {
    /// Build a date index. Entries are sorted by timestamp before writing.
    pub fn build(path: &Path, entries: &mut [(i64, u64)]) -> io::Result<()> {
        entries.sort_by_key(|e| e.0);

        let file_size = HEADER_SIZE + entries.len() * ENTRY_SIZE;
        let mut data = vec![0u8; file_size];

        data[0..8].copy_from_slice(&(entries.len() as u64).to_le_bytes());

        for (i, &(ts, offset)) in entries.iter().enumerate() {
            let base = HEADER_SIZE + i * ENTRY_SIZE;
            data[base..base + 8].copy_from_slice(&ts.to_le_bytes());
            data[base + 8..base + 16].copy_from_slice(&offset.to_le_bytes());
        }

        std::fs::write(path, &data)?;
        Ok(())
    }

    /// Open a date index from disk.
    pub fn open(path: &Path) -> io::Result<Self> {
        let data = std::fs::read(path)?;
        if data.len() < HEADER_SIZE {
            return Err(io::Error::new(io::ErrorKind::InvalidData, "date.idx too small"));
        }
        let count = u64::from_le_bytes(data[0..8].try_into().unwrap()) as usize;
        Ok(Self { data, count })
    }

    /// Get offsets for all entries with timestamps in [start, end].
    pub fn range(&self, start: i64, end: i64) -> Vec<u64> {
        let lo = self.lower_bound(start);
        let hi = self.upper_bound(end);

        (lo..hi).map(|i| self.offset_at(i)).collect()
    }

    /// Get offsets for the N most recent entries (highest timestamps).
    pub fn latest(&self, n: usize) -> Vec<u64> {
        let start = self.count.saturating_sub(n);
        (start..self.count)
            .rev()
            .map(|i| self.offset_at(i))
            .collect()
    }

    /// Number of entries.
    pub fn len(&self) -> usize {
        self.count
    }

    pub fn is_empty(&self) -> bool {
        self.count == 0
    }

    fn timestamp_at(&self, i: usize) -> i64 {
        let base = HEADER_SIZE + i * ENTRY_SIZE;
        i64::from_le_bytes(self.data[base..base + 8].try_into().unwrap())
    }

    fn offset_at(&self, i: usize) -> u64 {
        let base = HEADER_SIZE + i * ENTRY_SIZE;
        u64::from_le_bytes(self.data[base + 8..base + 16].try_into().unwrap())
    }

    /// Binary search: first index where timestamp >= target.
    fn lower_bound(&self, target: i64) -> usize {
        let (mut lo, mut hi) = (0usize, self.count);
        while lo < hi {
            let mid = lo + (hi - lo) / 2;
            if self.timestamp_at(mid) < target {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        lo
    }

    /// Binary search: first index where timestamp > target.
    fn upper_bound(&self, target: i64) -> usize {
        let (mut lo, mut hi) = (0usize, self.count);
        while lo < hi {
            let mid = lo + (hi - lo) / 2;
            if self.timestamp_at(mid) <= target {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        lo
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn build_and_range_query() {
        let tmp = tempfile::tempdir().unwrap();
        let path = tmp.path().join("date.idx");

        let mut entries = vec![
            (1000i64, 100u64),
            (2000, 200),
            (3000, 300),
            (4000, 400),
            (5000, 500),
        ];

        DateIndex::build(&path, &mut entries).unwrap();
        let idx = DateIndex::open(&path).unwrap();

        assert_eq!(idx.len(), 5);

        // Full range.
        let all = idx.range(0, 9999);
        assert_eq!(all, vec![100, 200, 300, 400, 500]);

        // Partial range.
        let mid = idx.range(2000, 4000);
        assert_eq!(mid, vec![200, 300, 400]);

        // Single.
        let one = idx.range(3000, 3000);
        assert_eq!(one, vec![300]);

        // Empty range.
        let none = idx.range(6000, 9000);
        assert!(none.is_empty());
    }

    #[test]
    fn latest_entries() {
        let tmp = tempfile::tempdir().unwrap();
        let path = tmp.path().join("date.idx");

        let mut entries = vec![
            (1000i64, 100u64),
            (2000, 200),
            (3000, 300),
        ];

        DateIndex::build(&path, &mut entries).unwrap();
        let idx = DateIndex::open(&path).unwrap();

        let top2 = idx.latest(2);
        assert_eq!(top2, vec![300, 200]);

        let top10 = idx.latest(10);
        assert_eq!(top10, vec![300, 200, 100]);
    }

    #[test]
    fn unsorted_input() {
        let tmp = tempfile::tempdir().unwrap();
        let path = tmp.path().join("date.idx");

        let mut entries = vec![
            (5000i64, 500u64),
            (1000, 100),
            (3000, 300),
        ];

        DateIndex::build(&path, &mut entries).unwrap();
        let idx = DateIndex::open(&path).unwrap();

        // Should be sorted on disk.
        let all = idx.range(0, 9999);
        assert_eq!(all, vec![100, 300, 500]);
    }
}
