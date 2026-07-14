//! PostgreSQL backup mirror — plan 16b (`docs/plan/16-postgres-mirror.md`).
//!
//! RAM is authoritative; this module is the **backup mechanism**: every
//! committed mutation is cloned onto an mpsc channel by the shard engines
//! (after the WAL made it durable — never before, never gating the ack) and
//! a single dedicated `wo-pg` thread drains the channel into Postgres as
//! JSONB upserts. Reads never touch Postgres.
//!
//! Failure doctrine (16b): Postgres being down costs clients nothing — the
//! thread reconnects with capped backoff while the bounded channel absorbs
//! the burst; if the channel fills, records are dropped **loudly** (counted
//! and logged). Lossless catch-up (dirty-flag full resync) is plan 16d;
//! restore-from-Postgres at boot is 16e.
//!
//! Schema (16b): one table per type — `"<storage_name>" (id BIGINT PRIMARY
//! KEY, row JSONB NOT NULL)` — the `@table(name: "prices")` annotation
//! names the table. Typed-column projection is 16c.

use std::sync::mpsc::{Receiver, RecvTimeoutError, SyncSender};
use std::time::Duration;

use serde_json::Value;

use crate::engine::Row;
use crate::pg::{escape_ident, escape_literal, Conn, PgConfig};

/// Mirror channel capacity. At ~200 bytes/record this bounds the buffered
/// backlog around a few tens of MB — enough to ride out a Postgres restart
/// under load without threatening the RAM budget.
pub const QUEUE_CAP: usize = 65_536;

/// One committed mutation, as the mirror needs it. Unlike `WalRec::Update`
/// (which carries the merge body), `Upsert` always carries the FULL
/// post-merge row — the mirror's `ON CONFLICT ... DO UPDATE` replaces the
/// whole JSONB value.
#[derive(Debug, Clone)]
pub enum MirrorRec {
    Upsert { ty: String, id: i64, row: Row },
    Delete { ty: String, id: i64 },
    /// One method transaction (plan 13b) — applied inside one Postgres
    /// transaction, mirroring the WAL's atomic `WalRec::Txn` frame.
    Txn(Vec<MirrorRec>),
}

pub type MirrorSender = SyncSender<MirrorRec>;

/// Spawn the `wo-pg` mirror thread. `tables` maps type name → storage
/// (table) name for every catalog type; DDL is bootstrapped on every
/// (re)connect so a fresh database works out of the box.
pub fn spawn(
    cfg:    PgConfig,
    rx:     Receiver<MirrorRec>,
    tables: Vec<(String, String)>,
) -> std::thread::JoinHandle<()> {
    std::thread::Builder::new()
        .name("wo-pg".into())
        .spawn(move || run(cfg, rx, tables))
        .expect("spawn wo-pg mirror thread")
}

fn run(cfg: PgConfig, rx: Receiver<MirrorRec>, tables: Vec<(String, String)>) {
    let mut dropped: u64 = 0;
    loop {
        // (Re)connect with capped backoff, bootstrapping DDL each time.
        let Some(mut c) = connect_with_backoff(&cfg, &tables, &rx, &mut dropped) else {
            return;   // channel closed — shutdown
        };

        eprintln!("[wo] pg mirror: connected to {}:{}/{} ({} tables)",
                  cfg.host, cfg.port, cfg.database, tables.len());
        if dropped > 0 {
            eprintln!("[wo] pg mirror: WARNING — {dropped} records were dropped while \
                       disconnected; Postgres is behind RAM until a resync (plan 16d)");
        }

        // Drain loop: batch what's queued, one round-trip per batch.
        loop {
            let batch = match next_batch(&rx) {
                Some(b) => b,
                None    => return,                        // senders gone — shutdown
            };
            if let Err(e) = apply_batch(&mut c, &tables, &batch) {
                eprintln!("[wo] pg mirror: connection lost ({e}) — reconnecting");
                break;                                    // outer loop reconnects
            }
        }
    }
}

/// Block for the next record, then opportunistically drain up to a batch.
/// `None` = all senders dropped (process shutting down).
fn next_batch(rx: &Receiver<MirrorRec>) -> Option<Vec<MirrorRec>> {
    const BATCH: usize = 512;
    let first = rx.recv().ok()?;
    let mut batch = vec![first];
    while batch.len() < BATCH {
        match rx.try_recv() {
            Ok(rec) => batch.push(rec),
            Err(_)  => break,
        }
    }
    Some(batch)
}

/// `None` = every sender is gone (process shutting down).
fn connect_with_backoff(
    cfg:     &PgConfig,
    tables:  &[(String, String)],
    rx:      &Receiver<MirrorRec>,
    dropped: &mut u64,
) -> Option<Conn> {
    let mut delay = Duration::from_millis(200);
    loop {
        match Conn::connect(cfg) {
            Ok(mut c) => match bootstrap_ddl(&mut c, tables) {
                Ok(())  => return Some(c),
                Err(e)  => eprintln!("[wo] pg mirror: DDL bootstrap failed ({e}) — retrying"),
            },
            Err(e) => eprintln!("[wo] pg mirror: connect failed ({e}) — retrying in {delay:?}"),
        }
        // While waiting, keep the channel from silently backing up forever:
        // absorb what we can into the void, counting the loss (16b policy —
        // 16d replaces this with dirty-flag resync).
        let wait_until = std::time::Instant::now() + delay;
        loop {
            let left = wait_until.saturating_duration_since(std::time::Instant::now());
            if left.is_zero() { break; }
            match rx.recv_timeout(left.min(Duration::from_millis(100))) {
                Ok(_)  => { *dropped += 1; }
                Err(RecvTimeoutError::Timeout)      => {}
                Err(RecvTimeoutError::Disconnected) => return None,
            }
        }
        delay = (delay * 2).min(Duration::from_secs(5));
    }
}

fn bootstrap_ddl(c: &mut Conn, tables: &[(String, String)]) -> Result<(), crate::pg::PgError> {
    for (_, storage) in tables {
        c.simple_query(&format!(
            "CREATE TABLE IF NOT EXISTS {} (id BIGINT PRIMARY KEY, row JSONB NOT NULL)",
            escape_ident(storage)))?;
    }
    Ok(())
}

/// Apply one batch. Statement/data errors are isolated per record and
/// logged (the batch continues); only I/O errors propagate (→ reconnect).
fn apply_batch(
    c:      &mut Conn,
    tables: &[(String, String)],
    batch:  &[MirrorRec],
) -> Result<(), crate::pg::PgError> {
    for rec in batch {
        let sql = rec_sql(tables, rec);
        match c.simple_query(&sql) {
            Ok(_) => {}
            Err(e) if e.severity == "CLIENT" => return Err(e),   // socket-level: reconnect
            Err(e) => eprintln!("[wo] pg mirror: statement rejected ({e}) — record skipped"),
        }
    }
    Ok(())
}

/// Render one record as SQL. A `Txn` becomes BEGIN; …; COMMIT in a single
/// simple-query message — atomic on the Postgres side like its WAL frame.
fn rec_sql(tables: &[(String, String)], rec: &MirrorRec) -> String {
    match rec {
        MirrorRec::Upsert { ty, id, row } => {
            let table = storage_for(tables, ty);
            let json = serde_json::to_string(&Value::Object(row.clone()))
                .unwrap_or_else(|_| "{}".into());
            format!(
                "INSERT INTO {} (id, row) VALUES ({}, {}::jsonb) \
                 ON CONFLICT (id) DO UPDATE SET row = EXCLUDED.row",
                escape_ident(table), id, escape_literal(&json))
        }
        MirrorRec::Delete { ty, id } => {
            format!("DELETE FROM {} WHERE id = {}", escape_ident(storage_for(tables, ty)), id)
        }
        MirrorRec::Txn(recs) => {
            let mut sql = String::from("BEGIN");
            for r in recs {
                sql.push_str("; ");
                sql.push_str(&rec_sql(tables, r));
            }
            sql.push_str("; COMMIT");
            sql
        }
    }
}

fn storage_for<'a>(tables: &'a [(String, String)], ty: &'a str) -> &'a str {
    tables.iter()
        .find(|(t, _)| t == ty)
        .map(|(_, s)| s.as_str())
        .unwrap_or(ty)
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn row(v: Value) -> Row {
        match v { Value::Object(m) => m, _ => panic!() }
    }

    #[test]
    fn sql_rendering_upsert_delete_txn() {
        let tables = vec![("Price".to_string(), "prices".to_string())];
        let up = MirrorRec::Upsert {
            ty: "Price".into(), id: 2,
            row: row(json!({"amount": 4999, "note": "it's"})),
        };
        let sql = rec_sql(&tables, &up);
        assert!(sql.starts_with(r#"INSERT INTO "prices" (id, row) VALUES (2, '{"#), "{sql}");
        assert!(sql.contains("''s"), "quote must be doubled: {sql}");
        assert!(sql.ends_with("ON CONFLICT (id) DO UPDATE SET row = EXCLUDED.row"));

        let del = MirrorRec::Delete { ty: "Price".into(), id: 7 };
        assert_eq!(rec_sql(&tables, &del), r#"DELETE FROM "prices" WHERE id = 7"#);

        // Unmapped type falls back to the type name.
        let other = MirrorRec::Delete { ty: "Ghost".into(), id: 1 };
        assert_eq!(rec_sql(&tables, &other), r#"DELETE FROM "Ghost" WHERE id = 1"#);

        let txn = MirrorRec::Txn(vec![up, del]);
        let sql = rec_sql(&tables, &txn);
        assert!(sql.starts_with("BEGIN; "));
        assert!(sql.ends_with("; COMMIT"));
    }

    /// Integration: full pipeline against a live server (WO_PG_TEST gated).
    #[test]
    fn mirror_pipeline_against_live_server() {
        let Ok(url) = std::env::var("WO_PG_TEST") else {
            eprintln!("mirror_pipeline: skipped (set WO_PG_TEST=postgres://... to run)");
            return;
        };
        let cfg = PgConfig::from_url(&url).unwrap();
        {
            let mut c = Conn::connect(&cfg).unwrap();
            c.simple_query(r#"DROP TABLE IF EXISTS "mirror_prices""#).unwrap();
        }

        let tables = vec![("Price".to_string(), "mirror_prices".to_string())];
        let (tx, rx) = std::sync::mpsc::sync_channel(QUEUE_CAP);
        let handle = spawn(cfg.clone(), rx, tables);

        tx.send(MirrorRec::Upsert {
            ty: "Price".into(), id: 1, row: row(json!({"amount": 100})),
        }).unwrap();
        tx.send(MirrorRec::Txn(vec![
            MirrorRec::Upsert { ty: "Price".into(), id: 3, row: row(json!({"amount": 300})) },
            MirrorRec::Upsert { ty: "Price".into(), id: 1, row: row(json!({"amount": 150})) },
        ])).unwrap();
        tx.send(MirrorRec::Delete { ty: "Price".into(), id: 3 }).unwrap();
        drop(tx);                       // close channel → thread drains and exits
        handle.join().unwrap();

        let mut c = Conn::connect(&cfg).unwrap();
        let r = c.simple_query(
            r#"SELECT id, row->>'amount' FROM "mirror_prices" ORDER BY id"#).unwrap();
        assert_eq!(r.rows.len(), 1, "id 3 deleted, id 1 remains: {:?}", r.rows);
        assert_eq!(r.rows[0][0].as_deref(), Some("1"));
        assert_eq!(r.rows[0][1].as_deref(), Some("150"), "txn upsert must have applied");
        c.simple_query(r#"DROP TABLE "mirror_prices""#).unwrap();
    }
}
