//! Row-scoped method execution — plan 13b (`docs/plan/13-class-model-live-pricing.md`).
//!
//! A class method is a free `fn` with a hidden first parameter: `self` binds
//! to the receiving row (fetched by the RPC route's `:id` on the owning
//! shard), arguments arrive as a JSON object, and the body — the
//! schema-layer DML of `02-wo-language.md` — runs inside an engine method
//! transaction. Commit emits ONE `WalRec::Txn` frame (all-or-nothing on
//! replay); any abort or execution error rolls RAM back completely.
//!
//! The Stage-2 engine is single-threaded per shard, so `in txn` /
//! `in txn snapshot` are already serializable by construction — the mode is
//! accepted and recorded, and every method (pure ones included) runs under
//! begin/commit so the semantics stay uniform when MVCC arrives.

use crate::ast::{BinOp, Expr, FieldTy, MethodDecl, Stmt, UnOp};
use crate::engine::Engine;

use serde_json::{json, Map, Value};

/// Why a method call failed — shaped for the RPC layer's status mapping.
#[derive(Debug)]
pub enum MethodError {
    /// No row of the receiving type with the requested id (→ 404).
    NoSuchRow,
    /// Missing / malformed arguments (→ 400).
    BadArgs(String),
    /// `assert … otherwise abort` fired — transaction rolled back (→ 409).
    Abort(String),
    /// Execution error (bad field, arithmetic on non-numbers, WAL failure…)
    /// — transaction rolled back (→ 500).
    Exec(String),
}

impl std::fmt::Display for MethodError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            MethodError::NoSuchRow   => write!(f, "no such row"),
            MethodError::BadArgs(m)  => write!(f, "bad arguments: {m}"),
            MethodError::Abort(m)    => write!(f, "aborted: {m}"),
            MethodError::Exec(m)     => write!(f, "{m}"),
        }
    }
}

/// Call `m` on row `id` of type `ty`. Runs entirely on the engine it's
/// handed — the RPC route routes to the owning shard before calling this.
/// Returns the method's `return` value (`Null` if it falls off the end).
pub fn call(
    e:    &mut Engine,
    ty:   &str,
    id:   i64,
    m:    &MethodDecl,
    args: &Map<String, Value>,
) -> Result<Value, MethodError> {
    // `self` is a snapshot of the receiving row at entry (spec: bindings
    // are snapshots; the row-scoped txn reads one consistent state).
    let row = e.get(ty, id)
        .map_err(|err| MethodError::Exec(err.to_string()))?
        .ok_or(MethodError::NoSuchRow)?;

    // Bind declared parameters. Missing → 400; extras are ignored.
    let mut scope: Map<String, Value> = Map::new();
    for (pname, pty) in &m.params {
        let Some(v) = args.get(pname) else {
            return Err(MethodError::BadArgs(format!("missing argument `{pname}` ({pty})")));
        };
        scope.insert(pname.clone(), v.clone());
    }

    e.begin_txn().map_err(|err| MethodError::Exec(err.to_string()))?;
    let mut cx = Cx { e, self_ty: ty, self_row: &row, scope };
    match exec_block(&mut cx, &m.body) {
        Ok(flow) => {
            cx.e.commit_txn().map_err(|err| MethodError::Exec(err.to_string()))?;
            Ok(match flow { Flow::Return(v) => v, Flow::Continue => Value::Null })
        }
        Err(err) => {
            cx.e.abort_txn();
            Err(err)
        }
    }
}

/// Statement-level control flow.
enum Flow {
    Continue,
    Return(Value),
}

struct Cx<'a> {
    e:        &'a mut Engine,
    self_ty:  &'a str,
    self_row: &'a Map<String, Value>,
    scope:    Map<String, Value>,
}

fn exec_block(cx: &mut Cx, stmts: &[Stmt]) -> Result<Flow, MethodError> {
    for s in stmts {
        match exec_stmt(cx, s)? {
            Flow::Continue  => {}
            r @ Flow::Return(_) => return Ok(r),
        }
    }
    Ok(Flow::Continue)
}

fn exec_stmt(cx: &mut Cx, s: &Stmt) -> Result<Flow, MethodError> {
    match s {
        Stmt::Let { name, expr } => {
            let v = eval(cx, expr)?;
            cx.scope.insert(name.clone(), v);
            Ok(Flow::Continue)
        }
        Stmt::Insert { ty, fields } => {
            let mut body = Map::new();
            for (fname, fexpr) in fields {
                body.insert(fname.clone(), eval(cx, fexpr)?);
            }
            cx.e.create(ty, Value::Object(body))
                .map_err(|e| MethodError::Exec(format!("insert {ty}: {e}")))?;
            Ok(Flow::Continue)
        }
        Stmt::Return { expr } => {
            let v = match expr {
                Some(e) => eval(cx, e)?,
                None    => Value::Null,
            };
            Ok(Flow::Return(v))
        }
        Stmt::Assert { cond, msg } => {
            if truthy(&eval(cx, cond)?) {
                Ok(Flow::Continue)
            } else {
                Err(MethodError::Abort(
                    msg.clone().unwrap_or_else(|| "assertion failed".into())))
            }
        }
        Stmt::If { cond, then, otherwise } => {
            if truthy(&eval(cx, cond)?) {
                exec_block(cx, then)
            } else {
                exec_block(cx, otherwise)
            }
        }
    }
}

fn truthy(v: &Value) -> bool {
    match v {
        Value::Bool(b) => *b,
        Value::Null    => false,
        _              => true,
    }
}

fn eval(cx: &mut Cx, e: &Expr) -> Result<Value, MethodError> {
    match e {
        Expr::Int(n)   => Ok(json!(n)),
        Expr::Str(s)   => Ok(Value::String(s.clone())),
        Expr::Bool(b)  => Ok(json!(b)),
        Expr::Null     => Ok(Value::Null),
        Expr::Ident(name) => {
            if name == "self" {
                return Ok(Value::Object(cx.self_row.clone()));
            }
            cx.scope.get(name).cloned()
                .ok_or_else(|| MethodError::Exec(format!("unknown name `{name}`")))
        }
        Expr::Field(base, field) => {
            // `self.<relation>` resolves the relation; anything else is
            // plain object access on the evaluated base.
            if matches!(&**base, Expr::Ident(n) if n == "self") {
                if let Some(rel) = relation_rows(cx, field)? {
                    return Ok(rel);
                }
            }
            let b = eval(cx, base)?;
            match b {
                Value::Object(m) => m.get(field).cloned().ok_or_else(||
                    MethodError::Exec(format!("no field `{field}`"))),
                // Dotted access distributes over a set — the spec's
                // cardinality rule: `select Price{...}.amount` is the set
                // of amounts.
                Value::Array(items) => {
                    let mut out = Vec::with_capacity(items.len());
                    for it in items {
                        match it {
                            Value::Object(m) => out.push(m.get(field).cloned()
                                .ok_or_else(|| MethodError::Exec(
                                    format!("no field `{field}` in set element")))?),
                            other => return Err(MethodError::Exec(format!(
                                "`.{field}` on a non-object set element ({other})"))),
                        }
                    }
                    Ok(Value::Array(out))
                }
                other => Err(MethodError::Exec(format!(
                    "`.{field}` on a non-object value ({other})"))),
            }
        }
        Expr::Select { ty, predicates, projection } => {
            // Equality predicates route through the engine's secondary
            // indexes (`@table(index: ...)`) via find_by; other comparison
            // operators filter the candidates.
            let mut eq   = Vec::new();
            let mut rest = Vec::new();
            for (field, op, rhs) in predicates {
                let v = eval(cx, rhs)?;
                if *op == BinOp::Eq { eq.push((field.clone(), v)); }
                else                { rest.push((field.as_str(), *op, v)); }
            }
            let rows = cx.e.find_by(ty, &eq)
                .map_err(|e| MethodError::Exec(format!("select {ty}: {e}")))?;
            let mut out = Vec::new();
            for row in rows {
                if !rest.iter().all(|(f, op, v)| pred_holds(row.get(*f), *op, v)) {
                    continue;
                }
                out.push(if projection.is_empty() {
                    Value::Object(row)
                } else {
                    let mut shaped = Map::new();
                    for p in projection {
                        if let Some(v) = row.get(p) {
                            shaped.insert(p.clone(), v.clone());
                        }
                    }
                    Value::Object(shaped)
                });
            }
            Ok(Value::Array(out))
        }
        Expr::Call(name, args) => {
            let mut vals = Vec::with_capacity(args.len());
            for a in args { vals.push(eval(cx, a)?); }
            builtin(name, vals)
        }
        Expr::Unary(op, inner) => {
            let v = eval(cx, inner)?;
            match op {
                UnOp::Neg => {
                    let n = as_i64(&v)?;
                    Ok(json!(-n))
                }
                UnOp::Not => Ok(json!(!truthy(&v))),
            }
        }
        Expr::Binary(op, l, r) => {
            // Short-circuit boolean operators.
            match op {
                BinOp::And => {
                    let lv = eval(cx, l)?;
                    if !truthy(&lv) { return Ok(json!(false)); }
                    return Ok(json!(truthy(&eval(cx, r)?)));
                }
                BinOp::Or => {
                    let lv = eval(cx, l)?;
                    if truthy(&lv) { return Ok(json!(true)); }
                    return Ok(json!(truthy(&eval(cx, r)?)));
                }
                _ => {}
            }
            let lv = eval(cx, l)?;
            let rv = eval(cx, r)?;
            match op {
                BinOp::Eq => Ok(json!(lv == rv)),
                BinOp::Ne => Ok(json!(lv != rv)),
                BinOp::Lt | BinOp::Le | BinOp::Gt | BinOp::Ge => {
                    let (a, b) = (as_i64(&lv)?, as_i64(&rv)?);
                    Ok(json!(match op {
                        BinOp::Lt => a <  b,
                        BinOp::Le => a <= b,
                        BinOp::Gt => a >  b,
                        _         => a >= b,
                    }))
                }
                BinOp::Add | BinOp::Sub | BinOp::Mul | BinOp::Div | BinOp::Mod => {
                    let (a, b) = (as_i64(&lv)?, as_i64(&rv)?);
                    if b == 0 && matches!(op, BinOp::Div | BinOp::Mod) {
                        return Err(MethodError::Exec("division by zero".into()));
                    }
                    Ok(json!(match op {
                        BinOp::Add => a + b,
                        BinOp::Sub => a - b,
                        BinOp::Mul => a * b,
                        BinOp::Div => a / b,
                        _          => a % b,
                    }))
                }
                BinOp::And | BinOp::Or => unreachable!("handled above"),
            }
        }
    }
}

fn as_i64(v: &Value) -> Result<i64, MethodError> {
    v.as_i64().ok_or_else(|| MethodError::Exec(format!("expected a number, got {v}")))
}

/// Non-equality select predicate over a row column. Numbers compare
/// numerically, strings lexicographically (covers `at > "2026-…"`);
/// mismatched or missing values fail the predicate rather than erroring —
/// a filter, not an expression.
fn pred_holds(actual: Option<&Value>, op: BinOp, wanted: &Value) -> bool {
    let Some(a) = actual else { return op == BinOp::Ne && !wanted.is_null() };
    match op {
        BinOp::Ne => a != wanted,
        BinOp::Lt | BinOp::Le | BinOp::Gt | BinOp::Ge => {
            let ord = match (a.as_i64(), wanted.as_i64()) {
                (Some(x), Some(y)) => x.cmp(&y),
                _ => match (a.as_str(), wanted.as_str()) {
                    (Some(x), Some(y)) => x.cmp(y),
                    _ => return false,
                },
            };
            match op {
                BinOp::Lt => ord.is_lt(),
                BinOp::Le => ord.is_le(),
                BinOp::Gt => ord.is_gt(),
                _         => ord.is_ge(),
            }
        }
        _ => unreachable!("equality predicates go through find_by"),
    }
}

/// If `field` names a relation on the receiving type, materialize it:
/// `multi T` / `backlink T.f` → array of the related rows, in id order.
/// Returns `Ok(None)` when `field` is not a relation (plain column access).
///
/// Shard note: the scan runs on the executing (owner) shard only. Related
/// rows created BY methods land here too (creates mint locally on the shard
/// that runs the method — `set_price`'s Price is by construction visible to
/// the same product's `current_price`). Cross-shard relation reads are 09d/
/// 09e territory.
fn relation_rows(cx: &mut Cx, field: &str) -> Result<Option<Value>, MethodError> {
    let Some(t) = cx.e.catalog().get(cx.self_ty) else { return Ok(None) };
    let Some(f) = t.fields.iter().find(|f| f.name == field && f.is_relation) else {
        return Ok(None);
    };
    let (target, link_field) = match &f.ty {
        FieldTy::MultiEdge { target, .. } | FieldTy::MultiVia { target, .. } => {
            // Find the ref column in the target type that points back at us.
            let tt = cx.e.catalog().get(target).ok_or_else(||
                MethodError::Exec(format!("relation `{field}`: unknown type {target}")))?;
            let mut backrefs = tt.fields.iter().filter(|g|
                matches!(&g.ty, FieldTy::Ref(r) if r == cx.self_ty));
            let Some(link) = backrefs.next() else {
                return Err(MethodError::Exec(format!(
                    "relation `{field}`: {target} has no `ref {}` field", cx.self_ty)));
            };
            if backrefs.next().is_some() {
                return Err(MethodError::Exec(format!(
                    "relation `{field}`: {target} has multiple refs to {} — ambiguous", cx.self_ty)));
            }
            (target.clone(), link.name.clone())
        }
        FieldTy::Backlink { target, field: link } => (target.clone(), link.clone()),
        _ => return Ok(None),   // `ref T` is a stored scalar column — plain access
    };

    let self_id = cx.self_row.get("id").cloned().unwrap_or(Value::Null);
    // Index-accelerated when the target declares @table(index: [<link>, …]);
    // find_by falls back to a scan otherwise.
    let rows = cx.e.find_by(&target, &[(link_field, self_id)])
        .map_err(|e| MethodError::Exec(e.to_string()))?
        .into_iter()
        .map(Value::Object)
        .collect::<Vec<_>>();
    Ok(Some(Value::Array(rows)))
}

/// Built-in functions available in method bodies.
fn builtin(name: &str, mut args: Vec<Value>) -> Result<Value, MethodError> {
    match (name, args.len()) {
        // `latest(set)` — the row with the highest id. Prices are
        // append-only, so highest id = most recently inserted (per shard).
        ("latest", 1) => {
            let Value::Array(items) = args.remove(0) else {
                return Err(MethodError::Exec("latest(): expected a set".into()));
            };
            items.into_iter()
                .max_by_key(|v| v.get("id").and_then(|i| i.as_i64()).unwrap_or(i64::MIN))
                .ok_or_else(|| MethodError::Exec("latest(): empty set".into()))
        }
        ("count", 1) => {
            match &args[0] {
                Value::Array(items) => Ok(json!(items.len() as i64)),
                other => Err(MethodError::Exec(format!("count(): expected a set, got {other}"))),
            }
        }
        ("now", 0) => Ok(Value::String(crate::engine::now_iso8601())),
        _ => Err(MethodError::Exec(format!(
            "unknown function `{name}`/{} (built-ins: latest, count, now)", args.len()))),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::compile::Catalog;
    use crate::parser::parse;

    const PRICING: &str = r#"
@table(name: "prices", index: [product, at])
class Price {
  id:       Id
  product:  ref Product
  amount:   Money
  currency: Text = "EUR"
  at:       Text = "t0"

  fn discounted(pct: Int) -> Money {
    return self.amount * (100 - pct) / 100;
  }
}

class Product {
  id:     Id
  sku:    SKU @unique
  name:   Text
  prices: multi Price

  fn current_price() -> Money in txn {
    return latest(self.prices).amount;
  }

  fn set_price(amount: Money) in txn {
    assert amount > 0 otherwise abort "price must be positive"
    insert Price { product: self.id, amount: amount };
  }

  fn history() -> [Money] in txn {
    return select Price{ product == self.id, amount };
  }

  service rest "/api/products" expose list, get, create
}
"#;

    fn engine() -> Engine {
        let cat = Catalog::from_schemas(vec![parse(PRICING).unwrap()]).unwrap();
        Engine::new(cat)
    }

    fn method<'a>(e: &'a Engine, ty: &str, name: &str) -> MethodDecl {
        e.catalog().get(ty).unwrap().methods.iter()
            .find(|m| m.name == name).unwrap().clone()
    }

    fn args(v: Value) -> Map<String, Value> {
        match v { Value::Object(m) => m, _ => Map::new() }
    }

    #[test]
    fn set_price_inserts_and_current_price_reads_it_back() {
        let mut e = engine();
        let p = e.create("Product", json!({"sku": "SKU-1", "name": "Gadget"})).unwrap();
        let id = p["id"].as_i64().unwrap();

        let set = method(&e, "Product", "set_price");
        call(&mut e, "Product", id, &set, &args(json!({"amount": 4999}))).unwrap();
        call(&mut e, "Product", id, &set, &args(json!({"amount": 5999}))).unwrap();

        let prices = e.list("Price").unwrap();
        assert_eq!(prices.len(), 2);
        assert_eq!(prices[0]["product"].as_i64().unwrap(), id);
        assert_eq!(prices[0]["currency"], "EUR");   // default seeded by create

        let cur = method(&e, "Product", "current_price");
        let v = call(&mut e, "Product", id, &cur, &Map::new()).unwrap();
        assert_eq!(v, json!(5999));
    }

    #[test]
    fn select_expression_filters_and_projects() {
        let mut e = engine();
        let p1 = e.create("Product", json!({"sku": "A", "name": "A"})).unwrap();
        let p2 = e.create("Product", json!({"sku": "B", "name": "B"})).unwrap();
        let (id1, id2) = (p1["id"].as_i64().unwrap(), p2["id"].as_i64().unwrap());

        let set = method(&e, "Product", "set_price");
        call(&mut e, "Product", id1, &set, &args(json!({"amount": 100}))).unwrap();
        call(&mut e, "Product", id1, &set, &args(json!({"amount": 200}))).unwrap();
        call(&mut e, "Product", id2, &set, &args(json!({"amount": 999}))).unwrap();

        // `history` = select Price{ product == self.id, amount } — the
        // equality predicate rides the (product, at) index; the projection
        // shapes each row down to { amount }.
        let hist = method(&e, "Product", "history");
        let v = call(&mut e, "Product", id1, &hist, &Map::new()).unwrap();
        assert_eq!(v, json!([{"amount": 100}, {"amount": 200}]));

        // Indexed relation read (self.prices) equals the select's row set.
        let cur = method(&e, "Product", "current_price");
        assert_eq!(call(&mut e, "Product", id1, &cur, &Map::new()).unwrap(), json!(200));
        assert_eq!(call(&mut e, "Product", id2, &cur, &Map::new()).unwrap(), json!(999));
    }

    #[test]
    fn pure_method_computes_from_self() {
        let mut e = engine();
        let p = e.create("Product", json!({"sku": "S", "name": "N"})).unwrap();
        let pid = p["id"].as_i64().unwrap();
        let set = method(&e, "Product", "set_price");
        call(&mut e, "Product", pid, &set, &args(json!({"amount": 1000}))).unwrap();

        let price_id = e.list("Price").unwrap()[0]["id"].as_i64().unwrap();
        let disc = method(&e, "Price", "discounted");
        let v = call(&mut e, "Price", price_id, &disc, &args(json!({"pct": 25}))).unwrap();
        assert_eq!(v, json!(750));
    }

    #[test]
    fn abort_rolls_back_completely() {
        let mut e = engine();
        let p = e.create("Product", json!({"sku": "S", "name": "N"})).unwrap();
        let id = p["id"].as_i64().unwrap();
        let set = method(&e, "Product", "set_price");

        // amount <= 0 trips the assert AFTER nothing, but build a stronger
        // case: a method that inserts and THEN aborts must leave no row.
        let src = r#"
class Product {
  id: Id
  fn bad(amount: Money) in txn {
    insert Price { product: self.id, amount: amount };
    assert false otherwise abort "always"
  }
}
"#;
        let sch  = parse(src).unwrap();
        let bad  = sch.types[0].methods[0].clone();

        let err = call(&mut e, "Product", id, &bad, &args(json!({"amount": 1}))).unwrap_err();
        assert!(matches!(err, MethodError::Abort(ref m) if m == "always"));
        assert_eq!(e.list("Price").unwrap().len(), 0, "aborted insert must roll back");

        // The plain assert path also rejects without side effects.
        let err = call(&mut e, "Product", id, &set, &args(json!({"amount": 0}))).unwrap_err();
        assert!(matches!(err, MethodError::Abort(_)));
        assert_eq!(e.list("Price").unwrap().len(), 0);
    }

    #[test]
    fn missing_row_and_missing_arg_are_typed_errors() {
        let mut e = engine();
        let set = method(&e, "Product", "set_price");
        assert!(matches!(
            call(&mut e, "Product", 999, &set, &args(json!({"amount": 1}))),
            Err(MethodError::NoSuchRow)));

        let p = e.create("Product", json!({"sku": "S", "name": "N"})).unwrap();
        let id = p["id"].as_i64().unwrap();
        assert!(matches!(
            call(&mut e, "Product", id, &set, &Map::new()),
            Err(MethodError::BadArgs(_))));
    }

    #[test]
    fn method_commit_is_one_atomic_wal_frame() {
        use crate::wal::Wal;
        let path = std::env::temp_dir()
            .join(format!("wo-method-wal-{}", std::process::id()));
        let _ = std::fs::remove_file(&path);

        let cat = Catalog::from_schemas(vec![parse(PRICING).unwrap()]).unwrap();
        let id;
        {
            let mut e = Engine::new(cat.clone());
            let (wal, n) = Wal::open_and_replay(&path, &mut e).unwrap();
            assert_eq!(n, 0);
            e.attach_wal(wal);
            let p = e.create("Product", json!({"sku": "S", "name": "N"})).unwrap();
            id = p["id"].as_i64().unwrap();
            let set = method(&e, "Product", "set_price");
            call(&mut e, "Product", id, &set, &args(json!({"amount": 4999}))).unwrap();
        }
        // Recovery: the create frame + ONE txn frame replay into a fresh engine.
        let mut e = Engine::new(cat);
        let (_, n) = Wal::open_and_replay(&path, &mut e).unwrap();
        assert_eq!(n, 2, "one create frame + one method-txn frame");
        let prices = e.list("Price").unwrap();
        assert_eq!(prices.len(), 1);
        assert_eq!(prices[0]["amount"], json!(4999));

        let cur = method(&e, "Product", "current_price");
        let v = call(&mut e, "Product", id, &cur, &Map::new()).unwrap();
        assert_eq!(v, json!(4999));
        let _ = std::fs::remove_file(&path);
    }
}
