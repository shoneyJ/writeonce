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
//! Since plan 09b the engine is **sharded**: each worker thread owns its own
//! [`Engine`] behind a [`ShardCtx`](crate::shard::ShardCtx) — no mutex, no
//! shared heap. Handlers resolve the owning shard from the row id
//! (`owner = (id-1) % n`, the interleaved-mint rule), run locally when it's
//! ours, ship a job over the shard bus when it isn't. Creates always mint
//! locally; lists fan out to every shard and merge by id.

use std::rc::Rc;

use crate::ast::{Operation, ServiceKind};
use crate::compile::Catalog;
use crate::engine::Row;
use crate::http::{Method, Request, Response, RouteParams, Router, Status};
use crate::shard::ShardCtx;

use serde_json::{json, Value};

/// Build the fully-wired [`Router`] for one shard's worker thread.
pub fn router(ctx: Rc<ShardCtx>, catalog: &Catalog) -> Router {
    let shard = ctx.id;
    let n     = ctx.n;
    let mut r = Router::new()
        .route(Method::Get, "/", move |_, _| {
            Response::ok().json(&json!({
                "runtime": "wo",
                "stage":   2,
                "threads": n,
                "shard":   shard,
                "notes":   "REST CRUD for each `service rest` block. /healthz for liveness. LIVE subscribe in Stage 3."
            }))
        })
        .route(Method::Get, "/healthz", |_, _| Response::ok().text("ok"));

    for name in &catalog.order {
        let t = catalog.get(name).expect("type present");
        for svc in &t.services {
            if svc.kind != ServiceKind::Rest { continue; }
            r = attach_rest(r, ctx.clone(), t.name.clone(), svc.path.clone(), &svc.expose);
        }
    }
    r
}

fn attach_rest(
    mut r: Router,
    ctx:   Rc<ShardCtx>,
    ty:    String,
    path:  String,
    ops:   &[Operation],
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
                let ctx = ctx.clone(); let ty = ty.clone();
                r = r.route(Method::Get, &path, move |req, params| list_h(&ctx, &ty, req, params));
            }
            Operation::Create => {
                let ctx = ctx.clone(); let ty = ty.clone();
                r = r.route(Method::Post, &path, move |req, params| create_h(&ctx, &ty, req, params));
            }
            Operation::Get => {
                let ctx = ctx.clone(); let ty = ty.clone();
                r = r.route(Method::Get, &id_path, move |req, params| get_h(&ctx, &ty, req, params));
            }
            Operation::Update => {
                let ctx = ctx.clone(); let ty = ty.clone();
                r = r.route(Method::Patch, &id_path, move |req, params| update_h(&ctx, &ty, req, params));
            }
            Operation::Delete => {
                let ctx = ctx.clone(); let ty = ty.clone();
                r = r.route(Method::Delete, &id_path, move |req, params| delete_h(&ctx, &ty, req, params));
            }
            Operation::Subscribe | Operation::Me | Operation::Custom => {}
        }
    }
    r
}

// --- handlers ---
//
// Cross-shard results travel as `Result<_, String>` (anyhow::Error isn't
// guaranteed Send-friendly to reconstruct losslessly; the string is what we
// put in the HTTP body anyway). `run_on` returning `None` means the owning
// shard is gone — only during shutdown — and maps to 503.

fn shard_gone() -> Response {
    Response::status(Status::INTERNAL_SERVER_ERROR).text("owning shard unavailable")
}

fn list_h(ctx: &Rc<ShardCtx>, ty: &str, _req: &Request, _params: &RouteParams) -> Response {
    let ty_owned = ty.to_string();
    // Fan out to every shard, merge by id — the cross-shard read per 09b.
    let per_shard: Vec<Result<Vec<Row>, String>> =
        ctx.fanout(move |e| e.list(&ty_owned).map_err(|e| e.to_string()));
    let mut rows = Vec::new();
    for r in per_shard {
        match r {
            Ok(mut v)  => rows.append(&mut v),
            Err(e)     => return Response::status(Status::INTERNAL_SERVER_ERROR).text(e),
        }
    }
    rows.sort_by_key(|row| row.get("id").and_then(|v| v.as_i64()).unwrap_or(0));
    Response::ok().json(&json!(rows))
}

fn get_h(ctx: &Rc<ShardCtx>, ty: &str, _req: &Request, params: &RouteParams) -> Response {
    let id = match parse_id(params) {
        Ok(id) => id,
        Err(r) => return r,
    };
    let ty_owned = ty.to_string();
    let res = ctx.run_on(ctx.owner_of(id), move |e| {
        e.get(&ty_owned, id).map_err(|e| e.to_string())
    });
    match res {
        None                => shard_gone(),
        Some(Ok(Some(row))) => Response::ok().json(&json!(row)),
        Some(Ok(None))      => Response::status(Status::NOT_FOUND).text(format!("no {ty} with id {id}")),
        Some(Err(e))        => Response::status(Status::INTERNAL_SERVER_ERROR).text(e),
    }
}

fn create_h(ctx: &Rc<ShardCtx>, ty: &str, req: &Request, _params: &RouteParams) -> Response {
    let body = match parse_json_body(req) {
        Ok(v)  => v,
        Err(r) => return r,
    };
    // Always local: the receiving shard mints from its own interleaved
    // stride, so the row it creates is by construction a row it owns.
    let res = ctx.engine.borrow_mut().create(ty, body);
    match res {
        Ok(row) => gate_if_staged(ctx, Response::status(Status::CREATED).json(&json!(row))),
        Err(e)  => Response::status(Status::BAD_REQUEST).text(e.to_string()),
    }
}

/// Group commit: a mutation that staged a WAL frame must not be acked until
/// the batch fsync — flag the response so the connection parks it.
fn gate_if_staged(ctx: &Rc<ShardCtx>, mut resp: Response) -> Response {
    if ctx.engine.borrow_mut().take_staged() {
        resp.gate = true;
    }
    resp
}

fn update_h(ctx: &Rc<ShardCtx>, ty: &str, req: &Request, params: &RouteParams) -> Response {
    let id = match parse_id(params) {
        Ok(id) => id,
        Err(r) => return r,
    };
    let body = match parse_json_body(req) {
        Ok(v)  => v,
        Err(r) => return r,
    };
    let ty_owned = ty.to_string();
    let res = ctx.run_on(ctx.owner_of(id), move |e| {
        e.update(&ty_owned, id, body).map_err(|e| e.to_string())
    });
    match res {
        None                => shard_gone(),
        Some(Ok(Some(row))) => gate_if_staged(ctx, Response::ok().json(&json!(row))),
        Some(Ok(None))      => Response::status(Status::NOT_FOUND).text(format!("no {ty} with id {id}")),
        Some(Err(e))        => Response::status(Status::BAD_REQUEST).text(e),
    }
}

fn delete_h(ctx: &Rc<ShardCtx>, ty: &str, _req: &Request, params: &RouteParams) -> Response {
    let id = match parse_id(params) {
        Ok(id) => id,
        Err(r) => return r,
    };
    let ty_owned = ty.to_string();
    let res = ctx.run_on(ctx.owner_of(id), move |e| {
        e.delete(&ty_owned, id).map_err(|e| e.to_string())
    });
    match res {
        None             => shard_gone(),
        Some(Ok(true))   => gate_if_staged(ctx, Response::no_content()),
        Some(Ok(false))  => Response::status(Status::NOT_FOUND).text(format!("no {ty} with id {id}")),
        Some(Err(e))     => Response::status(Status::INTERNAL_SERVER_ERROR).text(e),
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
pub fn describe_routes(catalog: &Catalog) -> String {
    let mut out = String::new();
    out.push_str("  GET    /                          runtime info\n");
    out.push_str("  GET    /healthz                   liveness\n");
    for name in &catalog.order {
        let t = catalog.get(name).unwrap();
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
    use crate::engine::Engine;
    use crate::parser::parse;
    use crate::shard::ShardBus;

    /// Single-shard context: `run_on` is always local, `fanout` is just us —
    /// exactly the WO_THREADS=1 production shape.
    fn build(src: &str) -> (Rc<ShardCtx>, Router) {
        let cat = Catalog::from_schemas(vec![parse(src).unwrap()]).unwrap();
        let bus = ShardBus::new(1).unwrap();
        let ctx = ShardCtx::new(0, 1, Engine::for_shard(cat.clone(), 0, 1), bus);
        let r = router(ctx.clone(), &cat);
        (ctx, r)
    }

    fn req(method: Method, path: &str, body: &[u8]) -> Request {
        Request {
            method,
            path:    path.into(),
            query:   None,
            headers: Default::default(),
            body:    body.to_vec(),
            keep_alive: true,
        }
    }

    #[test]
    fn router_serves_crud_for_a_service_rest_block() {
        let (_ctx, r) = build(r#"
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
        let (_ctx, r) = build(r#"
type Tag { id: Id
           label: Text
           service rest "/api/tags" expose list, get }
"#);
        let resp = r.dispatch(&req(Method::Post, "/api/tags", br#"{"label":"rust"}"#));
        assert_eq!(resp.status.0, 405);
    }

    #[test]
    fn live_subscribe_is_501() {
        let (_ctx, r) = build(r#"
type Article { id: Id
               title: Text
               service rest "/api/articles" expose list, subscribe }
"#);
        let resp = r.dispatch(&req(Method::Get, "/api/articles/live", b""));
        assert_eq!(resp.status.0, 501);
    }
}
