use serde::{Deserialize, Serialize};

/// Markdown-first article: minimal metadata + pre-rendered HTML content.
///
/// The `sys_title` field is the primary key — a URL-safe slug
/// used for lookups and routing (e.g., "linux-misc").
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct Article {
    pub sys_title: String,
    pub title: String,
    pub published: bool,
    pub author: String,
    #[serde(default)]
    pub tags: Vec<String>,
    #[serde(default)]
    pub published_on: Option<i64>,
    /// Pre-rendered HTML from the .md file. Populated by ContentLoader.
    #[serde(default)]
    pub content_html: String,
}

// --- Legacy types for backwards compatibility with old JSON format ---

/// Legacy article format (nested JSON with sections/codes/etc.).
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct LegacyArticle {
    pub title: String,
    pub sys_title: String,
    pub published: bool,
    pub content: LegacyArticleContent,
    #[serde(default)]
    pub do_aws_sync: Option<bool>,
    #[serde(default)]
    pub published_on: Option<i64>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct LegacyArticleContent {
    pub author: String,
    pub content: LegacyArticleBody,
    #[serde(default, rename = "publishedOn")]
    pub published_on: Option<i64>,
    #[serde(default)]
    pub references: Vec<LegacyReference>,
    #[serde(default)]
    pub tags: Vec<String>,
    #[serde(default)]
    pub title: Option<String>,
    #[serde(default)]
    pub systitle: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct LegacyArticleBody {
    #[serde(default)]
    pub sections: Vec<LegacySection>,
    #[serde(default)]
    pub codes: Vec<serde_json::Value>,
    #[serde(default)]
    pub images: Vec<serde_json::Value>,
    #[serde(default)]
    pub img: Option<serde_json::Value>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct LegacySection {
    pub heading: String,
    #[serde(default)]
    pub paragraphs: Vec<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct LegacyReference {
    #[serde(default, rename = "dateAccessed")]
    pub date_accessed: Option<i64>,
    #[serde(default)]
    pub title: Option<String>,
    #[serde(default)]
    pub url: Option<String>,
}

impl LegacyArticle {
    /// Convert a legacy article to the new format by building HTML from sections.
    pub fn to_article(&self) -> Article {
        let mut html = String::new();
        for section in &self.content.content.sections {
            html.push_str(&format!("<h2>{}</h2>\n", section.heading));
            for para in &section.paragraphs {
                if !para.is_empty() {
                    html.push_str(&format!("<p>{}</p>\n", wo_md::markdown_to_html(para)));
                }
            }
        }

        Article {
            sys_title: self.sys_title.clone(),
            title: self.title.clone(),
            published: self.published,
            author: self.content.author.clone(),
            tags: self.content.tags.clone(),
            published_on: self.published_on.or(self.content.published_on),
            content_html: html,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn deserialize_new_format() {
        let json = r#"{
            "sys_title": "test-article",
            "title": "Test Article",
            "published": true,
            "author": "Author",
            "tags": ["test"],
            "published_on": 1000
        }"#;

        let article: Article = serde_json::from_str(json).unwrap();
        assert_eq!(article.sys_title, "test-article");
        assert_eq!(article.author, "Author");
        assert_eq!(article.tags, vec!["test"]);
        assert!(article.published);
        assert_eq!(article.content_html, "");
    }

    #[test]
    fn round_trip_bincode() {
        let article = Article {
            sys_title: "test".into(),
            title: "Test".into(),
            published: true,
            author: "Author".into(),
            tags: vec!["rust".into()],
            published_on: Some(1000),
            content_html: "<h1>Hello</h1>".into(),
        };

        let bytes = bincode::serialize(&article).unwrap();
        let deserialized: Article = bincode::deserialize(&bytes).unwrap();
        assert_eq!(article, deserialized);
    }

    #[test]
    fn legacy_to_article() {
        let json = r#"{
            "title": "Legacy Article",
            "sys_title": "legacy",
            "published": true,
            "content": {
                "author": "Author",
                "content": {
                    "sections": [
                        {"heading": "Intro", "paragraphs": ["Hello **world**."]}
                    ],
                    "codes": [],
                    "images": []
                },
                "tags": ["test"],
                "publishedOn": 1000
            },
            "published_on": 2000
        }"#;

        let legacy: LegacyArticle = serde_json::from_str(json).unwrap();
        let article = legacy.to_article();
        assert_eq!(article.sys_title, "legacy");
        assert_eq!(article.author, "Author");
        assert_eq!(article.tags, vec!["test"]);
        assert_eq!(article.published_on, Some(2000));
        assert!(article.content_html.contains("<h2>Intro</h2>"));
        assert!(article.content_html.contains("<strong>world</strong>"));
    }
}
