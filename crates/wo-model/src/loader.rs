use std::fs;
use std::io;
use std::path::Path;

use crate::{Article, LegacyArticle};

/// Walks a content directory and loads all articles.
///
/// Supports two formats:
/// - **New**: minimal `.json` metadata + `.md` file for content
/// - **Legacy**: full JSON with nested sections/codes (falls back if no `.md`)
///
/// ```text
/// content_dir/
///   article-slug/
///     article-slug.json    # metadata
///     article-slug.md      # content (new format)
/// ```
pub struct ContentLoader;

impl ContentLoader {
    /// Load all articles from the given content directory.
    pub fn load_all(content_dir: &Path) -> io::Result<Vec<Article>> {
        let mut articles = Vec::new();

        let entries = fs::read_dir(content_dir)?;
        for entry in entries {
            let entry = entry?;
            let path = entry.path();

            if !path.is_dir() {
                continue;
            }

            match Self::load_from_dir(&path) {
                Ok(article) => articles.push(article),
                Err(e) => {
                    eprintln!(
                        "wo-model: skipping {:?}: {}",
                        path.file_name().unwrap_or_default(),
                        e
                    );
                }
            }
        }

        articles.sort_by(|a, b| {
            b.published_on
                .unwrap_or(0)
                .cmp(&a.published_on.unwrap_or(0))
        });

        Ok(articles)
    }

    /// Load a single article from a directory.
    ///
    /// Tries new format first (minimal JSON + .md), falls back to legacy JSON.
    pub fn load_from_dir(dir: &Path) -> io::Result<Article> {
        let json_file = Self::find_json_file(dir)?;
        let json_contents = fs::read_to_string(&json_file)?;

        // Check if a .md file exists alongside the JSON.
        let md_file = Self::find_md_file(dir);

        if let Some(md_path) = md_file {
            // New format: minimal JSON + markdown file.
            // Try parsing as new format first, then fall back to legacy.
            let mut article: Article =
                if let Ok(a) = serde_json::from_str::<Article>(&json_contents) {
                    a
                } else {
                    let legacy: LegacyArticle =
                        serde_json::from_str(&json_contents).map_err(|e| {
                            io::Error::new(
                                io::ErrorKind::InvalidData,
                                format!("{}: {}", json_file.display(), e),
                            )
                        })?;
                    legacy.to_article()
                };

            let md_content = fs::read_to_string(&md_path)?;
            article.content_html = wo_md::markdown_to_html_block(&md_content);
            Ok(article)
        } else {
            // Legacy format: full JSON with sections, no .md file.
            let legacy: LegacyArticle = serde_json::from_str(&json_contents).map_err(|e| {
                io::Error::new(
                    io::ErrorKind::InvalidData,
                    format!("{}: {}", json_file.display(), e),
                )
            })?;
            Ok(legacy.to_article())
        }
    }

    /// Load a single article from a specific JSON file path.
    pub fn load_from_file(path: &Path) -> io::Result<Article> {
        let dir = path.parent().unwrap_or(Path::new("."));
        Self::load_from_dir(dir)
    }

    fn find_json_file(dir: &Path) -> io::Result<std::path::PathBuf> {
        for entry in fs::read_dir(dir)? {
            let entry = entry?;
            let path = entry.path();
            if path.extension().and_then(|e| e.to_str()) == Some("json") {
                return Ok(path);
            }
        }
        Err(io::Error::new(
            io::ErrorKind::NotFound,
            format!("no .json file in {}", dir.display()),
        ))
    }

    fn find_md_file(dir: &Path) -> Option<std::path::PathBuf> {
        let entries = fs::read_dir(dir).ok()?;
        for entry in entries {
            let entry = entry.ok()?;
            let path = entry.path();
            if path.extension().and_then(|e| e.to_str()) == Some("md") {
                return Some(path);
            }
        }
        None
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;

    /// Create a new-format article: minimal JSON + .md file.
    fn create_new_article(base: &Path, sys_title: &str, published: bool, tags: &[&str]) {
        let dir = base.join(sys_title);
        fs::create_dir_all(&dir).unwrap();

        let tags_json: Vec<String> = tags.iter().map(|t| format!("\"{}\"", t)).collect();
        let json = format!(
            r#"{{
                "sys_title": "{}",
                "title": "{}",
                "published": {},
                "author": "Test Author",
                "tags": [{}],
                "published_on": 2000
            }}"#,
            sys_title, sys_title, published, tags_json.join(", ")
        );
        fs::write(dir.join(format!("{}.json", sys_title)), json).unwrap();

        let md = format!("# {}\n\nThis is the **content** of the article.\n\n```rust\nfn main() {{}}\n```\n", sys_title);
        fs::write(dir.join(format!("{}.md", sys_title)), md).unwrap();
    }

    /// Create a legacy-format article: full JSON, no .md file.
    fn create_legacy_article(base: &Path, sys_title: &str, published: bool, tags: &[&str]) {
        let dir = base.join(sys_title);
        fs::create_dir_all(&dir).unwrap();

        let tags_json: Vec<String> = tags.iter().map(|t| format!("\"{}\"", t)).collect();
        let json = format!(
            r#"{{
                "title": "{}",
                "sys_title": "{}",
                "published": {},
                "content": {{
                    "author": "Test Author",
                    "content": {{
                        "sections": [{{ "heading": "Intro", "paragraphs": ["Hello **world**."] }}],
                        "codes": [],
                        "images": []
                    }},
                    "tags": [{}],
                    "publishedOn": 1000
                }},
                "published_on": 2000
            }}"#,
            sys_title, sys_title, published, tags_json.join(", ")
        );
        fs::write(dir.join(format!("{}.json", sys_title)), json).unwrap();
    }

    #[test]
    fn load_new_format() {
        let tmp = tempfile::tempdir().unwrap();
        create_new_article(tmp.path(), "my-article", true, &["rust"]);

        let article = ContentLoader::load_from_dir(&tmp.path().join("my-article")).unwrap();
        assert_eq!(article.sys_title, "my-article");
        assert_eq!(article.author, "Test Author");
        assert_eq!(article.tags, vec!["rust"]);
        assert!(article.content_html.contains("<h1>my-article</h1>"));
        assert!(article.content_html.contains("<strong>content</strong>"));
        assert!(article.content_html.contains("<pre><code"));
    }

    #[test]
    fn load_legacy_format() {
        let tmp = tempfile::tempdir().unwrap();
        create_legacy_article(tmp.path(), "legacy-art", true, &["test"]);

        let article = ContentLoader::load_from_dir(&tmp.path().join("legacy-art")).unwrap();
        assert_eq!(article.sys_title, "legacy-art");
        assert_eq!(article.author, "Test Author");
        assert!(article.content_html.contains("<h2>Intro</h2>"));
        assert!(article.content_html.contains("<strong>world</strong>"));
    }

    #[test]
    fn load_all_mixed() {
        let tmp = tempfile::tempdir().unwrap();
        create_new_article(tmp.path(), "new-art", true, &["rust"]);
        create_legacy_article(tmp.path(), "old-art", true, &["go"]);

        let articles = ContentLoader::load_all(tmp.path()).unwrap();
        assert_eq!(articles.len(), 2);

        let new = articles.iter().find(|a| a.sys_title == "new-art").unwrap();
        assert!(new.content_html.contains("<h1>"));

        let old = articles.iter().find(|a| a.sys_title == "old-art").unwrap();
        assert!(old.content_html.contains("<h2>Intro</h2>"));
    }

    #[test]
    fn skip_non_directory_entries() {
        let tmp = tempfile::tempdir().unwrap();
        create_new_article(tmp.path(), "valid-article", true, &[]);
        fs::write(tmp.path().join("README.md"), "not an article").unwrap();

        let articles = ContentLoader::load_all(tmp.path()).unwrap();
        assert_eq!(articles.len(), 1);
    }

    #[test]
    fn skip_directory_without_json() {
        let tmp = tempfile::tempdir().unwrap();
        create_new_article(tmp.path(), "valid-article", true, &[]);
        fs::create_dir_all(tmp.path().join("images")).unwrap();
        fs::write(tmp.path().join("images/photo.png"), b"fake image").unwrap();

        let articles = ContentLoader::load_all(tmp.path()).unwrap();
        assert_eq!(articles.len(), 1);
    }
}
