//! REST routing built from `service rest` blocks in the compiled catalog.
//!
//! For each type that declares `service rest "/path" expose ...`, we bind the
//! exposed operations at the given path:
//!
//! ```text
//! list      GET    /path
//! get       GET    /path/:id
//! create    POST   /path
//! update    PATCH  /path/:id
//! delete    DELETE /path/:id
//! subscribe stubbed in Stage 2; wires up in Stage 3 (501)
//! me        GET    /path/me                                  (501)
//! ```
//!
//! Phase-04 cutover: the axum + tokio backend was replaced with the
//! hand-rolled [`http::Router`](crate::http::Router) running on the
//! phase-02 [`EventLoop`](crate::runtime::EventLoop). Handlers are
//! synchronous, the engine sits behind `Arc<std::sync::Mutex<_>>`, and
//! the binary owns one event loop on one thread.

use crate::ast::{Operation, ServiceKind};
use crate::compile::Catalog;
use crate::engine::Engine;
use crate::http::{Method, Request, Response, RouteParams, Router, Status};

use serde_json::{json, Value};
use std::sync::{Arc, Mutex};

pub type Shared = Arc<Mutex<Engine>>;

/// Build the fully-wired [`Router`] for a running engine.
pub fn router(engine: Shared, catalog: &Catalog) -> Router {
    let mut r = Router::new()
        .route(Method::Get, "/",        |_, _| root_response())
        .route(Method::Get, "/healthz", |_, _| Response::ok().text("ok"));

    for name in &catalog.order {
        let t = catalog.get(name).expect("type present");
        for svc in &t.services {
            if svc.kind != ServiceKind::Rest { continue; }
            r = attach_rest(r, engine.clone(), t.name.clone(), svc.path.clone(), &svc.expose);
        }
    }
    r
}

fn root_response() -> Response {
    Response::ok().json(&json!({
        "runtime": "wo",
        "stage":   2,
        "notes":   "REST CRUD for each `service rest` block. /healthz for liveness. LIVE subscribe in Stage 3."
    }))
}

fn attach_rest(
    mut r:   Router,
    engine:  Shared,
    ty:      String,
    path:    String,
    ops:     &[Operation],
) -> Router {
    let id_path = format!("{path}/:id");

    // Register literal sub-paths (`/live`, `/me`) BEFORE the `/:id` param
    // route — the first matching pattern wins, so `:id` would otherwise
    // swallow "live" / "me" and produce a 400 invalid-id response.
    for op in ops {
        match op {
            Operation::Subscribe => {
                r = r.route(Method::Get, &format!("{path}/live"),
                    |_, _| Response::status(Status::NOT_IMPLEMENTED).text("LIVE subscriptions arrive in Stage 3"));
            }
            Operation::Me => {
                r = r.route(Method::Get, &format!("{path}/me"),
                    |_, _| Response::status(Status::NOT_IMPLEMENTED).text("session layer not yet implemented"));
            }
            _ => {}
        }
    }

    for op in ops {
        match op {
            Operation::List => {
                let eng = engine.clone(); let ty = ty.clone();
                r = r.route(Method::Get, &path, move |req, params| list_h(&eng, &ty, req, params));
            }
            Operation::Create => {
                let eng = engine.clone(); let ty = ty.clone();
                r = r.route(Method::Post, &path, move |req, params| create_h(&eng, &ty, req, params));
            }
            Operation::Get => {
                let eng = engine.clone(); let ty = ty.clone();
                r = r.route(Method::Get, &id_path, move |req, params| get_h(&eng, &ty, req, params));
            }
            Operation::Update => {
                let eng = engine.clone(); let ty = ty.clone();
                r = r.route(Method::Patch, &id_path, move |req, params| update_h(&eng, &ty, req, params));
            }
            Operation::Delete => {
                let eng = engine.clone(); let ty = ty.clone();
                r = r.route(Method::Delete, &id_path, move |req, params| delete_h(&eng, &ty, req, params));
            }
            Operation::Subscribe | Operation::Me | Operation::Custom => {}
        }
    }
    r
}

// --- handlers ---

fn list_h(engine: &Shared, ty: &str, _req: &Request, _params: &RouteParams) -> Response {
    let eng = engine.lock().unwrap();
    match eng.list(ty) {
        Ok(rows) => Response::ok().json(&json!(rows)),
        Err(e)   => Response::status(Status::INTERNAL_SERVER_ERROR).text(e.to_string()),
    }
}

fn get_h(engine: &Shared, ty: &str, _req: &Request, params: &RouteParams) -> Response {
    let id = match parse_id(params) {
        Ok(id) => id,
        Err(r) => return r,
    };
    let eng = engine.lock().unwrap();
    match eng.get(ty, id) {
        Ok(Some(row)) => Response::ok().json(&json!(row)),
        Ok(None)      => Response::status(Status::NOT_FOUND).text(format!("no {ty} with id {id}")),
        Err(e)        => Response::status(Status::INTERNAL_SERVER_ERROR).text(e.to_string()),
    }
}

fn create_h(engine: &Shared, ty: &str, req: &Request, _params: &RouteParams) -> Response {
    let body = match parse_json_body(req) {
        Ok(v)  => v,
        Err(r) => return r,
    };
    let mut eng = engine.lock().unwrap();
    match eng.create(ty, body) {
        Ok(row) => Response::status(Status::CREATED).json(&json!(row)),
        Err(e)  => Response::status(Status::BAD_REQUEST).text(e.to_string()),
    }
}

fn update_h(engine: &Shared, ty: &str, req: &Request, params: &RouteParams) -> Response {
    let id = match parse_id(params) {
        Ok(id) => id,
        Err(r) => return r,
    };
    let body = match parse_json_body(req) {
        Ok(v)  => v,
        Err(r) => return r,
    };
    let mut eng = engine.lock().unwrap();
    match eng.update(ty, id, body) {
        Ok(Some(row)) => Response::ok().json(&json!(row)),
        Ok(None)      => Response::status(Status::NOT_FOUND).text(format!("no {ty} with id {id}")),
        Err(e)        => Response::status(Status::BAD_REQUEST).text(e.to_string()),
    }
}

fn delete_h(engine: &Shared, ty: &str, _req: &Request, params: &RouteParams) -> Response {
    let id = match parse_id(params) {
        Ok(id) => id,
        Err(r) => return r,
    };
    let mut eng = engine.lock().unwrap();
    match eng.delete(ty, id) {
        Ok(true)  => Response::no_content(),
        Ok(false) => Response::status(Status::NOT_FOUND).text(format!("no {ty} with id {id}")),
        Err(e)    => Response::status(Status::INTERNAL_SERVER_ERROR).text(e.to_string()),
    }
}

fn parse_id(params: &RouteParams) -> Result<i64, Response> {
    params.get("id")
        .and_then(|s| s.parse::<i64>().ok())
        .ok_or_else(|| Response::status(Status::BAD_REQUEST).text("invalid id"))
}

fn parse_json_body(req: &Request) -> Result<Value, Response> {
    if req.body.is_empty() {
        return Ok(Value::Object(Default::default()));
    }
    serde_json::from_slice::<Value>(&req.body)
        .map_err(|e| Response::status(Status::BAD_REQUEST).text(format!("invalid JSON: {e}")))
}

/// Format the endpoint banner the CLI prints on startup.
pub fn describe_routes(engine: &Engine) -> String {
    let mut out = String::new();
    out.push_str("  GET    /                          runtime info\n");
    out.push_str("  GET    /healthz                   liveness\n");
    for name in &engine.catalog().order {
        let t = engine.catalog().get(name).unwrap();
        for svc in &t.services {
            if svc.kind != ServiceKind::Rest { continue; }
            for op in &svc.expose {
                let (m, p, n) = match op {
                    Operation::List      => ("GET   ", svc.path.clone(),                 format!("list    {name}")),
                    Operation::Get       => ("GET   ", format!("{}/:id",  svc.path),     format!("get     {name}")),
                    Operation::Create    => ("POST  ", svc.path.clone(),                 format!("create  {name}")),
                    Operation::Update    => ("PATCH ", format!("{}/:id",  svc.path),     format!("update  {name}")),
                    Operation::Delete    => ("DELETE", format!("{}/:id",  svc.path),     format!("delete  {name}")),
                    Operation::Subscribe => ("WS    ", format!("{}/live", svc.path),     format!("subscribe {name} (Stage 3)")),
                    Operation::Me        => ("GET   ", format!("{}/me",   svc.path),     "(Stage 3)".into()),
                    Operation::Custom    => continue,
                };
                out.push_str(&format!("  {m} {p:<30}  {n}\n"));
            }
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::compile::Catalog;
    use crate::parser::parse;

    fn build(src: &str) -> (Shared, Router) {
        let cat = Catalog::from_schemas(vec![parse(src).unwrap()]).unwrap();
        let eng = Arc::new(Mutex::new(Engine::new(cat.clone())));
        let r = router(eng.clone(), &cat);
        (eng, r)
    }

    fn req(method: Method, path: &str, body: &[u8]) -> Request {
        Request {
            method,
            path:    path.into(),
            query:   None,
            headers: Default::default(),
            body:    body.to_vec(),
        }
    }

    #[test]
    fn router_serves_crud_for_a_service_rest_block() {
        let (_eng, r) = build(r#"
type Article { id: Id
               title: Text
               service rest "/api/articles" expose list, get, create, update, delete }
"#);

        // empty list
        let resp = r.dispatch(&req(Method::Get, "/api/articles", b""));
        assert_eq!(resp.status.0, 200);
        assert_eq!(resp.body, b"[]");

        // create
        let resp = r.dispatch(&req(Method::Post, "/api/articles", br#"{"title":"hello"}"#));
        assert_eq!(resp.status.0, 201);

        // get by id
        let resp = r.dispatch(&req(Method::Get, "/api/articles/1", b""));
        assert_eq!(resp.status.0, 200);

        // delete
        let resp = r.dispatch(&req(Method::Delete, "/api/articles/1", b""));
        assert_eq!(resp.status.0, 204);

        // 404 after delete
        let resp = r.dispatch(&req(Method::Get, "/api/articles/1", b""));
        assert_eq!(resp.status.0, 404);
    }

    #[test]
    fn unexposed_method_yields_405() {
        let (_eng, r) = build(r#"
type Tag { id: Id
           label: Text
           service rest "/api/tags" expose list, get }
"#);
        let resp = r.dispatch(&req(Method::Post, "/api/tags", br#"{"label":"rust"}"#));
        assert_eq!(resp.status.0, 405);
    }

    #[test]
    fn live_subscribe_is_501() {
        let (_eng, r) = build(r#"
type Article { id: Id
               title: Text
               service rest "/api/articles" expose list, subscribe }
"#);
        let resp = r.dispatch(&req(Method::Get, "/api/articles/live", b""));
        assert_eq!(resp.status.0, 501);
    }
}
