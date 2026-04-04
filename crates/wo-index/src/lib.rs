mod title;
mod date;
mod tags;

pub use title::TitleIndex;
pub use date::DateIndex;
pub use tags::TagIndex;

use std::io;
use std::path::Path;

/// Rebuild all three index files from an iterator of article metadata.
///
/// Each entry is `(sys_title, published_on_timestamp, tags, seg_offset)`.
pub fn rebuild_all(
    index_dir: &Path,
    entries: &[(String, i64, Vec<String>, u64)],
) -> io::Result<()> {
    std::fs::create_dir_all(index_dir)?;

    // Build title index.
    let title_entries: Vec<(&str, u64)> = entries.iter().map(|e| (e.0.as_str(), e.3)).collect();
    TitleIndex::build(&index_dir.join("title.idx"), &title_entries)?;

    // Build date index.
    let mut date_entries: Vec<(i64, u64)> = entries.iter().map(|e| (e.1, e.3)).collect();
    DateIndex::build(&index_dir.join("date.idx"), &mut date_entries)?;

    // Build tags index.
    let mut tag_map: std::collections::HashMap<String, Vec<u64>> = std::collections::HashMap::new();
    for entry in entries {
        for tag in &entry.2 {
            tag_map.entry(tag.clone()).or_default().push(entry.3);
        }
    }
    let tag_entries: Vec<(String, Vec<u64>)> = tag_map.into_iter().collect();
    TagIndex::build(&index_dir.join("tags.idx"), &tag_entries)?;

    Ok(())
}
