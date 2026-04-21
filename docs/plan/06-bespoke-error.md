# 06 — Bespoke Error Type

**Context sources:** [`./05-hand-rolled-json.md`](./05-hand-rolled-json.md), [`../01-problem.md`](../01-problem.md).

## Goal

Replace `anyhow::Error` / `anyhow::Result` with a single `Error` enum rooted in `crates/rt/src/error.rs`. Remove `anyhow` from `crates/rt/Cargo.toml`. At the end of this phase, `[dependencies]` contains only `libc` — the **stated end goal** of this plan sequence.

## Design decisions (locked)

1. **One enum per crate.** `rt::Error` covers everything the runtime produces — parse errors, compile errors, engine errors, I/O errors, lex errors, HTTP framing errors, JSON parse errors (phase 05), signal-handler failures. The v1 crates use `std::io::Result` throughout, which is fine for I/O-heavy code but loses context for parse and compile failures. We split the difference with a tagged variant enum.
2. **`From` impls for standard errors.** `io::Error`, `ParseIntError`, `FromUtf8Error`, `Utf8Error` — automatically convert via `?`. Everything else wraps explicitly through constructors like `Error::parse(line, col, msg)`.
3. **`Display` composes a line + context.** No chained backtrace. Error messages stay compact: `parse error at line 42: expected ':', got '}'`. This is what every caller prints today after `anyhow::Error` formats.
4. **`Result<T>` alias.** Shorthand: `pub type Result<T> = core::result::Result<T, Error>`. Replaces `anyhow::Result<T>` every existing call site uses.
5. **No macros.** `anyhow::anyhow!("...")` becomes `Error::msg("...")`. `anyhow::bail!(...)` becomes `return Err(Error::msg(...))`. `anyhow::Context::context(err, "...")` becomes `err.with_context(|| "...")` via a tiny inherent method.

## Scope

### New file

| File | Responsibility | Approx LOC |
| --- | --- | --- |
| `crates/rt/src/error.rs` | `pub enum Error`, `pub type Result`, `From` impls, `Display`, `fn msg`, `fn parse`, `fn with_context` | ~120 |

### Enum shape (target)

```rust
#[derive(Debug)]
pub enum Error {
    Io(io::Error),
    Parse    { line: u32, col: u32, message: String },
    Lex      { line: u32, col: u32, message: String },
    Compile  { message: String },
    Engine   { message: String },
    Http     { message: String },
    Json     { message: String },
    NotFound { ty: String, id: i64 },
    Config   { message: String },
    Msg      (String),                  // anyhow::anyhow!-style catch-all
}

impl Error {
    pub fn msg(s: impl Into<String>) -> Self       { Error::Msg(s.into()) }
    pub fn parse(line: u32, col: u32, m: impl Into<String>) -> Self
        { Error::Parse { line, col, message: m.into() } }
    // ... one constructor per non-Io variant

    pub fn with_context<F>(self, ctx: F) -> Error
    where F: FnOnce() -> String
    {
        Error::Msg(format!("{}: {}", ctx(), self))
    }
}

impl From<io::Error>    for Error { ... }
impl From<ParseIntError> for Error { ... }
impl From<Utf8Error>    for Error { ... }
impl From<FromUtf8Error> for Error { ... }
impl core::fmt::Display for Error { ... }
impl core::error::Error for Error {}

pub type Result<T> = core::result::Result<T, Error>;
```

### Call-site mechanical sweep

Find: `rg 'anyhow::|anyhow!|bail!|\.context\(' crates/rt/src | wc -l` — expected ~40 sites.

| Current | After |
| --- | --- |
| `use anyhow::{Context, Result};` | `use crate::error::{Error, Result};` (from `rt`) |
| `fn foo() -> anyhow::Result<T>` | `fn foo() -> Result<T>` |
| `anyhow::anyhow!("no such table: {}", name)` | `Error::msg(format!("no such table: {}", name))` |
| `anyhow::bail!("...")` | `return Err(Error::msg("..."))` |
| `result.context("while doing X")?` | `result.map_err(\|e\| e.with_context(\|\| "while doing X".into()))?` |
| `Err(anyhow::anyhow!("parse error at line {line}: ..."))` | `Err(Error::parse(line, col, "..."))` |

`crates/rt/src/parser.rs` and `crates/rt/src/compile.rs` are the biggest consumers. Most edits are verbatim substitutions.

### `Cargo.toml` delta

```diff
 [dependencies]
-anyhow = "1"
 libc   = "0.2"
```

**After this phase:** `[dependencies]` has one line. The stated end goal.

## Exit criteria

1. **`cargo build`** — compiles with exactly one external dep.
2. **`cargo test --lib`** — all 14 existing `rt` tests pass. A new test in `error.rs` exercises `From<io::Error>`, `with_context`, and `Display` formatting.
3. **No `anyhow::` references anywhere in the repo.** `rg 'anyhow' crates/ docs/` returns zero hits (docs updated by this phase too).
4. **`reference/rest/blog.rest`** — 20 assertions still pass. Error paths (404, 400) still produce the same response body format (plain-text error message from the handler's `.to_string()`).
5. **`cd reference/crates && cargo build && cargo test`** unchanged. V1 doesn't use `anyhow` — nothing to touch there.
6. **`cargo tree -p rt --depth 1`** lists only `libc` as an external dep (plus transitive ones brought in by libc itself, all of which are kernel-facing).

## Non-scope

- **No custom `#[derive(Error)]` macro.** `thiserror` would be nicer but it's a dep. Hand-writing the enum + impls is ~120 lines, done once, maintained rarely.
- **No source-chain traversal.** `Error` stores messages, not source errors (except `Io` which wraps). If we need chained context later, extend the enum then — don't over-engineer now.
- **No `backtrace` crate.** If a panic-style backtrace is ever needed, `RUST_BACKTRACE=1` on `panic!()` gives it. Errors don't carry them.

## Verification

```bash
cargo build                                    # one external dep
cargo test --lib                               # 14 + error.rs test green
rg 'anyhow' crates/ docs/                      # zero hits
# full .rest smoke — same script as phase 04
cd reference/crates && cargo build && cargo test   # v1 untouched
cat crates/rt/Cargo.toml | grep -A 20 '\[dependencies\]'   # libc is the only line
```

## After this phase

`crates/rt/Cargo.toml` is at its minimum. The runtime drives every I/O operation through direct kernel primitives: `socket`, `bind`, `listen`, `accept4`, `epoll_wait`, `read`, `write`, `signalfd4`, `close`. Nothing between the code and the kernel except `libc`.

Phases 07 and 08 extend the kernel-primitive surface without adding deps — they're pure feature work on top of the foundation this sequence laid.
