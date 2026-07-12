//! In-memory CRUD engine for the compiled schema.
//!
//! Storage model: `HashMap<type_name, BTreeMap<id, row>>`. Rows are
//! `serde_json::Value::Object`. `Id` columns are auto-populated on insert.
//! All queries are plain iteration — fine for Stage 2.

use crate::ast::{DefaultExpr, FieldTy};
use crate::compile::{Catalog, CompiledType};

use anyhow::Result;
use serde_json::{json, Map, Value};
use std::collections::BTreeMap;
use std::time::{SystemTime, UNIX_EPOCH};

pub type Row = Map<String, Value>;

#[derive(Debug, Default)]
pub struct Engine {
    catalog: Catalog,
    /// type_name → { id → row }
    tables:  std::collections::HashMap<String, BTreeMap<i64, Row>>,
    /// per-type id allocator
    next_id: std::collections::HashMap<String, i64>,
    /// id stride — 1 for a standalone engine, `n_shards` for a 09b shard so
    /// ids interleave (shard t mints t+1, t+1+n, …) and the owner of any id
    /// is recoverable as `(id-1) % n` with zero coordination.
    id_step: i64,
    /// Per-shard write-ahead log (plan 09c). `PerCommit` fsyncs inside the
    /// mutating call (simple, used by tests); `Group` stages frames and
    /// parks acks for the worker's per-tick io_uring flush — the C
    /// prototype's phase-D group commit.
    wal: Option<WalBackend>,
    /// Set when the last mutation staged a group-commit frame — the caller
    /// (handler or shard job) must park its ack. Cleared by `take_staged`.
    staged: bool,
}

#[derive(Debug)]
enum WalBackend {
    PerCommit(crate::wal::Wal),
    Group(crate::wal::WalGroup),
}

impl Engine {
    pub fn new(catalog: Catalog) -> Self {
        Self::for_shard(catalog, 0, 1)
    }

    /// One shard of a thread-per-core deployment (plan 09b): same engine,
    /// interleaved id minting.
    pub fn for_shard(catalog: Catalog, shard: usize, n_shards: usize) -> Self {
        let mut tables = std::collections::HashMap::new();
        let mut next_id = std::collections::HashMap::new();
        for name in catalog.order.iter() {
            tables.insert(name.clone(), BTreeMap::new());
            next_id.insert(name.clone(), shard as i64 + 1);
        }
        Self { catalog, tables, next_id, id_step: n_shards.max(1) as i64, wal: None, staged: false }
    }

    /// Attach a per-commit WAL (fsync inside each mutation). Must happen
    /// AFTER replay — replayed mutations must not be re-logged.
    pub fn attach_wal(&mut self, wal: crate::wal::Wal) {
        self.wal = Some(WalBackend::PerCommit(wal));
    }

    /// Attach a group-commit WAL (io_uring): mutations stage frames; the
    /// worker flushes once per tick and releases parked acks on the CQE.
    pub fn attach_wal_group(&mut self, wal: crate::wal::WalGroup) {
        self.wal = Some(WalBackend::Group(wal));
    }

    /// Did the last mutation stage a group-commit frame? (Cleared on read.)
    /// The caller must park its ack on the batch when this is true.
    pub fn take_staged(&mut self) -> bool {
        std::mem::take(&mut self.staged)
    }

    /// Park a cross-shard reply on the active batch — sent on fsync.
    pub fn park_reply(&mut self, cb: Box<dyn FnOnce() + Send>) {
        match self.wal.as_mut() {
            Some(WalBackend::Group(g)) => g.park(crate::wal::Parked::Reply(cb)),
            _ => cb(),   // no group WAL: durability already settled (or off)
        }
    }

    /// Park a local connection's response on the active batch.
    pub fn park_conn(&mut self, fd: std::os::unix::io::RawFd, gen: u64) {
        if let Some(WalBackend::Group(g)) = self.wal.as_mut() {
            g.park(crate::wal::Parked::Conn { fd, gen });
        }
    }

    /// Worker hooks — flush at tick end; reap on ring-fd readable.
    pub fn wal_flush(&mut self) {
        if let Some(WalBackend::Group(g)) = self.wal.as_mut() {
            if let Err(e) = g.flush() { eprintln!("[wo] wal flush: {e}"); }
        }
    }

    pub fn wal_complete(&mut self) -> Option<(bool, Vec<crate::wal::Parked>)> {
        match self.wal.as_mut() {
            Some(WalBackend::Group(g)) => g.complete(),
            _ => None,
        }
    }

    pub fn wal_ring_fd(&self) -> Option<std::os::unix::io::RawFd> {
        match self.wal.as_ref() {
            Some(WalBackend::Group(g)) => Some(g.ring_fd()),
            _ => None,
        }
    }

    /// Apply one replayed WAL record. Bypasses default-seeding and id
    /// minting — the log carries exact state — but advances the id
    /// high-water mark so post-recovery mints never collide.
    pub fn replay(&mut self, rec: &crate::wal::WalRec) {
        use crate::wal::WalRec;
        match rec {
            WalRec::Create { ty, row } => {
                let Some(id) = row.get("id").and_then(|v| v.as_i64()) else { return };
                if let Some(table) = self.tables.get_mut(ty) {
                    table.insert(id, row.clone());
                    let step = self.id_step;
                    let counter = self.next_id.entry(ty.clone()).or_insert(1);
                    while *counter <= id { *counter += step; }
                }
            }
            WalRec::Update { ty, id, body } => {
                if let Some(row) = self.tables.get_mut(ty).and_then(|t| t.get_mut(id)) {
                    if let Value::Object(input) = body {
                        for (k, v) in input {
                            if k != "id" { row.insert(k.clone(), v.clone()); }
                        }
                    }
                }
            }
            WalRec::Delete { ty, id } => {
                if let Some(table) = self.tables.get_mut(ty) { table.remove(id); }
            }
        }
    }

    /// Make a mutation durable (per-commit) or stage it (group). `Err`
    /// means the caller must undo the RAM apply.
    fn wal_log(&mut self, rec: crate::wal::WalRec) -> Result<()> {
        match self.wal.as_mut() {
            Some(WalBackend::PerCommit(w)) => {
                w.append(&rec).map_err(|e| anyhow::anyhow!("wal append: {e}"))?;
            }
            Some(WalBackend::Group(g)) => {
                g.stage(&rec).map_err(|e| anyhow::anyhow!("wal stage: {e}"))?;
                self.staged = true;
            }
            None => {}
        }
        Ok(())
    }

    pub fn catalog(&self) -> &Catalog { &self.catalog }

    /// List every row of `ty` in insertion (id) order.
    pub fn list(&self, ty: &str) -> Result<Vec<Row>> {
        self.table(ty).map(|t| t.values().cloned().collect())
    }

    pub fn get(&self, ty: &str, id: i64) -> Result<Option<Row>> {
        Ok(self.table(ty)?.get(&id).cloned())
    }

    /// Create a row. `body` is the JSON object from the request; missing columns
    /// fill in from defaults. Returns the finalized row (including the auto-id).
    pub fn create(&mut self, ty: &str, body: Value) -> Result<Row> {
        let t = self.compiled(ty)?.clone();
        let mut row = self.seed_defaults(&t);

        if let Value::Object(input) = body {
            for (k, v) in input {
                row.insert(k, v);
            }
        }

        // Assign id if the type declares one and the caller didn't provide.
        if t.has_id && !row.contains_key("id") {
            let id = self.mint_id(ty);
            row.insert("id".into(), json!(id));
        }

        let id = row.get("id")
            .and_then(|v| v.as_i64())
            .unwrap_or_else(|| self.mint_id(ty));
        row.insert("id".into(), json!(id));

        self.tables.get_mut(ty).unwrap().insert(id, row.clone());
        // Dual-write order: RAM applied above, durable now, ack after return.
        if let Err(e) = self.wal_log(crate::wal::WalRec::Create { ty: ty.into(), row: row.clone() }) {
            self.tables.get_mut(ty).unwrap().remove(&id);   // never ack non-durable
            return Err(e);
        }
        Ok(row)
    }

    /// Merge-update a row.
    pub fn update(&mut self, ty: &str, id: i64, body: Value) -> Result<Option<Row>> {
        let table = self.tables.get_mut(ty)
            .ok_or_else(|| anyhow::anyhow!("no such type: {ty}"))?;
        let Some(row) = table.get_mut(&id) else { return Ok(None); };
        let prev = row.clone();
        if let Value::Object(input) = &body {
            for (k, v) in input {
                if k == "id" { continue; }   // don't let the client mutate the primary key
                row.insert(k.clone(), v.clone());
            }
        }
        let updated = row.clone();
        if let Err(e) = self.wal_log(crate::wal::WalRec::Update { ty: ty.into(), id, body }) {
            self.tables.get_mut(ty).unwrap().insert(id, prev);  // undo: never ack non-durable
            return Err(e);
        }
        Ok(Some(updated))
    }

    pub fn delete(&mut self, ty: &str, id: i64) -> Result<bool> {
        let table = self.tables.get_mut(ty)
            .ok_or_else(|| anyhow::anyhow!("no such type: {ty}"))?;
        let Some(removed) = table.remove(&id) else { return Ok(false) };
        if let Err(e) = self.wal_log(crate::wal::WalRec::Delete { ty: ty.into(), id }) {
            self.tables.get_mut(ty).unwrap().insert(id, removed);  // undo
            return Err(e);
        }
        Ok(true)
    }

    // --- helpers ---

    fn table(&self, ty: &str) -> Result<&BTreeMap<i64, Row>> {
        self.tables.get(ty).ok_or_else(|| anyhow::anyhow!("no such type: {ty}"))
    }

    fn compiled(&self, ty: &str) -> Result<&CompiledType> {
        self.catalog.get(ty).ok_or_else(|| anyhow::anyhow!("no such type: {ty}"))
    }

    fn mint_id(&mut self, ty: &str) -> i64 {
        let counter = self.next_id.entry(ty.to_string()).or_insert(1);
        let id = *counter;
        *counter += self.id_step;
        id
    }

    /// Produce the initial row object — default values for every non-relation
    /// field that declared one.
    fn seed_defaults(&self, t: &CompiledType) -> Row {
        let mut row = Map::new();
        for f in &t.fields {
            if f.is_relation { continue; }
            if let Some(def) = &f.default {
                if let Some(v) = Self::eval_default(def, &f.ty) {
                    row.insert(f.name.clone(), v);
                }
            } else if matches!(f.ty, FieldTy::Array(_)) && !f.nullable {
                row.insert(f.name.clone(), Value::Array(Vec::new()));
            } else if let FieldTy::Struct(inner) = &f.ty {
                let mut nested = Map::new();
                for g in inner {
                    if let Some(d) = &g.default {
                        if let Some(v) = Self::eval_default(d, &g.ty) {
                            nested.insert(g.name.clone(), v);
                        }
                    }
                }
                if !nested.is_empty() {
                    row.insert(f.name.clone(), Value::Object(nested));
                }
            }
        }
        row
    }

    fn eval_default(def: &DefaultExpr, _ty: &FieldTy) -> Option<Value> {
        Some(match def {
            DefaultExpr::Str(s)  => Value::String(s.clone()),
            DefaultExpr::Int(n)  => json!(n),
            DefaultExpr::Bool(b) => json!(b),
            DefaultExpr::Null    => Value::Null,
            DefaultExpr::Now     => Value::String(now_iso8601()),
            DefaultExpr::Enum(s) => Value::String(s.clone()),
            // Opaque expressions (computed fields like `total = sum(...)`) are
            // Stage 2-unevaluated. Omit rather than echo parser-debug tokens.
            DefaultExpr::Opaque(_) => return None,
        })
    }
}

fn now_iso8601() -> String {
    let d = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default();
    let secs = d.as_secs() as i64;
    let nanos = d.subsec_nanos();
    // Minimal ISO-8601 UTC formatter — good enough for Stage 2.
    // (`chrono` would be nicer but we're keeping deps small.)
    let (yr, mo, da, hr, mi, se) = ymdhms(secs);
    format!("{yr:04}-{mo:02}-{da:02}T{hr:02}:{mi:02}:{se:02}.{:03}Z", nanos / 1_000_000)
}

/// Pure-Rust epoch → (year, month, day, hour, minute, second) for UTC.
fn ymdhms(mut secs: i64) -> (i32, u32, u32, u32, u32, u32) {
    const SECS_PER_DAY: i64 = 86_400;
    let se = (secs.rem_euclid(60)) as u32;
    secs = secs.div_euclid(60);
    let mi = (secs.rem_euclid(60)) as u32;
    secs = secs.div_euclid(60);
    let hr = (secs.rem_euclid(24)) as u32;
    let mut days = secs.div_euclid(24);

    // Days → calendar. Epoch 1970-01-01 is a Thursday but we don't need weekday.
    let mut year: i32 = 1970;
    loop {
        let dy = if is_leap(year) { 366 } else { 365 };
        if days >= dy {
            days -= dy;
            year += 1;
        } else {
            break;
        }
    }

    let months = if is_leap(year) {
        [31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31]
    } else {
        [31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31]
    };
    let mut mo = 1u32;
    for (i, d) in months.iter().enumerate() {
        if days < *d as i64 {
            mo = i as u32 + 1;
            break;
        }
        days -= *d as i64;
    }
    let _ = SECS_PER_DAY;
    let da = days as u32 + 1;
    (year, mo, da, hr, mi, se)
}

fn is_leap(y: i32) -> bool {
    (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::parser::parse;

    fn engine_from(src: &str) -> Engine {
        let cat = Catalog::from_schemas(vec![parse(src).unwrap()]).unwrap();
        Engine::new(cat)
    }

    #[test]
    fn crud_roundtrip_auto_id() {
        let mut eng = engine_from(r#"
type Article { id: Id
               title: Text
               published: Bool = false
               service rest "/api/articles" expose list, get, create, update, delete }
"#);
        let a = eng.create("Article", json!({"title": "Hello"})).unwrap();
        assert_eq!(a.get("id").unwrap().as_i64().unwrap(), 1);
        assert_eq!(a.get("title").unwrap().as_str().unwrap(), "Hello");
        assert_eq!(a.get("published").unwrap(), &json!(false));

        let b = eng.create("Article", json!({"title": "World", "published": true})).unwrap();
        assert_eq!(b.get("id").unwrap().as_i64().unwrap(), 2);

        let list = eng.list("Article").unwrap();
        assert_eq!(list.len(), 2);

        let got = eng.get("Article", 1).unwrap().unwrap();
        assert_eq!(got.get("title").unwrap().as_str().unwrap(), "Hello");

        let upd = eng.update("Article", 1, json!({"title": "Hi"})).unwrap().unwrap();
        assert_eq!(upd.get("title").unwrap().as_str().unwrap(), "Hi");

        assert!(eng.delete("Article", 1).unwrap());
        assert!(!eng.delete("Article", 1).unwrap());
        assert_eq!(eng.list("Article").unwrap().len(), 1);
    }
}
