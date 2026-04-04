use std::collections::HashMap;

use wo_http::request::{Method, Request};

use crate::pattern::Pattern;

/// Extracted route parameters.
#[derive(Debug, Clone, Default)]
pub struct RouteParams {
    params: HashMap<String, String>,
}

impl RouteParams {
    pub fn get(&self, key: &str) -> Option<&str> {
        self.params.get(key).map(|s| s.as_str())
    }

    pub fn from_pairs(pairs: Vec<(String, String)>) -> Self {
        Self {
            params: pairs.into_iter().collect(),
        }
    }
}

/// A route entry: method + pattern + handler name.
struct Route {
    method: Method,
    pattern: Pattern,
    handler: String,
}

/// URL router that matches requests to named handlers.
pub struct Router {
    routes: Vec<Route>,
}

impl Router {
    pub fn new() -> Self {
        Self { routes: Vec::new() }
    }

    /// Add a route. Handler is a string name that the caller maps to a function.
    pub fn add(&mut self, method: Method, pattern: &str, handler: &str) {
        self.routes.push(Route {
            method,
            pattern: Pattern::compile(pattern),
            handler: handler.to_string(),
        });
    }

    /// Match a request to a route. Returns the handler name and extracted params.
    pub fn dispatch(&self, request: &Request) -> Option<(String, RouteParams)> {
        for route in &self.routes {
            if route.method != request.method {
                continue;
            }
            if let Some(pairs) = route.pattern.matches(&request.path) {
                return Some((
                    route.handler.clone(),
                    RouteParams::from_pairs(pairs),
                ));
            }
        }
        None
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn make_request(method: Method, path: &str) -> Request {
        Request {
            method,
            path: path.to_string(),
            query: None,
            headers: HashMap::new(),
        }
    }

    #[test]
    fn dispatch_routes() {
        let mut router = Router::new();
        router.add(Method::Get, "/", "home");
        router.add(Method::Get, "/blog/:sys_title", "article");
        router.add(Method::Get, "/about", "about");
        router.add(Method::Get, "/tag/:tag", "tag_listing");
        router.add(Method::Get, "/static/*path", "static_file");

        // Home.
        let (handler, _) = router.dispatch(&make_request(Method::Get, "/")).unwrap();
        assert_eq!(handler, "home");

        // Article.
        let (handler, params) = router
            .dispatch(&make_request(Method::Get, "/blog/linux-misc"))
            .unwrap();
        assert_eq!(handler, "article");
        assert_eq!(params.get("sys_title"), Some("linux-misc"));

        // About.
        let (handler, _) = router.dispatch(&make_request(Method::Get, "/about")).unwrap();
        assert_eq!(handler, "about");

        // Tag.
        let (handler, params) = router
            .dispatch(&make_request(Method::Get, "/tag/rust"))
            .unwrap();
        assert_eq!(handler, "tag_listing");
        assert_eq!(params.get("tag"), Some("rust"));

        // Static.
        let (handler, params) = router
            .dispatch(&make_request(Method::Get, "/static/styles/main.css"))
            .unwrap();
        assert_eq!(handler, "static_file");
        assert_eq!(params.get("path"), Some("styles/main.css"));

        // No match.
        assert!(router
            .dispatch(&make_request(Method::Get, "/nonexistent"))
            .is_none());

        // Wrong method.
        assert!(router.dispatch(&make_request(Method::Post, "/")).is_none());
    }
}
