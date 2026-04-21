use std::io;

/// Magic bytes: "WOSF" (WriteOnce Segment File).
pub const MAGIC: [u8; 4] = *b"WOSF";

/// Segment file version.
pub const VERSION: u16 = 1;

/// Fixed header size in bytes.
pub const HEADER_SIZE: u64 = 32;

/// The segment file header, stored at byte 0 of every .seg file.
///
/// Layout (32 bytes):
/// ```text
/// [0..4]   magic: b"WOSF"
/// [4..6]   version: u16 LE
/// [6..8]   flags: u16 LE (reserved)
/// [8..16]  record_count: u64 LE
/// [16..24] data_start: u64 LE
/// [24..32] reserved: 8 bytes
/// ```
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct SegHeader {
    pub version: u16,
    pub flags: u16,
    pub record_count: u64,
    pub data_start: u64,
}

impl SegHeader {
    /// Create a new header with default values.
    pub fn new() -> Self {
        Self {
            version: VERSION,
            flags: 0,
            record_count: 0,
            data_start: HEADER_SIZE,
        }
    }

    /// Serialize the header into a 32-byte buffer.
    pub fn to_bytes(&self) -> [u8; 32] {
        let mut buf = [0u8; 32];
        buf[0..4].copy_from_slice(&MAGIC);
        buf[4..6].copy_from_slice(&self.version.to_le_bytes());
        buf[6..8].copy_from_slice(&self.flags.to_le_bytes());
        buf[8..16].copy_from_slice(&self.record_count.to_le_bytes());
        buf[16..24].copy_from_slice(&self.data_start.to_le_bytes());
        // [24..32] reserved, stays zero
        buf
    }

    /// Parse a header from a 32-byte buffer.
    pub fn from_bytes(buf: &[u8; 32]) -> io::Result<Self> {
        if &buf[0..4] != &MAGIC {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!(
                    "invalid magic bytes: expected {:?}, got {:?}",
                    MAGIC,
                    &buf[0..4]
                ),
            ));
        }

        let version = u16::from_le_bytes([buf[4], buf[5]]);
        if version != VERSION {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!("unsupported version: {}", version),
            ));
        }

        let flags = u16::from_le_bytes([buf[6], buf[7]]);
        let record_count = u64::from_le_bytes(buf[8..16].try_into().unwrap());
        let data_start = u64::from_le_bytes(buf[16..24].try_into().unwrap());

        Ok(Self {
            version,
            flags,
            record_count,
            data_start,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn round_trip() {
        let header = SegHeader {
            version: VERSION,
            flags: 0,
            record_count: 42,
            data_start: HEADER_SIZE,
        };
        let bytes = header.to_bytes();
        let parsed = SegHeader::from_bytes(&bytes).unwrap();
        assert_eq!(header, parsed);
    }

    #[test]
    fn bad_magic() {
        let mut bytes = SegHeader::new().to_bytes();
        bytes[0] = b'X';
        assert!(SegHeader::from_bytes(&bytes).is_err());
    }
}
