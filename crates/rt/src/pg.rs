//! Hand-rolled PostgreSQL wire-protocol client — plan 16a
//! (`docs/plan/16-postgres-mirror.md`).
//!
//! The mirror's outbound half: protocol v3 over a blocking
//! `std::net::TcpStream`, zero external crates — the same doctrine as the
//! hand-rolled HTTP layer and the CRC32 in `wal.rs`. Scope is exactly what
//! the backup mirror needs:
//!
//!   * startup + auth: `trust`, `password` (cleartext), `md5`
//!     (SCRAM-SHA-256 is plan 16f)
//!   * the **simple query protocol** only (`Query` → `RowDescription` /
//!     `DataRow` / `CommandComplete` / `ErrorResponse` / `ReadyForQuery`) —
//!     no extended protocol, no prepared statements, no TLS
//!   * literal/identifier escaping for SQL the mirror generates
//!
//! Protocol reference: PostgreSQL docs “Frontend/Backend Protocol” and
//! `reference/postgresql/src/include/libpq/` (research symlink).
//!
//! Blocking I/O is deliberate: the only caller is the dedicated `wo-pg`
//! mirror thread (plan 16b) — never a shard worker.

use std::fmt;
use std::io::{self, Read, Write};
use std::net::TcpStream;
use std::time::Duration;

/// Parsed `postgres://user[:password]@host[:port]/database` URL.
/// (No percent-decoding — keep credentials URL-safe.)
#[derive(Debug, Clone)]
pub struct PgConfig {
    pub user:     String,
    pub password: Option<String>,
    pub host:     String,
    pub port:     u16,
    pub database: String,
}

impl PgConfig {
    pub fn from_url(url: &str) -> Result<PgConfig, String> {
        let rest = url.strip_prefix("postgres://")
            .or_else(|| url.strip_prefix("postgresql://"))
            .ok_or_else(|| format!("WO_PG url must start with postgres:// — got {url}"))?;
        let (userinfo, hostpart) = rest.split_once('@')
            .ok_or_else(|| "WO_PG url needs user@host".to_string())?;
        let (user, password) = match userinfo.split_once(':') {
            Some((u, p)) => (u.to_string(), Some(p.to_string())),
            None         => (userinfo.to_string(), None),
        };
        let (hostport, database) = hostpart.split_once('/')
            .ok_or_else(|| "WO_PG url needs /database".to_string())?;
        let (host, port) = match hostport.split_once(':') {
            Some((h, p)) => (h.to_string(),
                             p.parse::<u16>().map_err(|_| format!("bad port `{p}`"))?),
            None         => (hostport.to_string(), 5432),
        };
        if user.is_empty() || host.is_empty() || database.is_empty() {
            return Err(format!("incomplete WO_PG url: {url}"));
        }
        Ok(PgConfig { user, password, host, port, database: database.to_string() })
    }
}

/// A backend `ErrorResponse` (or client-side failure talking to it).
#[derive(Debug)]
pub struct PgError {
    pub severity: String,
    pub code:     String,
    pub message:  String,
}

impl fmt::Display for PgError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{} {}: {}", self.severity, self.code, self.message)
    }
}

impl PgError {
    fn client(msg: impl Into<String>) -> PgError {
        PgError { severity: "CLIENT".into(), code: "XX000".into(), message: msg.into() }
    }
}

impl From<io::Error> for PgError {
    fn from(e: io::Error) -> PgError { PgError::client(format!("io: {e}")) }
}

/// Result of one simple query (possibly multi-statement).
#[derive(Debug, Default)]
pub struct QueryResult {
    pub columns: Vec<String>,
    /// Text-format values, `None` = SQL NULL. Rows of the LAST result set.
    pub rows:    Vec<Vec<Option<String>>>,
    /// One CommandComplete tag per statement, e.g. `INSERT 0 1`.
    pub tags:    Vec<String>,
}

pub struct Conn {
    stream: TcpStream,
}

impl Conn {
    /// Connect and authenticate. Blocking, with a connect timeout.
    pub fn connect(cfg: &PgConfig) -> Result<Conn, PgError> {
        let addr = format!("{}:{}", cfg.host, cfg.port);
        let sockaddr = addr.parse()
            .map_err(|_| {
                // Not a literal ip:port — resolve via ToSocketAddrs.
                PgError::client("resolve")
            });
        let stream = match sockaddr {
            Ok(sa) => TcpStream::connect_timeout(&sa, Duration::from_secs(5))?,
            Err(_) => TcpStream::connect(&addr)?,   // DNS path
        };
        stream.set_nodelay(true).ok();
        stream.set_read_timeout(Some(Duration::from_secs(30)))?;
        stream.set_write_timeout(Some(Duration::from_secs(30)))?;
        let mut conn = Conn { stream };
        conn.startup(cfg)?;
        Ok(conn)
    }

    fn startup(&mut self, cfg: &PgConfig) -> Result<(), PgError> {
        // StartupMessage: no type byte — i32 len | i32 196608 | k\0v\0 ... \0
        let mut body = Vec::new();
        body.extend_from_slice(&196_608i32.to_be_bytes());   // protocol 3.0
        for (k, v) in [("user", cfg.user.as_str()),
                       ("database", cfg.database.as_str()),
                       ("client_encoding", "UTF8"),
                       ("application_name", "wo-pg-mirror")] {
            body.extend_from_slice(k.as_bytes()); body.push(0);
            body.extend_from_slice(v.as_bytes()); body.push(0);
        }
        body.push(0);
        let mut msg = Vec::with_capacity(body.len() + 4);
        msg.extend_from_slice(&((body.len() as i32 + 4).to_be_bytes()));
        msg.extend_from_slice(&body);
        self.stream.write_all(&msg)?;

        // Authentication exchange, then drain to ReadyForQuery.
        loop {
            let (kind, payload) = self.read_message()?;
            match kind {
                b'R' => {
                    let auth = be_i32(&payload, 0)?;
                    match auth {
                        0 => {}                                    // AuthenticationOk
                        3 => {                                     // CleartextPassword
                            let pw = cfg.password.clone().ok_or_else(||
                                PgError::client("server wants a password; none in WO_PG url"))?;
                            self.send_password(&pw)?;
                        }
                        5 => {                                     // MD5Password + 4B salt
                            let pw = cfg.password.clone().ok_or_else(||
                                PgError::client("server wants md5 auth; no password in WO_PG url"))?;
                            let salt = payload.get(4..8).ok_or_else(||
                                PgError::client("short md5 salt"))?;
                            // "md5" + md5hex(md5hex(password + user) + salt)
                            let inner = md5_hex(format!("{pw}{}", cfg.user).as_bytes());
                            let mut outer_in = inner.into_bytes();
                            outer_in.extend_from_slice(salt);
                            let digest = format!("md5{}", md5_hex(&outer_in));
                            self.send_password(&digest)?;
                        }
                        10 => return Err(PgError::client(
                            "server requires SCRAM-SHA-256 — not supported until plan 16f; \
                             configure md5/password/trust auth for the mirror role")),
                        n => return Err(PgError::client(format!("unsupported auth type {n}"))),
                    }
                }
                b'S' | b'K' | b'N' => {}     // ParameterStatus / BackendKeyData / Notice
                b'Z' => return Ok(()),       // ReadyForQuery
                b'E' => return Err(parse_error(&payload)),
                other => return Err(PgError::client(format!(
                    "unexpected message '{}' during startup", other as char))),
            }
        }
    }

    fn send_password(&mut self, pw: &str) -> Result<(), PgError> {
        let mut msg = Vec::with_capacity(pw.len() + 6);
        msg.push(b'p');
        msg.extend_from_slice(&((pw.len() as i32 + 5).to_be_bytes()));
        msg.extend_from_slice(pw.as_bytes());
        msg.push(0);
        self.stream.write_all(&msg)?;
        Ok(())
    }

    /// Run one simple query (may contain multiple `;`-separated statements —
    /// the backend wraps them in an implicit transaction). Returns the last
    /// result set + all command tags; a backend error is returned AFTER the
    /// stream is drained to ReadyForQuery, so the connection stays usable.
    pub fn simple_query(&mut self, sql: &str) -> Result<QueryResult, PgError> {
        let mut msg = Vec::with_capacity(sql.len() + 6);
        msg.push(b'Q');
        msg.extend_from_slice(&((sql.len() as i32 + 5).to_be_bytes()));
        msg.extend_from_slice(sql.as_bytes());
        msg.push(0);
        self.stream.write_all(&msg)?;

        let mut out = QueryResult::default();
        let mut err: Option<PgError> = None;
        loop {
            let (kind, payload) = self.read_message()?;
            match kind {
                b'T' => {                                    // RowDescription
                    out.columns.clear();
                    let n = be_i16(&payload, 0)? as usize;
                    let mut off = 2;
                    for _ in 0..n {
                        let name = read_cstr(&payload, off)?;
                        off += name.len() + 1 + 18;          // 4+2+4+2+4+2 fixed fields
                        out.columns.push(name);
                    }
                    out.rows.clear();                        // keep the last result set
                }
                b'D' => {                                    // DataRow
                    let n = be_i16(&payload, 0)? as usize;
                    let mut off = 2;
                    let mut row = Vec::with_capacity(n);
                    for _ in 0..n {
                        let len = be_i32(&payload, off)?;
                        off += 4;
                        if len < 0 { row.push(None); continue; }
                        let len = len as usize;
                        let bytes = payload.get(off..off + len)
                            .ok_or_else(|| PgError::client("short DataRow"))?;
                        row.push(Some(String::from_utf8_lossy(bytes).into_owned()));
                        off += len;
                    }
                    out.rows.push(row);
                }
                b'C' => out.tags.push(read_cstr(&payload, 0)?),   // CommandComplete
                b'E' => { if err.is_none() { err = Some(parse_error(&payload)); } }
                b'Z' => break,                                    // ReadyForQuery
                b'N' | b'S' | b'I' | b'G' | b'H' | b'W' => {}     // notices etc.
                other => return Err(PgError::client(format!(
                    "unexpected message '{}' in query response", other as char))),
            }
        }
        match err {
            Some(e) => Err(e),
            None    => Ok(out),
        }
    }

    /// Read one backend message: 1-byte type + i32 length (incl. itself).
    fn read_message(&mut self) -> Result<(u8, Vec<u8>), PgError> {
        let mut head = [0u8; 5];
        self.stream.read_exact(&mut head)?;
        let len = i32::from_be_bytes([head[1], head[2], head[3], head[4]]);
        if !(4..=64 * 1024 * 1024).contains(&len) {
            return Err(PgError::client(format!("bad message length {len}")));
        }
        let mut payload = vec![0u8; len as usize - 4];
        self.stream.read_exact(&mut payload)?;
        Ok((head[0], payload))
    }
}

// --- wire helpers ---

fn be_i32(b: &[u8], off: usize) -> Result<i32, PgError> {
    b.get(off..off + 4)
        .map(|s| i32::from_be_bytes(s.try_into().unwrap()))
        .ok_or_else(|| PgError::client("short message"))
}

fn be_i16(b: &[u8], off: usize) -> Result<i16, PgError> {
    b.get(off..off + 2)
        .map(|s| i16::from_be_bytes(s.try_into().unwrap()))
        .ok_or_else(|| PgError::client("short message"))
}

fn read_cstr(b: &[u8], off: usize) -> Result<String, PgError> {
    let end = b[off..].iter().position(|&c| c == 0)
        .ok_or_else(|| PgError::client("unterminated string"))?;
    Ok(String::from_utf8_lossy(&b[off..off + end]).into_owned())
}

/// ErrorResponse / NoticeResponse: (field-code byte, cstring) pairs.
fn parse_error(payload: &[u8]) -> PgError {
    let mut e = PgError { severity: "ERROR".into(), code: String::new(), message: String::new() };
    let mut off = 0;
    while off < payload.len() && payload[off] != 0 {
        let code = payload[off];
        let Ok(val) = read_cstr(payload, off + 1) else { break };
        off += 1 + val.len() + 1;
        match code {
            b'S' => e.severity = val,
            b'C' => e.code = val,
            b'M' => e.message = val,
            _ => {}
        }
    }
    e
}

// --- SQL text helpers (the mirror builds statements as text) ---

/// `'…'` literal with single quotes doubled. Standard-conforming strings
/// (the server default) treat backslashes literally, so quotes are the only
/// metacharacter.
pub fn escape_literal(s: &str) -> String {
    let mut out = String::with_capacity(s.len() + 2);
    out.push('\'');
    for c in s.chars() {
        if c == '\'' { out.push('\''); }
        out.push(c);
    }
    out.push('\'');
    out
}

/// `"…"` identifier with double quotes doubled.
pub fn escape_ident(s: &str) -> String {
    let mut out = String::with_capacity(s.len() + 2);
    out.push('"');
    for c in s.chars() {
        if c == '"' { out.push('"'); }
        out.push(c);
    }
    out.push('"');
    out
}

// --- hand-rolled MD5 (RFC 1321) — for the `md5` auth exchange only, the
// --- same no-crates spirit as the CRC32 in wal.rs. Not for new designs.

pub fn md5_hex(data: &[u8]) -> String {
    const S: [u32; 64] = [
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
    ];
    const K: [u32; 64] = [
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a,
        0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
        0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
        0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
        0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
        0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
        0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
        0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
        0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
    ];

    let mut msg = data.to_vec();
    let bit_len = (data.len() as u64).wrapping_mul(8);
    msg.push(0x80);
    while msg.len() % 64 != 56 { msg.push(0); }
    msg.extend_from_slice(&bit_len.to_le_bytes());

    let (mut a0, mut b0, mut c0, mut d0) =
        (0x6745_2301u32, 0xefcd_ab89u32, 0x98ba_dcfeu32, 0x1032_5476u32);

    for chunk in msg.chunks_exact(64) {
        let m: Vec<u32> = chunk.chunks_exact(4)
            .map(|w| u32::from_le_bytes(w.try_into().unwrap()))
            .collect();
        let (mut a, mut b, mut c, mut d) = (a0, b0, c0, d0);
        for i in 0..64 {
            let (f, g) = match i {
                0..=15  => ((b & c) | (!b & d), i),
                16..=31 => ((d & b) | (!d & c), (5 * i + 1) % 16),
                32..=47 => (b ^ c ^ d, (3 * i + 5) % 16),
                _       => (c ^ (b | !d), (7 * i) % 16),
            };
            let f2 = f.wrapping_add(a).wrapping_add(K[i]).wrapping_add(m[g]);
            a = d; d = c; c = b;
            b = b.wrapping_add(f2.rotate_left(S[i]));
        }
        a0 = a0.wrapping_add(a);
        b0 = b0.wrapping_add(b);
        c0 = c0.wrapping_add(c);
        d0 = d0.wrapping_add(d);
    }

    let mut out = String::with_capacity(32);
    for word in [a0, b0, c0, d0] {
        for byte in word.to_le_bytes() {
            out.push_str(&format!("{byte:02x}"));
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn md5_matches_rfc_vectors() {
        assert_eq!(md5_hex(b""), "d41d8cd98f00b204e9800998ecf8427e");
        assert_eq!(md5_hex(b"abc"), "900150983cd24fb0d6963f7d28e17f72");
        assert_eq!(md5_hex(b"message digest"), "f96b697d7cb7938d525a2f31aaf161d0");
        // > one block
        assert_eq!(
            md5_hex(b"12345678901234567890123456789012345678901234567890123456789012345678901234567890"),
            "57edf4a22be3c955ac49da2e2107b67a");
    }

    #[test]
    fn url_parse_covers_the_forms() {
        let c = PgConfig::from_url("postgres://wo:secret@db.example:6432/prod").unwrap();
        assert_eq!((c.user.as_str(), c.password.as_deref(), c.host.as_str(), c.port, c.database.as_str()),
                   ("wo", Some("secret"), "db.example", 6432, "prod"));
        let c = PgConfig::from_url("postgres://postgres@127.0.0.1/wo").unwrap();
        assert_eq!(c.port, 5432);
        assert!(c.password.is_none());
        assert!(PgConfig::from_url("mysql://nope@x/y").is_err());
        assert!(PgConfig::from_url("postgres://user-only-no-host").is_err());
    }

    #[test]
    fn escaping_doubles_quotes() {
        assert_eq!(escape_literal("it's"), "'it''s'");
        assert_eq!(escape_literal(r#"back\slash"#), r#"'back\slash'"#);
        assert_eq!(escape_ident(r#"we"ird"#), r#""we""ird""#);
    }

    /// Integration: needs a reachable server — set WO_PG_TEST to run, e.g.
    ///   WO_PG_TEST=postgres://postgres@127.0.0.1:54329/wo cargo test pg_
    #[test]
    fn pg_roundtrip_against_live_server() {
        let Ok(url) = std::env::var("WO_PG_TEST") else {
            eprintln!("pg_roundtrip: skipped (set WO_PG_TEST=postgres://... to run)");
            return;
        };
        let cfg = PgConfig::from_url(&url).unwrap();
        let mut c = Conn::connect(&cfg).unwrap();

        c.simple_query("DROP TABLE IF EXISTS wo_pg_smoke").unwrap();
        c.simple_query("CREATE TABLE wo_pg_smoke (id BIGINT PRIMARY KEY, row JSONB NOT NULL)").unwrap();
        c.simple_query(&format!(
            "INSERT INTO wo_pg_smoke (id, row) VALUES (1, {}::jsonb) \
             ON CONFLICT (id) DO UPDATE SET row = EXCLUDED.row",
            escape_literal(r#"{"amount":4999,"note":"it's fine"}"#))).unwrap();

        let r = c.simple_query("SELECT row->>'amount', row->>'note' FROM wo_pg_smoke").unwrap();
        assert_eq!(r.rows.len(), 1);
        assert_eq!(r.rows[0][0].as_deref(), Some("4999"));
        assert_eq!(r.rows[0][1].as_deref(), Some("it's fine"));

        // A backend error must leave the connection usable.
        assert!(c.simple_query("SELECT * FROM does_not_exist_xyz").is_err());
        let r = c.simple_query("SELECT count(*) FROM wo_pg_smoke").unwrap();
        assert_eq!(r.rows[0][0].as_deref(), Some("1"));

        // Multi-statement query = implicit transaction; both tags come back.
        let r = c.simple_query(
            "INSERT INTO wo_pg_smoke VALUES (2, '{}'::jsonb); DELETE FROM wo_pg_smoke WHERE id = 2"
        ).unwrap();
        assert_eq!(r.tags.len(), 2);
        c.simple_query("DROP TABLE wo_pg_smoke").unwrap();
    }
}
