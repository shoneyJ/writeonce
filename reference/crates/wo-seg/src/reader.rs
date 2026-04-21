use std::fs::File;
use std::io;
use std::os::unix::io::AsRawFd;
use std::path::Path;

use wo_model::Article;

use crate::header::SegHeader;
use crate::{FLAG_TOMBSTONED, RECORD_HEADER_SIZE};

/// Reads articles from a .seg file.
pub struct SegReader {
    file: File,
    header: SegHeader,
}

impl SegReader {
    /// Open an existing .seg file and validate its header.
    pub fn open(path: &Path) -> io::Result<Self> {
        let file = File::open(path)?;
        let mut header_buf = [0u8; 32];
        pread_exact(&file, &mut header_buf, 0)?;
        let header = SegHeader::from_bytes(&header_buf)?;

        Ok(Self { file, header })
    }

    /// Read a single article record at the given byte offset.
    ///
    /// Returns `None` if the record is tombstoned.
    pub fn read_at(&self, offset: u64) -> io::Result<Option<Article>> {
        // Read the record header: [u32 length][u8 flags]
        let mut rec_header = [0u8; RECORD_HEADER_SIZE];
        pread_exact(&self.file, &mut rec_header, offset)?;

        let payload_len = u32::from_le_bytes(rec_header[0..4].try_into().unwrap()) as usize;
        let flags = rec_header[4];

        if flags == FLAG_TOMBSTONED {
            return Ok(None);
        }

        // Read the payload.
        let mut payload = vec![0u8; payload_len];
        pread_exact(&self.file, &mut payload, offset + RECORD_HEADER_SIZE as u64)?;

        let article: Article = bincode::deserialize(&payload).map_err(|e| {
            io::Error::new(io::ErrorKind::InvalidData, format!("bincode: {}", e))
        })?;

        Ok(Some(article))
    }

    /// Iterate all active (non-tombstoned) records.
    ///
    /// Returns `(byte_offset, Article)` pairs.
    pub fn iter(&self) -> io::Result<Vec<(u64, Article)>> {
        let mut results = Vec::new();
        let mut pos = self.header.data_start;

        for _ in 0..self.header.record_count {
            // Read record header.
            let mut rec_header = [0u8; RECORD_HEADER_SIZE];
            pread_exact(&self.file, &mut rec_header, pos)?;

            let payload_len = u32::from_le_bytes(rec_header[0..4].try_into().unwrap()) as usize;
            let flags = rec_header[4];
            let record_offset = pos;

            pos += RECORD_HEADER_SIZE as u64 + payload_len as u64;

            if flags == FLAG_TOMBSTONED {
                continue;
            }

            let mut payload = vec![0u8; payload_len];
            pread_exact(
                &self.file,
                &mut payload,
                record_offset + RECORD_HEADER_SIZE as u64,
            )?;

            let article: Article = bincode::deserialize(&payload).map_err(|e| {
                io::Error::new(io::ErrorKind::InvalidData, format!("bincode: {}", e))
            })?;

            results.push((record_offset, article));
        }

        Ok(results)
    }

    /// Return the parsed file header.
    pub fn header(&self) -> &SegHeader {
        &self.header
    }
}

/// Positional read using pread(2). Does not modify the file offset.
fn pread_exact(file: &File, buf: &mut [u8], offset: u64) -> io::Result<()> {
    let fd = file.as_raw_fd();
    let mut read = 0usize;
    while read < buf.len() {
        let ret = unsafe {
            libc::pread(
                fd,
                buf[read..].as_mut_ptr() as *mut libc::c_void,
                buf.len() - read,
                (offset + read as u64) as libc::off_t,
            )
        };
        if ret < 0 {
            return Err(io::Error::last_os_error());
        }
        if ret == 0 {
            return Err(io::Error::new(
                io::ErrorKind::UnexpectedEof,
                "pread returned 0",
            ));
        }
        read += ret as usize;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::SegWriter;
    use wo_model::*;

    fn sample_article(sys_title: &str) -> Article {
        Article {
            sys_title: sys_title.to_string(),
            title: format!("Title: {}", sys_title),
            published: true,
            author: "Author".into(),
            tags: vec!["test".into()],
            published_on: Some(2000),
            content_html: "<h1>Hello world.</h1>".into(),
        }
    }

    #[test]
    fn write_and_read_single() {
        let tmp = tempfile::tempdir().unwrap();
        let path = tmp.path().join("test.seg");

        let article = sample_article("test-one");

        let offset = {
            let mut writer = SegWriter::create(&path).unwrap();
            writer.append(&article).unwrap()
        };

        let reader = SegReader::open(&path).unwrap();
        assert_eq!(reader.header().record_count, 1);

        let loaded = reader.read_at(offset).unwrap().unwrap();
        assert_eq!(loaded.sys_title, "test-one");
        assert_eq!(loaded, article);
    }

    #[test]
    fn write_and_read_multiple() {
        let tmp = tempfile::tempdir().unwrap();
        let path = tmp.path().join("multi.seg");

        let articles: Vec<Article> = (0..5).map(|i| sample_article(&format!("art-{}", i))).collect();

        let offsets: Vec<u64> = {
            let mut writer = SegWriter::create(&path).unwrap();
            articles.iter().map(|a| writer.append(a).unwrap()).collect()
        };

        let reader = SegReader::open(&path).unwrap();
        assert_eq!(reader.header().record_count, 5);

        // Read each by offset.
        for (i, offset) in offsets.iter().enumerate() {
            let loaded = reader.read_at(*offset).unwrap().unwrap();
            assert_eq!(loaded.sys_title, format!("art-{}", i));
        }

        // Iterate all.
        let all = reader.iter().unwrap();
        assert_eq!(all.len(), 5);
    }

    #[test]
    fn tombstone_record() {
        let tmp = tempfile::tempdir().unwrap();
        let path = tmp.path().join("tomb.seg");

        let offset = {
            let mut writer = SegWriter::create(&path).unwrap();
            let o = writer.append(&sample_article("to-delete")).unwrap();
            writer.append(&sample_article("to-keep")).unwrap();
            writer.tombstone(o).unwrap();
            o
        };

        let reader = SegReader::open(&path).unwrap();

        // Direct read returns None for tombstoned.
        assert!(reader.read_at(offset).unwrap().is_none());

        // Iter skips tombstoned.
        let all = reader.iter().unwrap();
        assert_eq!(all.len(), 1);
        assert_eq!(all[0].1.sys_title, "to-keep");
    }

    #[test]
    fn corrupted_header() {
        let tmp = tempfile::tempdir().unwrap();
        let path = tmp.path().join("bad.seg");
        std::fs::write(&path, b"not a segment file at all!!!!!xx").unwrap();
        assert!(SegReader::open(&path).is_err());
    }
}
