//! Axum REST server built from `service rest` blocks in the compiled catalog.
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
//! subscribe stubbed in Stage 2; wires up in Stage 3
//! me        GET    /path/me       (stubbed 501)
//! ```
//!
//! The engine lives behind a single `Arc<Mutex<Engine>>` — matches the
//! single-threaded event-loop design of the runtime (see Phase 2 Concurrency
//! Model).

use crate::ast::{Operation, ServiceKind};
use crate::engine::Engine;

use axum::{
    extract::{Path, State},
    http::StatusCode,
    response::IntoResponse,
    routing::{get, post},
    Json, Router,
};
use serde_json::{json, Value};
use std::sync::Arc;
use tokio::sync::Mutex;

pub type Shared = Arc<Mutex<Engine>>;

/// Per-route type context carried via axum `State`.
#[derive(Clone)]
struct TypeState {
    engine: Shared,
    ty:     Arc<String>,
}

/// Build the fully-wired axum `Router` for a running engine.
/// The caller keeps the `Shared` handle alongside for Stage 3 subscription wiring.
pub fn router(engine: Shared, catalog: &crate::compile::Catalog) -> Router {
    let mut app = Router::new()
        .route("/", get(root))
        .route("/healthz", get(|| async { "ok" }));

    for name in &catalog.order {
        let t = catalog.get(name).expect("type present");
        for svc in &t.services {
            if svc.kind != ServiceKind::Rest { continue; }
            let state = TypeState {
                engine: engine.clone(),
                ty:     Arc::new(t.name.clone()),
            };
            app = attach_rest(app, state, svc.path.clone(), &svc.expose);
        }
    }

    app
}

async fn root() -> impl IntoResponse {
    Json(json!({
        "runtime": "wo",
        "stage":   2,
        "notes":   "REST CRUD for each `service rest` block. /healthz for liveness. LIVE subscribe in Stage 3."
    }))
}

fn attach_rest(
    mut app:  Router,
    state:    TypeState,
    path:     String,
    ops:      &[Operation],
) -> Router {
    // Build a per-type sub-router with shared state, then merge.
    let mut sub = Router::new();

    // Collect collection-path handlers and id-path handlers separately so
    // axum's `method_routing` merges correctly.
    let mut collection = None::<axum::routing::MethodRouter<TypeState>>;
    let mut by_id      = None::<axum::routing::MethodRouter<TypeState>>;

    for op in ops {
        match op {
            Operation::List => {
                collection = Some(match collection.take() {
                    Some(r) => r.get(list_h),
                    None    => get(list_h),
                });
            }
            Operation::Create => {
                collection = Some(match collection.take() {
                    Some(r) => r.post(create_h),
                    None    => post(create_h),
                });
            }
            Operation::Get => {
                by_id = Some(match by_id.take() {
                    Some(r) => r.get(get_h),
                    None    => get(get_h),
                });
            }
            Operation::Update => {
                by_id = Some(match by_id.take() {
                    Some(r) => r.patch(update_h),
                    None    => axum::routing::patch(update_h),
                });
            }
            Operation::Delete => {
                by_id = Some(match by_id.take() {
                    Some(r) => r.delete(delete_h),
                    None    => axum::routing::delete(delete_h),
                });
            }
            Operation::Subscribe => {
                let p = format!("{path}/live");
                sub = sub.route(&p, get(subscribe_stub));
            }
            Operation::Me => {
                let p = format!("{path}/me");
                sub = sub.route(&p, get(me_stub));
            }
            Operation::Custom => { /* reserved */ }
        }
    }

    if let Some(r) = collection { sub = sub.route(&path, r); }
    if let Some(r) = by_id      { sub = sub.route(&format!("{path}/:id"), r); }

    let sub = sub.with_state(state);
    app = app.merge(sub);
    app
}

// --- handlers ---

async fn list_h(State(st): State<TypeState>) -> impl IntoResponse {
    let eng = st.engine.lock().await;
    match eng.list(&st.ty) {
        Ok(rows) => (StatusCode::OK, Json(json!(rows))).into_response(),
        Err(e)   => (StatusCode::INTERNAL_SERVER_ERROR, e.to_string()).into_response(),
    }
}

async fn get_h(State(st): State<TypeState>, Path(id): Path<i64>) -> impl IntoResponse {
    let eng = st.engine.lock().await;
    match eng.get(&st.ty, id) {
        Ok(Some(row)) => (StatusCode::OK, Json(json!(row))).into_response(),
        Ok(None)      => (StatusCode::NOT_FOUND, format!("no {} with id {id}", st.ty)).into_response(),
        Err(e)        => (StatusCode::INTERNAL_SERVER_ERROR, e.to_string()).into_response(),
    }
}

async fn create_h(State(st): State<TypeState>, Json(body): Json<Value>) -> impl IntoResponse {
    let mut eng = st.engine.lock().await;
    match eng.create(&st.ty, body) {
        Ok(row) => (StatusCode::CREATED, Json(json!(row))).into_response(),
        Err(e)  => (StatusCode::BAD_REQUEST, e.to_string()).into_response(),
    }
}

async fn update_h(State(st): State<TypeState>, Path(id): Path<i64>, Json(body): Json<Value>) -> impl IntoResponse {
    let mut eng = st.engine.lock().await;
    match eng.update(&st.ty, id, body) {
        Ok(Some(row)) => (StatusCode::OK, Json(json!(row))).into_response(),
        Ok(None)      => (StatusCode::NOT_FOUND, format!("no {} with id {id}", st.ty)).into_response(),
        Err(e)        => (StatusCode::BAD_REQUEST, e.to_string()).into_response(),
    }
}

async fn delete_h(State(st): State<TypeState>, Path(id): Path<i64>) -> impl IntoResponse {
    let mut eng = st.engine.lock().await;
    match eng.delete(&st.ty, id) {
        Ok(true)  => StatusCode::NO_CONTENT.into_response(),
        Ok(false) => (StatusCode::NOT_FOUND, format!("no {} with id {id}", st.ty)).into_response(),
        Err(e)    => (StatusCode::INTERNAL_SERVER_ERROR, e.to_string()).into_response(),
    }
}

async fn subscribe_stub() -> impl IntoResponse {
    (StatusCode::NOT_IMPLEMENTED, "LIVE subscriptions arrive in Stage 3")
}

async fn me_stub() -> impl IntoResponse {
    (StatusCode::NOT_IMPLEMENTED, "session layer not yet implemented")
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
                    Operation::List      => ("GET   ", svc.path.clone(),                  format!("list    {name}")),
                    Operation::Get       => ("GET   ", format!("{}/:id", svc.path),       format!("get     {name}")),
                    Operation::Create    => ("POST  ", svc.path.clone(),                  format!("create  {name}")),
                    Operation::Update    => ("PATCH ", format!("{}/:id", svc.path),       format!("update  {name}")),
                    Operation::Delete    => ("DELETE", format!("{}/:id", svc.path),       format!("delete  {name}")),
                    Operation::Subscribe => ("WS    ", format!("{}/live", svc.path),      format!("subscribe {name} (Stage 3)")),
                    Operation::Me        => ("GET   ", format!("{}/me", svc.path),        "(Stage 3)".into()),
                    Operation::Custom    => continue,
                };
                out.push_str(&format!("  {m} {p:<30}  {n}\n"));
            }
        }
    }
    out
}
