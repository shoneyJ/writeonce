use std::collections::HashMap;
use std::fs;
use std::io;
use std::path::Path;

use crate::ast::Node;
use crate::parser;

/// Holds parsed templates and partials, loaded from a templates directory.
pub struct TemplateRegistry {
    /// Page templates (e.g., "home" -> parsed nodes from home.htmlx).
    templates: HashMap<String, Vec<Node>>,
    /// Component partials (e.g., "article-card" -> parsed nodes).
    partials: HashMap<String, Vec<Node>>,
}

impl TemplateRegistry {
    /// Load all .htmlx files from the given templates directory.
    ///
    /// Templates in the root are page templates.
    /// Templates in `components/` are partials.
    pub fn load(templates_dir: &Path) -> io::Result<Self> {
        let mut templates = HashMap::new();
        let mut partials = HashMap::new();

        // Load page templates from root.
        if templates_dir.exists() {
            for entry in fs::read_dir(templates_dir)? {
                let entry = entry?;
                let path = entry.path();
                if path.is_file() && has_htmlx_ext(&path) {
                    let name = stem(&path);
                    let content = fs::read_to_string(&path)?;
                    templates.insert(name, parser::parse(&content));
                }
            }
        }

        // Load partials from components/.
        let components_dir = templates_dir.join("components");
        if components_dir.exists() {
            for entry in fs::read_dir(&components_dir)? {
                let entry = entry?;
                let path = entry.path();
                if path.is_file() && has_htmlx_ext(&path) {
                    let name = stem(&path);
                    let content = fs::read_to_string(&path)?;
                    partials.insert(name, parser::parse(&content));
                }
            }
        }

        Ok(Self { templates, partials })
    }

    /// Get a page template by name.
    pub fn get(&self, name: &str) -> Option<&Vec<Node>> {
        self.templates.get(name)
    }

    /// Get the partials map (for passing to render()).
    pub fn partials(&self) -> &HashMap<String, Vec<Node>> {
        &self.partials
    }

    /// Number of loaded templates.
    pub fn template_count(&self) -> usize {
        self.templates.len()
    }

    /// Number of loaded partials.
    pub fn partial_count(&self) -> usize {
        self.partials.len()
    }
}

fn has_htmlx_ext(path: &Path) -> bool {
    path.extension().and_then(|e| e.to_str()) == Some("htmlx")
}

fn stem(path: &Path) -> String {
    path.file_stem()
        .and_then(|s| s.to_str())
        .unwrap_or("")
        .to_string()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn load_templates() {
        let tmp = tempfile::tempdir().unwrap();
        let dir = tmp.path();

        fs::write(dir.join("home.htmlx"), "<h1>{{title}}</h1>").unwrap();
        fs::write(dir.join("article.htmlx"), "<article>{{body}}</article>").unwrap();

        fs::create_dir_all(dir.join("components")).unwrap();
        fs::write(
            dir.join("components/card.htmlx"),
            "<div>{{name}}</div>",
        )
        .unwrap();

        let reg = TemplateRegistry::load(dir).unwrap();
        assert_eq!(reg.template_count(), 2);
        assert_eq!(reg.partial_count(), 1);
        assert!(reg.get("home").is_some());
        assert!(reg.get("article").is_some());
        assert!(reg.partials().contains_key("card"));
    }

    #[test]
    fn empty_directory() {
        let tmp = tempfile::tempdir().unwrap();
        let reg = TemplateRegistry::load(tmp.path()).unwrap();
        assert_eq!(reg.template_count(), 0);
    }
}
