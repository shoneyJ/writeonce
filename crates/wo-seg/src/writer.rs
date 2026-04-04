use std::fs::{File, OpenOptions};
use std::io;
use std::os::unix::io::AsRawFd;
use std::path::Path;

use wo_model::Article;

use crate::header::{SegHeader, HEADER_SIZE};
use crate::{FLAG_ACTIVE, RECORD_HEADER_SIZE};

/// Writes articles to a .seg file.
///
/// Records are appended sequentially. Each append returns the byte offset
/// of the record, which can be stored in an index for direct access.
pub struct SegWriter {
    file: File,
    header: SegHeader,
    /// Current write position (end of file).
    pos: u64,
}

impl SegWriter {
    /// Create a new .seg file at the given path.
    ///
    /// Writes the initial header and optionally pre-allocates disk space
    /// using `fallocate` to reduce fragmentation.
    pub fn create(path: &Path) -> io::Result<Self> {
        let file = OpenOptions::new()
            .read(true)
            .write(true)
            .create(true)
            .truncate(true)
            .open(path)?;

        let header = SegHeader::new();
        // Write the header at position 0.
        pwrite_all(&file, &header.to_bytes(), 0)?;

        // Pre-allocate 1 MB to reduce fragmentation.
        let _ = fallocate_safe(&file, HEADER_SIZE as i64, 1024 * 1024);

        Ok(Self {
            file,
            header,
            pos: HEADER_SIZE,
        })
    }

    /// Append an article as an active record.
    ///
    /// Returns the byte offset of the record start (the position of the
    /// length prefix), which can be used for direct reads via `SegReader::read_at`.
    pub fn append(&mut self, article: &Article) -> io::Result<u64> {
        let payload = bincode::serialize(article).map_err(|e| {
            io::Error::new(io::ErrorKind::InvalidData, format!("bincode: {}", e))
        })?;

        let record_offset = self.pos;
        let payload_len = payload.len() as u32;

        // Build record: [u32 length][u8 flags][payload]
        let mut record = Vec::with_capacity(RECORD_HEADER_SIZE + payload.len());
        record.extend_from_slice(&payload_len.to_le_bytes());
        record.push(FLAG_ACTIVE);
        record.extend_from_slice(&payload);

        pwrite_all(&self.file, &record, record_offset)?;

        self.pos += record.len() as u64;
        self.header.record_count += 1;

        // Update the header with the new record count.
        pwrite_all(&self.file, &self.header.to_bytes(), 0)?;

        Ok(record_offset)
    }

    /// Tombstone a record at the given offset.
    ///
    /// Sets the flags byte to `FLAG_TOMBSTONED` without modifying the payload.
    pub fn tombstone(&self, record_offset: u64) -> io::Result<()> {
        let flags_offset = record_offset + 4; // skip the u32 length
        pwrite_all(&self.file, &[crate::FLAG_TOMBSTONED], flags_offset)
    }

    /// Return the current header (record count, etc.).
    pub fn header(&self) -> &SegHeader {
        &self.header
    }

    /// Sync file data and metadata to disk.
    pub fn sync(&self) -> io::Result<()> {
        self.file.sync_all()
    }
}

/// Positional write using pwrite(2). Does not modify the file offset.
fn pwrite_all(file: &File, buf: &[u8], offset: u64) -> io::Result<()> {
    let fd = file.as_raw_fd();
    let mut written = 0usize;
    while written < buf.len() {
        let ret = unsafe {
            libc::pwrite(
                fd,
                buf[written..].as_ptr() as *const libc::c_void,
                buf.len() - written,
                (offset + written as u64) as libc::off_t,
            )
        };
        if ret < 0 {
            return Err(io::Error::last_os_error());
        }
        if ret == 0 {
            return Err(io::Error::new(io::ErrorKind::WriteZero, "pwrite returned 0"));
        }
        written += ret as usize;
    }
    Ok(())
}

/// Best-effort fallocate. Non-fatal if unsupported.
fn fallocate_safe(file: &File, offset: i64, len: i64) -> io::Result<()> {
    let ret = unsafe { libc::fallocate(file.as_raw_fd(), 0, offset, len) };
    if ret < 0 {
        Err(io::Error::last_os_error())
    } else {
        Ok(())
    }
}
