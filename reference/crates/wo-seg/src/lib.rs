mod header;
mod reader;
mod writer;

pub use header::{SegHeader, HEADER_SIZE, MAGIC};
pub use reader::SegReader;
pub use writer::SegWriter;

/// Record flags.
pub const FLAG_ACTIVE: u8 = 0x00;
pub const FLAG_TOMBSTONED: u8 = 0x01;

/// Size of the per-record header: 4 bytes length + 1 byte flags.
pub const RECORD_HEADER_SIZE: usize = 5;
