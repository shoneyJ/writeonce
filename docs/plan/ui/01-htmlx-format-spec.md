# 01 — `.htmlx` format spec

**Context sources:** [`./00-overview.md`](./00-overview.md) §§ "`.htmlx` with live subscriptions — target format" (L127–166), "Design decisions" (L28–37), [`reference/crates/wo-htmlx/`](../../../reference/crates/wo-htmlx/) (the v1 template engine that 90% of this phase ports), [`templates/article.htmlx`](../../../templates/article.htmlx) and [`templates/home.htmlx`](../../../templates/home.htmlx) (v1 concrete usage), [`docs/examples/ecommerce/shared/components/order-row.htmlx`](../../examples/ecommerce/shared/components/order-row.htmlx) (the live-binding workload this format must serve).

## Goal

Lock the exact `.htmlx` grammar — every v1 Mustache construct unchanged plus two new constructs: a `<wo:live source=… key=…>…</wo:live>` subscription subtree and a `wo:bind="field"` field-level live attribute — and define the JSON manifest emitted in a `<script data-wo-manifest>` so the client runtime (phase 03) knows which DOM nodes map to which subscription rows and fields.

## Design decisions (locked)

1. **Mustache constructs carry through unchanged.** `{{path}}`, `{{#each xs as y}}…{{/each}}`, `{{#if cond}}…{{/if}}`, `{{#when cond}}…{{/when}}`, `{{> partial arg=val}}`. The v1 parser already handles all of these; the new parser inherits them verbatim. See [`reference/crates/wo-htmlx/src/parser.rs`](../../../reference/crates/wo-htmlx/src/parser.rs) (173 LOC) and the AST in [`ast.rs`](../../../reference/crates/wo-htmlx/src/ast.rs) (18 LOC).
2. **`<wo:live>` is a parsed structured node, not HTML passthrough.** The parser recognises the `<wo:` prefix, captures attributes (`source`, `key`, optional `sort`, `filter`), and recursively parses the body as a normal `.htmlx` subtree. No nesting in this phase — error at parse if a `<wo:live>` contains another `<wo:live>`.
3. **`wo:bind="field"` is an HTML attribute, parsed but emitted verbatim.** SSR writes the attribute through; the consumer is the client runtime. The parser records each `(element, field)` pair into the manifest; nothing else changes about element rendering.
4. **Helpers are a closed Rust enum.** v1 invocation forms (`{{relative ts}}`, `{{#if (eq for "ops")}}`, `{{> money amount=x}}`) carry through. The registered set is fixed for this phase: `relative`, `eq`, `markdown`, `code`, `money`, `tag-chips`, `pill`, `image`, `stock-badge`, `list`. No author extensibility.
5. **Manifest is one JSON object per page.** Emitted as `<script type="application/json" data-wo-manifest>{ … }</script>` and consumed only by phase 03's client runtime. Schema below; `version: 1` is a constant for this phase.

## Scope

### New files inside `crates/ui/src/htmlx/`

| File | Responsibility | Port source |
| --- | --- | --- |
| `mod.rs` | Re-exports `Template`, `Manifest`, `LiveSubscription`, `BindSite`, `ParseError`, `RenderError` | [`reference/crates/wo-htmlx/src/lib.rs`](../../../reference/crates/wo-htmlx/src/lib.rs) (11 LOC) |
| `ast.rs` | Adds `Node::Live { attrs, body }` and `wo_bind: Option<String>` on element nodes | [`reference/crates/wo-htmlx/src/ast.rs`](../../../reference/crates/wo-htmlx/src/ast.rs) (18 LOC) — extend by ~50 LOC |
| `parser.rs` | Adds `<wo:` prefix recognition + attribute capture; rest unchanged | [`reference/crates/wo-htmlx/src/parser.rs`](../../../reference/crates/wo-htmlx/src/parser.rs) (173 LOC) — extend by ~90 LOC |
| `value.rs` | Path resolution against a context Value | [`reference/crates/wo-htmlx/src/value.rs`](../../../reference/crates/wo-htmlx/src/value.rs) (122 LOC) — copied verbatim |
| `registry.rs` | Closed helper-fn registry | [`reference/crates/wo-htmlx/src/registry.rs`](../../../reference/crates/wo-htmlx/src/registry.rs) (121 LOC) — extend by ~60 LOC for new helpers |
| `render.rs` | Emits HTML; wraps `<wo:live>` body in `<div data-wo-subscription="…">` for the runtime | [`reference/crates/wo-htmlx/src/render.rs`](../../../reference/crates/wo-htmlx/src/render.rs) (140 LOC) — extend by ~70 LOC |
| `manifest.rs` | Walks the AST, collects subscriptions + bind sites, serialises JSON | new (~150 LOC) |

Total: ~835 LOC (585 ported + ~250 new).

### `Cargo.toml` change

```toml
[dependencies]
serde      = { version = "1", features = ["derive"] }
serde_json = "1"
```

(Both already in `crates/rt`; `crates/ui` adopts them rather than hand-rolling JSON for the manifest at this phase.)

### Manifest schema

```json
{
  "version": 1,
  "subscriptions": [
    {
      "id":             "orders-live-0",
      "source":         "Order{ status != Cancelled }",
      "key":            "id",
      "sort":           "placed_at desc",
      "filter":         null,
      "root_selector": "[data-wo-subscription=\"orders-live-0\"]"
    }
  ],
  "bind_sites": [
    { "subscription_id": "orders-live-0", "key": "id", "field": "status" },
    { "subscription_id": "orders-live-0", "key": "id", "field": "total"  }
  ]
}
```

## API shape (target)

```rust
use ui::htmlx::{Template, Manifest, HelperRegistry};

let tmpl = Template::parse(src)?;            // Result<Template, ParseError>
let html = tmpl.render(&ctx, &registry)?;    // Result<String, RenderError>
let mani = tmpl.manifest();                  // Manifest

// SSR pattern: page = head + html + "<script data-wo-manifest>" + mani.to_json() + "</script>"
```

## Exit criteria

1. `cargo build -p ui` green; no new top-level dependencies beyond `serde`/`serde_json`.
2. **Golden parse + render** for every `.htmlx` file under [`docs/examples/blog/ui/components/`](../../examples/blog/ui/components/) and [`docs/examples/ecommerce/shared/components/`](../../examples/ecommerce/shared/components/) — output is HTML and parses back into an isomorphic AST.
3. **Manifest emission** for `<wo:live source="Order{ status != Cancelled }" key="id" sort="placed_at desc">…</wo:live>` produces a `LiveSubscription` with the source string preserved verbatim and the body wrapped under `data-wo-subscription="orders-live-0"`.
4. **Bind-site collection** for `<td wo:bind="status">{{status}}</td>` inside `<wo:live>` records `(subscription_id, key="id", field="status")` once and only once.
5. **v1 regression**: `cargo run --bin wo -- run docs/examples/blog` continues to start without parser errors. The blog sample has no `<wo:live>` or `wo:bind` today; nothing should regress.
6. All 14 existing `crates/rt` tests pass.

## Non-scope

- **No SSR.** Phase 02 emits templates; phase 03 ships the runtime; serving them is a downstream concern that the per-app binary in phase 05 wires together.
- **No nested `<wo:live>`.** Parse error in this phase. Author can compose live subtrees by partial inclusion (`{{> child}}`).
- **No author-extensible helpers.** The closed enum is the contract for this phase.
- **No streaming render.** Templates are rendered to a single `String`.
- **No manifest version negotiation.** Wire format is `version: 1` always.

## Verification

```bash
cargo build -p ui
cargo test  -p ui --test golden_v1            # blog/ecommerce templates byte-identical
cargo test  -p ui --test golden_extensions    # <wo:live> + wo:bind cases
cargo test  -p ui --test manifest             # manifest emission

# v1 regression
cargo run --bin wo -- run docs/examples/blog &
PID=$!; sleep 1; curl -fsS http://127.0.0.1:8080/ >/dev/null; kill $PID

cd reference/crates && cargo build && cargo test
```

## After this phase

Phase 02 (`02-ui-compiler.md`) consumes the `Template` + `Manifest` types defined here as its emission target — every `##ui` block compiles down to an `.htmlx` file that parses cleanly under this phase's parser. Phase 03 (`03-client-runtime.md`) consumes the manifest JSON schema as its wire input.
