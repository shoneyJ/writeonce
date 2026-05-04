//! Method + URL pattern → handler dispatch.
//!
//! Combined adaptation of `reference/crates/wo-route/src/{router,pattern}.rs`.
//! Handler shape is `Fn(&Request, &RouteParams) -> Response`, captured as a
//! boxed closure so each route closes over its own state (typically an
//! `Arc<Mutex<Engine>>` — see `crates/rt/src/server.rs`).
//!
//! Dispatch distinguishes 404 (no path matches any registered route) from
//! 405 (path matches at least one route but not for the request's method),
//! which the REST sample asserts (`expose list, get, ...` → POST → 405).

use std::collections::HashMap;

use super::{Method, Request, Response, Status};

pub type HandlerFn = dyn Fn(&Request, &RouteParams) -> Response + Send + Sync + 'static;

#[derive(Debug, Clone, PartialEq)]
enum Segment {
    Literal(String),
    Param(String),
    Wildcard(String),
}

#[derive(Debug, Clone)]
struct Pattern {
    segments: Vec<Segment>,
}

impl Pattern {
    fn compile(s: &str) -> Self {
        let segments = s.trim_start_matches('/')
            .split('/')
            .filter(|s| !s.is_empty())
            .map(|seg| {
                if let Some(name) = seg.strip_prefix(':') {
                    Segment::Param(name.to_string())
                } else if let Some(name) = seg.strip_prefix('*') {
                    Segment::Wildcard(name.to_string())
                } else {
                    Segment::Literal(seg.to_string())
                }
            })
            .collect();
        Self { segments }
    }

    fn matches(&self, path: &str) -> Option<Vec<(String, String)>> {
        let parts: Vec<&str> = path.trim_start_matches('/')
            .split('/')
            .filter(|s| !s.is_empty())
            .collect();
        let mut params = Vec::new();
        let mut pi = 0usize;
        for seg in &self.segments {
            match seg {
                Segment::Literal(lit) => {
                    if pi >= parts.len() || parts[pi] != lit { return None; }
                    pi += 1;
                }
                Segment::Param(name) => {
                    if pi >= parts.len() { return None; }
                    params.push((name.clone(), parts[pi].to_string()));
                    pi += 1;
                }
                Segment::Wildcard(name) => {
                    if pi >= parts.len() { return None; }
                    params.push((name.clone(), parts[pi..].join("/")));
                    return Some(params);
                }
            }
        }
        if pi == parts.len() { Some(params) } else { None }
    }
}

#[derive(Debug, Clone, Default)]
pub struct RouteParams {
    params: HashMap<String, String>,
}

impl RouteParams {
    pub fn get(&self, k: &str) -> Option<&str> {
        self.params.get(k).map(String::as_str)
    }

    fn from_pairs(pairs: Vec<(String, String)>) -> Self {
        Self { params: pairs.into_iter().collect() }
    }
}

struct Route {
    method:  Method,
    pattern: Pattern,
    handler: Box<HandlerFn>,
}

#[derive(Default)]
pub struct Router {
    routes: Vec<Route>,
}

impl Router {
    pub fn new() -> Self { Self::default() }

    pub fn route<F>(mut self, method: Method, pattern: &str, handler: F) -> Self
    where
        F: Fn(&Request, &RouteParams) -> Response + Send + Sync + 'static,
    {
        self.routes.push(Route {
            method,
            pattern: Pattern::compile(pattern),
            handler: Box::new(handler),
        });
        self
    }

    /// Resolve a request to a response. 404 if no path matches; 405 if the
    /// path matches a registered route under a different method.
    pub fn dispatch(&self, req: &Request) -> Response {
        let mut path_matched_any = false;
        for r in &self.routes {
            if let Some(pairs) = r.pattern.matches(&req.path) {
                if r.method == req.method {
                    let params = RouteParams::from_pairs(pairs);
                    return (r.handler)(req, &params);
                }
                path_matched_any = true;
            }
        }
        if path_matched_any {
            Response::status(Status::METHOD_NOT_ALLOWED).text("method not allowed")
        } else {
            Response::status(Status::NOT_FOUND).text("no route")
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashMap;

    fn req(method: Method, path: &str) -> Request {
        Request { method, path: path.into(), query: None, headers: HashMap::new(), body: vec![] }
    }

    #[test]
    fn dispatches_static_and_param_routes() {
        let r = Router::new()
            .route(Method::Get,  "/healthz",          |_, _| Response::ok().text("ok"))
            .route(Method::Get,  "/api/articles",     |_, _| Response::ok().text("list"))
            .route(Method::Get,  "/api/articles/:id", |_, p| Response::ok().text(format!("get {}", p.get("id").unwrap())))
            .route(Method::Post, "/api/articles",     |_, _| Response::created().text("create"));

        let resp = r.dispatch(&req(Method::Get, "/healthz"));
        assert_eq!(resp.status.0, 200);
        assert_eq!(resp.body, b"ok");

        let resp = r.dispatch(&req(Method::Get, "/api/articles/42"));
        assert_eq!(resp.body, b"get 42");

        let resp = r.dispatch(&req(Method::Post, "/api/articles"));
        assert_eq!(resp.status.0, 201);
    }

    #[test]
    fn returns_405_when_path_matches_but_method_does_not() {
        let r = Router::new()
            .route(Method::Get, "/api/articles", |_, _| Response::ok().text("list"));
        let resp = r.dispatch(&req(Method::Post, "/api/articles"));
        assert_eq!(resp.status.0, 405);
    }

    #[test]
    fn returns_404_when_no_path_matches() {
        let r = Router::new()
            .route(Method::Get, "/api/articles", |_, _| Response::ok().text("list"));
        let resp = r.dispatch(&req(Method::Get, "/nope"));
        assert_eq!(resp.status.0, 404);
    }
}
