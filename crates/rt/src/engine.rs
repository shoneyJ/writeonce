//! In-memory CRUD engine for the compiled schema.
//!
//! Storage model: `HashMap<type_name, BTreeMap<id, row>>`. Rows are
//! `serde_json::Value::Object`. `Id` columns are auto-populated on insert.
//! All queries are plain iteration — fine for Stage 2.

use crate::ast::{DefaultExpr, FieldTy};
use crate::compile::{Catalog, CompiledType};

use anyhow::Result;
use serde_json::{json, Map, Value};
use std::collections::{BTreeMap, BTreeSet};
use std::time::{SystemTime, UNIX_EPOCH};

pub type Row = Map<String, Value>;

/// Comparable encoding of an indexed column value — the key space of the
/// secondary indexes (`@table(index: [...])`). Ordering: Null < Bool < Int
/// < Str, then natural order within each. Residual filters always re-check
/// with real JSON equality, so encoding collisions cannot produce wrong
/// results — only wasted candidates.
#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord)]
enum IndexKey {
    Null,
    Bool(bool),
    Int(i64),
    Str(String),
}

impl IndexKey {
    fn from_value(v: Option<&Value>) -> IndexKey {
        match v {
            None | Some(Value::Null) => IndexKey::Null,
            Some(Value::Bool(b))     => IndexKey::Bool(*b),
            Some(other) => match other.as_i64() {
                Some(n) => IndexKey::Int(n),
                None => match other {
                    Value::String(s) => IndexKey::Str(s.clone()),
                    v                => IndexKey::Str(v.to_string()),
                },
            },
        }
    }
}

/// One composite secondary index: ordered key tuples → row ids. Per-shard,
/// in RAM, maintained incrementally by [`Engine::row_insert`]/[`row_remove`].
#[derive(Debug)]
struct Index {
    cols: Vec<String>,
    map:  BTreeMap<Vec<IndexKey>, BTreeSet<i64>>,
}

impl Index {
    fn key_for(&self, row: &Row) -> Vec<IndexKey> {
        self.cols.iter().map(|c| IndexKey::from_value(row.get(c))).collect()
    }
}

#[derive(Debug, Default)]
pub struct Engine {
    catalog: Catalog,
    /// type_name → { id → row }
    tables:  std::collections::HashMap<String, BTreeMap<i64, Row>>,
    /// type_name → its secondary indexes (`@table(index: [...])`). Only
    /// mutated by `row_insert`/`row_remove` — every table mutation path
    /// (CRUD, replay, txn undo) goes through those two helpers.
    indexes: std::collections::HashMap<String, Vec<Index>>,
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
    /// Active method transaction (plan 13b): mutations defer their WAL
    /// records here and journal undo entries; `commit_txn` emits one
    /// [`WalRec::Txn`] frame, `abort_txn` reverts RAM in reverse order.
    txn: Option<TxnState>,
}

#[derive(Debug, Default)]
struct TxnState {
    wal:  Vec<crate::wal::WalRec>,
    undo: Vec<Undo>,
}

/// Inverse of one applied mutation — enough to restore the pre-txn RAM state.
#[derive(Debug)]
enum Undo {
    Created { ty: String, id: i64 },
    Updated { ty: String, id: i64, prev: Row },
    Deleted { ty: String, id: i64, row: Row },
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
        let mut tables  = std::collections::HashMap::new();
        let mut next_id = std::collections::HashMap::new();
        let mut indexes = std::collections::HashMap::new();
        for name in catalog.order.iter() {
            tables.insert(name.clone(), BTreeMap::new());
            next_id.insert(name.clone(), shard as i64 + 1);
            let t = catalog.get(name).expect("type present");
            if !t.indexes.is_empty() {
                indexes.insert(name.clone(), t.indexes.iter().map(|cols| Index {
                    cols: cols.clone(),
                    map:  BTreeMap::new(),
                }).collect());
            }
        }
        Self { catalog, tables, indexes, next_id, id_step: n_shards.max(1) as i64,
               wal: None, staged: false, txn: None }
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
                if self.tables.contains_key(ty) {
                    self.row_insert(ty, id, row.clone());
                    let step = self.id_step;
                    let counter = self.next_id.entry(ty.clone()).or_insert(1);
                    while *counter <= id { *counter += step; }
                }
            }
            WalRec::Update { ty, id, body } => {
                // Remove-then-insert keeps the secondary indexes in step.
                let Some(mut row) = self.row_remove(ty, *id) else { return };
                if let Value::Object(input) = body {
                    for (k, v) in input {
                        if k != "id" { row.insert(k.clone(), v.clone()); }
                    }
                }
                self.row_insert(ty, *id, row);
            }
            WalRec::Delete { ty, id } => {
                self.row_remove(ty, *id);
            }
            // A method's mutations — the frame validated whole, apply all.
            WalRec::Txn { recs } => {
                for r in recs { self.replay(r); }
            }
        }
    }

    // --- method transactions (plan 13b) ---

    /// Enter method-transaction mode: subsequent mutations journal undo
    /// entries and defer their WAL records until [`commit_txn`].
    pub fn begin_txn(&mut self) -> Result<()> {
        if self.txn.is_some() {
            anyhow::bail!("nested method transactions are not supported");
        }
        self.txn = Some(TxnState::default());
        Ok(())
    }

    /// Commit the active transaction: every deferred record leaves as ONE
    /// `WalRec::Txn` frame (atomic on replay). `Err` means the WAL rejected
    /// the frame — RAM has been rolled back and the caller must not ack.
    pub fn commit_txn(&mut self) -> Result<()> {
        let Some(t) = self.txn.take() else {
            anyhow::bail!("commit_txn without begin_txn");
        };
        if t.wal.is_empty() { return Ok(()); }   // read-only method — nothing to log
        if let Err(e) = self.wal_log(crate::wal::WalRec::Txn { recs: t.wal }) {
            self.apply_undo(t.undo);             // never ack non-durable
            return Err(e);
        }
        Ok(())
    }

    /// Abort the active transaction: revert RAM in reverse order, log nothing.
    pub fn abort_txn(&mut self) {
        if let Some(t) = self.txn.take() {
            self.apply_undo(t.undo);
        }
    }

    fn apply_undo(&mut self, undo: Vec<Undo>) {
        for u in undo.into_iter().rev() {
            match u {
                Undo::Created { ty, id } => {
                    self.row_remove(&ty, id);
                }
                Undo::Updated { ty, id, prev } | Undo::Deleted { ty, id, row: prev } => {
                    // Clear the current version's index keys (if any row is
                    // present) before restoring the previous one.
                    self.row_remove(&ty, id);
                    self.row_insert(&ty, id, prev);
                }
            }
        }
    }

    /// Make a mutation durable (per-commit) or stage it (group). `Err`
    /// means the caller must undo the RAM apply. Inside a method
    /// transaction the record is deferred instead — durability happens
    /// once, at [`commit_txn`], as a single atomic frame.
    fn wal_log(&mut self, rec: crate::wal::WalRec) -> Result<()> {
        if let Some(t) = self.txn.as_mut() {
            t.wal.push(rec);
            return Ok(());
        }
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

        self.row_insert(ty, id, row.clone());
        if let Some(t) = self.txn.as_mut() {
            t.undo.push(Undo::Created { ty: ty.into(), id });
        }
        // Dual-write order: RAM applied above, durable now, ack after return.
        if let Err(e) = self.wal_log(crate::wal::WalRec::Create { ty: ty.into(), row: row.clone() }) {
            self.row_remove(ty, id);   // never ack non-durable
            return Err(e);
        }
        Ok(row)
    }

    /// Merge-update a row. Remove-then-insert so the secondary indexes see
    /// both the old and the new key tuples.
    pub fn update(&mut self, ty: &str, id: i64, body: Value) -> Result<Option<Row>> {
        let Some(prev) = self.table(ty)?.get(&id).cloned() else { return Ok(None); };
        let mut row = prev.clone();
        if let Value::Object(input) = &body {
            for (k, v) in input {
                if k == "id" { continue; }   // don't let the client mutate the primary key
                row.insert(k.clone(), v.clone());
            }
        }
        let updated = row.clone();
        self.row_remove(ty, id);
        self.row_insert(ty, id, row);
        if let Some(t) = self.txn.as_mut() {
            t.undo.push(Undo::Updated { ty: ty.into(), id, prev: prev.clone() });
        }
        if let Err(e) = self.wal_log(crate::wal::WalRec::Update { ty: ty.into(), id, body }) {
            self.row_remove(ty, id);            // undo: never ack non-durable
            self.row_insert(ty, id, prev);
            return Err(e);
        }
        Ok(Some(updated))
    }

    pub fn delete(&mut self, ty: &str, id: i64) -> Result<bool> {
        self.table(ty)?;   // surface unknown-type as an error, not a silent false
        let Some(removed) = self.row_remove(ty, id) else { return Ok(false) };
        if let Some(t) = self.txn.as_mut() {
            t.undo.push(Undo::Deleted { ty: ty.into(), id, row: removed.clone() });
        }
        if let Err(e) = self.wal_log(crate::wal::WalRec::Delete { ty: ty.into(), id }) {
            self.row_insert(ty, id, removed);  // undo
            return Err(e);
        }
        Ok(true)
    }

    /// Equality lookup, index-accelerated. Picks the index whose leading
    /// columns form the longest prefix of the queried fields (prefix range
    /// scan on its BTreeMap); remaining predicates filter the candidates;
    /// no matching index → full scan. Results in id order. `eq` empty =
    /// plain `list`.
    pub fn find_by(&self, ty: &str, eq: &[(String, Value)]) -> Result<Vec<Row>> {
        let table = self.table(ty)?;
        if eq.is_empty() {
            return Ok(table.values().cloned().collect());
        }
        // Real-equality re-check over ALL queried fields — the index only
        // narrows candidates, it never decides membership.
        let matches = |row: &Row| eq.iter().all(|(f, v)| {
            match row.get(f) {
                Some(rv) => rv == v,
                None     => v.is_null(),
            }
        });

        let mut best: Option<(&Index, usize)> = None;
        if let Some(idxs) = self.indexes.get(ty) {
            for idx in idxs {
                let mut k = 0;
                for col in &idx.cols {
                    if eq.iter().any(|(f, _)| f == col) { k += 1; } else { break; }
                }
                if k > 0 && best.map_or(true, |(_, bk)| k > bk) {
                    best = Some((idx, k));
                }
            }
        }

        let Some((idx, k)) = best else {
            return Ok(table.values().filter(|r| matches(r)).cloned().collect());
        };
        let prefix: Vec<IndexKey> = idx.cols[..k].iter()
            .map(|c| IndexKey::from_value(eq.iter().find(|(f, _)| f == c).map(|(_, v)| v)))
            .collect();
        let mut ids: Vec<i64> = Vec::new();
        // A shorter Vec sorts before any longer Vec sharing its prefix, so
        // range(prefix..) starts exactly at the first candidate key.
        for (key, set) in idx.map.range(prefix.clone()..) {
            if key.len() < k || key[..k] != prefix[..] { break; }
            ids.extend(set.iter().copied());
        }
        ids.sort_unstable();
        Ok(ids.into_iter()
            .filter_map(|id| table.get(&id))
            .filter(|r| matches(r))
            .cloned()
            .collect())
    }

    // --- helpers ---

    fn table(&self, ty: &str) -> Result<&BTreeMap<i64, Row>> {
        self.tables.get(ty).ok_or_else(|| anyhow::anyhow!("no such type: {ty}"))
    }

    /// THE two table-mutation primitives — every path that changes a row
    /// (CRUD, WAL replay, txn undo) goes through these so the secondary
    /// indexes can never drift from the tables.
    fn row_insert(&mut self, ty: &str, id: i64, row: Row) {
        if let Some(idxs) = self.indexes.get_mut(ty) {
            for idx in idxs {
                let key = idx.key_for(&row);
                idx.map.entry(key).or_default().insert(id);
            }
        }
        if let Some(t) = self.tables.get_mut(ty) {
            t.insert(id, row);
        }
    }

    fn row_remove(&mut self, ty: &str, id: i64) -> Option<Row> {
        let row = self.tables.get_mut(ty)?.remove(&id)?;
        if let Some(idxs) = self.indexes.get_mut(ty) {
            for idx in idxs {
                let key = idx.key_for(&row);
                if let Some(set) = idx.map.get_mut(&key) {
                    set.remove(&id);
                    if set.is_empty() { idx.map.remove(&key); }
                }
            }
        }
        Some(row)
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

pub(crate) fn now_iso8601() -> String {
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

    const INDEXED: &str = r#"
@table(index: [owner, at])
type Item { id: Id
            owner: Int
            at: Text
            service rest "/api/items" expose list }
"#;

    #[test]
    fn secondary_index_tracks_create_update_delete() {
        let mut eng = engine_from(INDEXED);
        for i in 0..3 {
            eng.create("Item", json!({"owner": 1, "at": format!("t{i}")})).unwrap();
        }
        eng.create("Item", json!({"owner": 2, "at": "t9"})).unwrap();

        // prefix match (owner) and full composite (owner, at)
        let one = eng.find_by("Item", &[("owner".into(), json!(1))]).unwrap();
        assert_eq!(one.len(), 3);
        let exact = eng.find_by("Item",
            &[("owner".into(), json!(1)), ("at".into(), json!("t1"))]).unwrap();
        assert_eq!(exact.len(), 1);

        // update moves the row between index keys
        let id = exact[0]["id"].as_i64().unwrap();
        eng.update("Item", id, json!({"owner": 2})).unwrap();
        assert_eq!(eng.find_by("Item", &[("owner".into(), json!(1))]).unwrap().len(), 2);
        assert_eq!(eng.find_by("Item", &[("owner".into(), json!(2))]).unwrap().len(), 2);

        // delete clears its entries
        eng.delete("Item", id).unwrap();
        assert_eq!(eng.find_by("Item", &[("owner".into(), json!(2))]).unwrap().len(), 1);

        // non-indexed field → scan fallback, same semantics
        assert_eq!(eng.find_by("Item", &[("at".into(), json!("t0"))]).unwrap().len(), 1);

        // index answers equal scan answers (ground truth)
        let scan: Vec<_> = eng.list("Item").unwrap().into_iter()
            .filter(|r| r["owner"] == json!(1)).collect();
        assert_eq!(eng.find_by("Item", &[("owner".into(), json!(1))]).unwrap(), scan);
    }

    #[test]
    fn txn_abort_restores_index_state() {
        let mut eng = engine_from(INDEXED);
        eng.create("Item", json!({"owner": 1, "at": "a"})).unwrap();   // id 1

        eng.begin_txn().unwrap();
        eng.create("Item", json!({"owner": 1, "at": "b"})).unwrap();
        eng.update("Item", 1, json!({"owner": 5})).unwrap();
        eng.abort_txn();

        assert_eq!(eng.find_by("Item", &[("owner".into(), json!(1))]).unwrap().len(), 1);
        assert!(eng.find_by("Item", &[("owner".into(), json!(5))]).unwrap().is_empty());
        assert_eq!(eng.list("Item").unwrap().len(), 1);
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
