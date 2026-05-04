# 02 — `##ui` → `.htmlx` compiler

**Context sources:** [`./00-overview.md`](./00-overview.md) §§ "Sub-phase sequence" (L172–180), "Design decisions" 1–6, [`./01-htmlx-format-spec.md`](./01-htmlx-format-spec.md) (the emission target), [`../../runtime/database/06-lowcode-fullstack.md`](../../runtime/database/06-lowcode-fullstack.md) (the `##ui` block spec), [`docs/examples/blog/ui/article_list.wo`](../../examples/blog/ui/article_list.wo), [`docs/examples/blog/ui/article_detail.wo`](../../examples/blog/ui/article_detail.wo), [`docs/examples/ecommerce/apps/admin/ui/orders/orders.wo`](../../examples/ecommerce/apps/admin/ui/orders/orders.wo) (the test corpus), [`crates/rt/src/parser.rs:80–116`](../../../crates/rt/src/parser.rs) (the parse-and-discard call site to replace).

## Goal

Walk a parsed `##ui` block — every key listed in 00-overview's locked grammar (`title`, `source`, `live`, `role`, `filter`, `quick-filters`, `columns`, `sort`, `actions`, `pagination`, `refresh`, `highlight-new`, `key`, `sections`, `inputs`, `use`, `with`, `template`, `styles`) — and emit a complete `.htmlx` template that parses under phase 01's grammar. When a hand-written `<screen>.htmlx` sits beside the `.wo`, the compiler honours it and only validates that the manifest still aligns.

## Design decisions (locked)

1. **File-presence dispatch.** If `apps/<X>/ui/<screen>/<screen>.htmlx` exists alongside `<screen>.wo`, the hand-written file wins. The compiler still emits a manifest; it errors if the manifest's `bind_sites` reference fields the hand-written template doesn't expose. Anchored in [`./00-overview.md`](./00-overview.md) L24, L30, L46.
2. **`live: true` ⇒ `<wo:live>` wrapper.** The compiler emits `<wo:live source="<source>" key="<key|id>" sort="<sort.default|nothing>" filter="<filter|nothing>">` around the auto-generated `<table>`. `key` defaults to `id` when not declared.
3. **`renderer: <name>` ⇒ helper invocation by rule table.** A static rule table maps each renderer name to its emission form: `markdown` → `{{markdown <field>}}`, `money` → `{{> money amount=<field>}}`, `relative-date` → `{{relative <field>}}`, `code` / `tag-chips` / `pill` / `image` / `stock-badge` / `list` similarly. The set is closed and matches phase 01's helper registry.
4. **`actions: row-* / bulk-*` ⇒ `data-action` + `data-role` attributes.** Click → POST `/api/fn/<fn>`. Role gating is a `data-role="<set>"` attribute the runtime hides on; phase 07 wires the server-side check.
5. **Generated templates land in `target/wo/<app>/ui/<screen>.htmlx`.** Same path the per-app binary in phase 05 reads from at startup. Build artefact, not committed.
6. **`crates/rt/src/parser.rs:80–88` no longer skips `##ui`.** The `Kind::HashHash` arm parses into a `Screen` IR (this phase's new type). All other `##` blocks (`##app`, `##component`) keep their current `skip_top_level_chunk` behaviour for now — `##app` is owned by phase 05 and `##component` parses inline as a sibling of `##ui` but emits no template (it's a partial that other screens reference).

## Scope

### New files inside `crates/ui/src/compiler/`

| File | Responsibility | Port source |
| --- | --- | --- |
| `mod.rs` | Re-exports `compile_screen`, `Screen`, `Column`, `Action`, `CompileError` | new (~30 LOC) |
| `screen.rs` | `Screen` IR — every key listed in the goal section above | new (~150 LOC) |
| `codegen.rs` | Walk `Screen` → emit `.htmlx` source string | new (~280 LOC) |
| `renderers.rs` | Closed `renderer:` → helper-emission rule table | new (~120 LOC) |
| `fallback.rs` | File-presence dispatch + manifest cross-check | new (~80 LOC) |

### Modified file

| File | Change | Notes |
| --- | --- | --- |
| `crates/rt/src/parser.rs` | Replace the `Kind::HashHash(_)` skip arm at L80–88 with a real parse into `Screen` when the tag is `ui` | +60 LOC delta |

Total: ~720 LOC (all new — the v1 codebase has no `##ui` precedent to port).

### `Cargo.toml` change

`crates/ui` already depends on `serde`/`serde_json` from phase 01. No new deps.

## API shape (target)

```rust
use ui::compiler::{compile_screen, compile_app, Screen};

let screen: Screen   = ql::parse_ui_block(src)?;
let template: String = compile_screen(&screen, &ctx)?;     // an .htmlx string
let mani             = ui::htmlx::Template::parse(&template)?.manifest();

// Whole-app pipeline used by phase 05's `wo build`:
let outputs: Vec<(PathBuf, String)> = compile_app(&app_dir)?;
for (path, src) in outputs { fs::write(path, src)?; }
```

## Exit criteria

1. `cargo build -p ui` and `cargo build -p rt` green.
2. **Compile every sample `##ui`** in the test corpus: blog `article_list.wo`, blog `article_detail.wo`, ecommerce `apps/storefront/ui/home/home.wo`, `apps/storefront/ui/orders/orders.wo`, `apps/admin/ui/orders/orders.wo`. Output template parses cleanly under phase 01.
3. **Hand-written fallback honoured.** With a hand-written `apps/admin/ui/orders/orders.htmlx` present, the compiler returns its source unchanged but still emits the manifest.
4. **Manifest cross-check fires.** Renaming `body` to `text` in a hand-written template that the `##ui` block expects under `wo:bind="body"` produces a `CompileError::HandWrittenMissingField` diagnostic.
5. **Parser change is non-breaking.** `crates/rt`'s 14 unit tests still pass; `cargo run --bin wo -- run docs/examples/blog` boots and serves REST as before.
6. `cd reference/crates && cargo build && cargo test`.

## Non-scope

- **No SSR.** This phase emits files only — the runtime in phase 05 reads them at startup.
- **No author-defined renderers.** The closed table from phase 01's helper registry is the contract.
- **No build-output caching.** The compiler runs on every `wo build`. Caching is a future concern.
- **No partial recovery.** First parse or codegen error aborts compilation for the screen; whole-app compilation reports per-screen status.
- **No `##component` codegen in this phase.** Components remain their existing partial-include shape (`{{> name args}}`) — only `##ui` screens drive new emission.

## Verification

```bash
cargo build -p ui -p rt
cargo test  -p ui --test compile_blog
cargo test  -p ui --test compile_ecommerce
cargo test  -p ui --test fallback_handwritten

# end-to-end: emit templates for the storefront and inspect them
cargo run --bin wo -- build docs/examples/ecommerce/apps/storefront --emit-templates-only
ls target/wo/storefront/ui/                    # home.htmlx, orders.htmlx
head -1 target/wo/storefront/ui/orders.htmlx   # starts with <wo:live source="Order{ … }">

# v1 regression
cargo run --bin wo -- run docs/examples/blog &
PID=$!; sleep 1; curl -fsS http://127.0.0.1:8080/ >/dev/null; kill $PID

cd reference/crates && cargo build && cargo test
```

## After this phase

Phase 03 (`03-client-runtime.md`) is now unblocked: every screen has a manifest the client runtime can read. Phase 04 (`04-workspace-layout.md`) places the compiled outputs under `target/wo/<app>/ui/`, which phase 05 then bakes into the per-app binary.
