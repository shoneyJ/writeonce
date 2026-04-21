use std::collections::HashMap;
use std::io;
use std::path::{Path, PathBuf};

use wo_index::{DateIndex, TagIndex, TitleIndex};
use wo_model::{Article, ContentLoader};
use wo_seg::{SegReader, SegWriter};

/// Unified storage engine composing .seg files and indexes.
///
/// Provides the query API that the rest of the system uses.
/// Handles cold-start (rebuild from content/) and incremental ingestion.
pub struct Store {
    seg_path: PathBuf,
    index_dir: PathBuf,
    content_dir: PathBuf,
    title_idx: TitleIndex,
    date_idx: DateIndex,
    tag_idx: TagIndex,
    /// Monotonic version counter per sys_title (for subscription diffs).
    versions: HashMap<String, u64>,
    version_counter: u64,
}

impl Store {
    /// Open an existing data directory, or cold-start from content/.
    ///
    /// If `data_dir` contains valid .seg and index files, opens them.
    /// Otherwise, rebuilds everything from `content_dir`.
    pub fn open(content_dir: &Path, data_dir: &Path) -> io::Result<Self> {
        let seg_path = data_dir.join("articles.seg");
        let index_dir = data_dir.join("index");

        if seg_path.exists() && index_dir.join("title.idx").exists() {
            Self::open_existing(content_dir, &seg_path, &index_dir)
        } else {
            let mut store = Self::init_empty(content_dir, &seg_path, &index_dir)?;
            store.rebuild()?;
            Ok(store)
        }
    }

    fn open_existing(
        content_dir: &Path,
        seg_path: &Path,
        index_dir: &Path,
    ) -> io::Result<Self> {
        let title_idx = TitleIndex::open(&index_dir.join("title.idx"))?;
        let date_idx = DateIndex::open(&index_dir.join("date.idx"))?;
        let tag_idx = TagIndex::open(&index_dir.join("tags.idx"))?;

        // Build version map from existing seg records.
        let reader = SegReader::open(seg_path)?;
        let mut versions = HashMap::new();
        let mut version_counter = 0u64;
        for (_, article) in reader.iter()? {
            version_counter += 1;
            versions.insert(article.sys_title.clone(), version_counter);
        }

        Ok(Self {
            seg_path: seg_path.to_path_buf(),
            index_dir: index_dir.to_path_buf(),
            content_dir: content_dir.to_path_buf(),
            title_idx,
            date_idx,
            tag_idx,
            versions,
            version_counter,
        })
    }

    fn init_empty(
        content_dir: &Path,
        seg_path: &Path,
        index_dir: &Path,
    ) -> io::Result<Self> {
        std::fs::create_dir_all(seg_path.parent().unwrap())?;
        std::fs::create_dir_all(index_dir)?;

        // Create empty seg file.
        let _writer = SegWriter::create(seg_path)?;

        // Create empty indexes.
        TitleIndex::build(&index_dir.join("title.idx"), &[])?;
        DateIndex::build(&index_dir.join("date.idx"), &mut [])?;
        TagIndex::build(&index_dir.join("tags.idx"), &[])?;

        let title_idx = TitleIndex::open(&index_dir.join("title.idx"))?;
        let date_idx = DateIndex::open(&index_dir.join("date.idx"))?;
        let tag_idx = TagIndex::open(&index_dir.join("tags.idx"))?;

        Ok(Self {
            seg_path: seg_path.to_path_buf(),
            index_dir: index_dir.to_path_buf(),
            content_dir: content_dir.to_path_buf(),
            title_idx,
            date_idx,
            tag_idx,
            versions: HashMap::new(),
            version_counter: 0,
        })
    }

    /// Full rebuild: load all articles from content/, rewrite .seg and indexes.
    pub fn rebuild(&mut self) -> io::Result<()> {
        let articles = ContentLoader::load_all(&self.content_dir)?;

        // Write all articles to a new .seg file.
        let mut writer = SegWriter::create(&self.seg_path)?;
        let mut index_entries: Vec<(String, i64, Vec<String>, u64)> = Vec::new();

        self.versions.clear();
        self.version_counter = 0;

        for article in &articles {
            let offset = writer.append(article)?;
            let timestamp = article.published_on.unwrap_or(0);

            index_entries.push((
                article.sys_title.clone(),
                timestamp,
                article.tags.clone(),
                offset,
            ));

            self.version_counter += 1;
            self.versions
                .insert(article.sys_title.clone(), self.version_counter);
        }

        writer.sync()?;

        // Rebuild all indexes.
        wo_index::rebuild_all(&self.index_dir, &index_entries)?;

        // Reload indexes.
        self.title_idx = TitleIndex::open(&self.index_dir.join("title.idx"))?;
        self.date_idx = DateIndex::open(&self.index_dir.join("date.idx"))?;
        self.tag_idx = TagIndex::open(&self.index_dir.join("tags.idx"))?;

        Ok(())
    }

    /// Look up a single article by sys_title.
    pub fn get_by_title(&self, sys_title: &str) -> io::Result<Option<Article>> {
        match self.title_idx.get(sys_title) {
            Some(offset) => {
                let reader = SegReader::open(&self.seg_path)?;
                reader.read_at(offset)
            }
            None => Ok(None),
        }
    }

    /// List published articles, most recent first, with pagination.
    pub fn list_published(&self, skip: usize, limit: usize) -> io::Result<Vec<Article>> {
        let offsets = self.date_idx.latest(skip + limit);
        let reader = SegReader::open(&self.seg_path)?;

        let mut articles = Vec::new();
        for &offset in offsets.iter().skip(skip).take(limit) {
            if let Some(article) = reader.read_at(offset)? {
                if article.published {
                    articles.push(article);
                }
            }
        }
        Ok(articles)
    }

    /// List all articles with a given tag.
    pub fn list_by_tag(&self, tag: &str) -> io::Result<Vec<Article>> {
        let offsets = match self.tag_idx.get(tag) {
            Some(offsets) => offsets,
            None => return Ok(vec![]),
        };

        let reader = SegReader::open(&self.seg_path)?;
        let mut articles = Vec::new();
        for &offset in offsets {
            if let Some(article) = reader.read_at(offset)? {
                articles.push(article);
            }
        }
        Ok(articles)
    }

    /// List articles published within a timestamp range.
    pub fn list_by_date_range(&self, start: i64, end: i64) -> io::Result<Vec<Article>> {
        let offsets = self.date_idx.range(start, end);
        let reader = SegReader::open(&self.seg_path)?;

        let mut articles = Vec::new();
        for offset in offsets {
            if let Some(article) = reader.read_at(offset)? {
                articles.push(article);
            }
        }
        Ok(articles)
    }

    /// Count of published articles.
    pub fn count_published(&self) -> io::Result<usize> {
        // For now, iterate and count. With small article counts this is fine.
        let reader = SegReader::open(&self.seg_path)?;
        let count = reader
            .iter()?
            .into_iter()
            .filter(|(_, a)| a.published)
            .count();
        Ok(count)
    }

    /// Ingest a single article from a JSON file path.
    ///
    /// Appends to .seg and triggers a full index rebuild.
    /// Returns the sys_title of the ingested article.
    pub fn ingest_article(&mut self, json_path: &Path) -> io::Result<String> {
        let article = ContentLoader::load_from_file(json_path)?;
        let sys_title = article.sys_title.clone();

        // For simplicity, rebuild the entire store.
        // A future optimization can do incremental append + index update.
        self.rebuild()?;

        Ok(sys_title)
    }

    /// Get the current version number for a sys_title.
    pub fn article_version(&self, sys_title: &str) -> Option<u64> {
        self.versions.get(sys_title).copied()
    }

    /// Get a reference to the content directory path.
    pub fn content_dir(&self) -> &Path {
        &self.content_dir
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;

    fn create_test_article(base: &Path, sys_title: &str, published: bool, tags: &[&str], ts: i64) {
        let dir = base.join(sys_title);
        fs::create_dir_all(&dir).unwrap();

        let tags_json: Vec<String> = tags.iter().map(|t| format!("\"{}\"", t)).collect();
        let json = format!(
            r#"{{
                "title": "{}",
                "sys_title": "{}",
                "published": {},
                "content": {{
                    "author": "Author",
                    "content": {{
                        "sections": [{{ "heading": "Intro", "paragraphs": ["Hello."] }}],
                        "codes": [],
                        "images": []
                    }},
                    "tags": [{}],
                    "publishedOn": {}
                }},
                "published_on": {}
            }}"#,
            sys_title,
            sys_title,
            published,
            tags_json.join(", "),
            ts,
            ts
        );
        fs::write(dir.join(format!("{}.json", sys_title)), json).unwrap();
    }

    #[test]
    fn cold_start_and_queries() {
        let tmp = tempfile::tempdir().unwrap();
        let content = tmp.path().join("content");
        let data = tmp.path().join("data");

        create_test_article(&content, "art-one", true, &["rust"], 1000);
        create_test_article(&content, "art-two", true, &["rust", "linux"], 2000);
        create_test_article(&content, "art-three", false, &["draft"], 3000);
        create_test_article(&content, "art-four", true, &["linux"], 4000);

        let store = Store::open(&content, &data).unwrap();

        // Title lookup.
        let art = store.get_by_title("art-one").unwrap().unwrap();
        assert_eq!(art.sys_title, "art-one");

        assert!(store.get_by_title("nonexistent").unwrap().is_none());

        // Tag query.
        let rust_articles = store.list_by_tag("rust").unwrap();
        assert_eq!(rust_articles.len(), 2);

        let linux_articles = store.list_by_tag("linux").unwrap();
        assert_eq!(linux_articles.len(), 2);

        // Date range.
        let range = store.list_by_date_range(1500, 3500).unwrap();
        assert_eq!(range.len(), 2); // art-two(2000) and art-three(3000)

        // Published count.
        let count = store.count_published().unwrap();
        assert_eq!(count, 3);

        // Version tracking.
        assert!(store.article_version("art-one").is_some());
        assert!(store.article_version("nonexistent").is_none());
    }

    #[test]
    fn rebuild_after_delete() {
        let tmp = tempfile::tempdir().unwrap();
        let content = tmp.path().join("content");
        let data = tmp.path().join("data");

        create_test_article(&content, "art-one", true, &["test"], 1000);

        // First open: cold start.
        let store = Store::open(&content, &data).unwrap();
        assert_eq!(store.count_published().unwrap(), 1);
        drop(store);

        // Delete data/ and reopen: should rebuild.
        fs::remove_dir_all(&data).unwrap();
        let store = Store::open(&content, &data).unwrap();
        assert_eq!(store.count_published().unwrap(), 1);
    }

    #[test]
    fn reopen_existing() {
        let tmp = tempfile::tempdir().unwrap();
        let content = tmp.path().join("content");
        let data = tmp.path().join("data");

        create_test_article(&content, "art-one", true, &["test"], 1000);

        // First open: cold start builds .seg + indexes.
        let store = Store::open(&content, &data).unwrap();
        assert_eq!(store.count_published().unwrap(), 1);
        drop(store);

        // Second open: loads existing files, no rebuild.
        let store = Store::open(&content, &data).unwrap();
        assert_eq!(store.count_published().unwrap(), 1);
        let art = store.get_by_title("art-one").unwrap().unwrap();
        assert_eq!(art.sys_title, "art-one");
    }
}
