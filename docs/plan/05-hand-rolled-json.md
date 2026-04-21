# 05 — Hand-Rolled JSON

**Context sources:** [`./04-cutover-remove-tokio-axum.md`](./04-cutover-remove-tokio-axum.md), [`../../prototypes/wo-db/src/value.hpp`](../../prototypes/wo-db/src/value.hpp).

## Goal

Replace `serde_json::Value` / `serde_json::Map` with a hand-rolled `Value` type covering exactly the shapes the runtime reads and writes: request bodies, response bodies, and the `Engine`'s in-memory `Row`. Remove `serde` + `serde_json` from `crates/rt/Cargo.toml`. After this phase the dep list is `anyhow` + `libc`.

## Design decisions (locked)

1. **Minimal surface.** The runtime's actual JSON needs are small:
   - Parse request body bytes → `Value::Object` (single top-level object on every sample endpoint).
   - Emit `Value::Object` / `Value::Array` → bytes for the response.
   - Pretty-printing is **not** required. Operators reach for `| python3 -m json.tool` if they want it.
2. **No `#[derive(Serialize/Deserialize)]`.** `Value` is the union type; every `Row`, `Product`, `Article` is already a `Value::Object` at the boundary. The only thing that "serializes" is `Value`. The ecosystem of derive-based types doesn't exist in `rt` today — `engine::Row` is `HashMap<String, Value>` via `serde_json` today, becomes `HashMap<String, Value>` via the new module tomorrow.
3. **RFC 8259 compliant, but strict.** No unquoted keys, no trailing commas, no comments. Standard JSON. The runtime isn't serving JSON5.
4. **Parser is recursive descent, zero-copy where possible.** String values borrow from the input buffer unless they contain escapes; objects own their keys. Preserves the "no heavy abstraction" pattern of phase 02 / 03.
5. **Module at `crates/rt/src/json/`.** Same "extract when a second consumer appears" rule. Eventual home is the empty [`crates/value/`](../../crates/value/) sibling — not this phase.

## Scope

### New files inside `crates/rt/src/json/`

| File | Responsibility | Approx LOC |
| --- | --- | --- |
| `mod.rs` | Re-exports `Value`, `Object`, `Array`, `parse`, `emit` | ~10 |
| `value.rs` | `pub enum Value { Null, Bool(bool), Int(i64), Float(f64), Str(String), Array(Vec<Value>), Object(BTreeMap<String, Value>) }` + `impl Value` helpers (`as_str`, `as_i64`, `get`, indexing) | ~200 |
| `parse.rs` | `parse(&[u8]) -> Result<Value, ParseError>` — recursive descent: `parse_value` → `parse_object` / `parse_array` / `parse_string` / `parse_number` / `parse_keyword`. Single-pass, no backtracking. | ~300 |
| `emit.rs` | `emit(value: &Value, buf: &mut Vec<u8>)` — iterative-ish writer, escapes strings per RFC 8259 §7 | ~150 |

Total: ~660 LOC. No v1 precedent — no sample parser to port. Reference the target shape against [`prototypes/wo-db/src/value.hpp`](../../prototypes/wo-db/src/value.hpp) for the `Value` variants (same six kinds as the C++ prototype, minus `Float` which that prototype folds into `Int` but we need for HTTP request bodies like `{"qty": 2.5}`).

### `Cargo.toml` delta

```diff
 [dependencies]
 anyhow     = "1"
-serde      = { version = "1", features = ["derive"] }
-serde_json = "1"
 libc       = "0.2"
```

### Consumers to update

Search: `rg 'serde_json|serde::' crates/rt/src | wc -l` — expected ~20 call sites. Each is a mechanical swap:

| Current | After |
| --- | --- |
| `serde_json::json!({"key": value})` | `json::Value::Object(…)` or a small `json!` macro we ship |
| `serde_json::Value` | `json::Value` |
| `serde_json::Map<String, Value>` | `BTreeMap<String, json::Value>` (the runtime already uses BTreeMap for stable order) |
| `serde_json::from_slice::<Value>(&bytes)?` | `json::parse(&bytes)?` |
| `Json(json!(rows)).into_response()` | `Response::ok().json_body(&rows)` (new helper on phase-03 `Response`) |
| `#[derive(Serialize, Deserialize)]` on any `rt` struct | deleted — no consumer after this phase |

The biggest consumer is `crates/rt/src/engine.rs` — `Row` is a `serde_json::Map<String, Value>` today. It becomes `BTreeMap<String, json::Value>`. The `eval_default()` function's `json!(n)` / `json!(b)` calls become `Value::Int(n)` / `Value::Bool(b)`. Minor, all local.

## A compact `json!` macro (for ergonomics)

Without `serde_json::json!`, the most-used construction pattern (`json!({"runtime": "wo", "stage": 2})`) gets verbose. Ship a minimal macro:

```rust
#[macro_export]
macro_rules! json {
    (null)             => ($crate::json::Value::Null);
    (true)             => ($crate::json::Value::Bool(true));
    (false)            => ($crate::json::Value::Bool(false));
    ([$($e:tt),* $(,)?]) => (
        $crate::json::Value::Array(vec![$($crate::json!($e)),*])
    );
    ({$($k:tt : $v:tt),* $(,)?}) => ({
        let mut m = std::collections::BTreeMap::new();
        $( m.insert(stringify!($k).trim_matches('"').to_string(), $crate::json!($v)); )*
        $crate::json::Value::Object(m)
    });
    ($e:expr) => ($crate::json::Value::from($e));
}
```

Covers 95% of current `serde_json::json!(...)` uses in the codebase. For the other 5%, build `Value` by hand.

## Exit criteria

1. **`cargo build`** — compiles with three deps (`anyhow`, `libc` + `Cargo.toml` itself).
2. **New tests in `crates/rt/src/json/`:**
   - `parse_object_simple` — `{"a":1,"b":"x"}` round-trips.
   - `parse_nested_and_array` — `{"xs":[1,2,3],"meta":{"k":"v"}}` round-trips.
   - `parse_escapes` — `"\\n\\t\\\"\\u0041"` → `"\n\t\"A"`.
   - `emit_stable_key_order` — emitting a `BTreeMap`-backed object produces keys in sorted order (matters for `.rest` expected-body stability).
   - `parse_errors` — unterminated string, trailing comma, missing comma, unclosed object all return `ParseError` with line/col.
3. **All 14 existing `rt` tests pass** after the swap (the `engine::Engine` and `server::*` tests most affected).
4. **`reference/rest/blog.rest`** — 20 assertions all return the same HTTP status AND the same response body shape (may differ in key ordering if `BTreeMap` ordering differs from `serde_json`'s insertion order — document the shift).
5. **Dep audit.** `cargo tree -p rt --depth 1` shows zero `serde*` lines.

## Non-scope

- **No streaming parse.** Request bodies are small (< 1 MB on every sample endpoint). A buffered full-body parse is fine.
- **No `serde_json`-compat feature flag.** Clean break; this is the only consumer that matters, and we control it.
- **No JSON Pointer, no JSON Schema, no JSON Patch.** If needed later, layer on top.
- **No crate extraction to `crates/value/`.** Same rule as phase 02/03: wait for a second consumer.

## Verification

```bash
cargo build                                    # three deps
cargo test --lib json                          # new parser/emitter tests
cargo test --lib                               # 14 existing tests still green
# full .rest smoke — same script as phase 04 exit criterion 3
cd reference/crates && cargo build && cargo test
```

## After this phase

Two deps left: `anyhow` and `libc`. Phase 06 removes `anyhow`. After that, `libc` is the only external crate — the stated end goal.
