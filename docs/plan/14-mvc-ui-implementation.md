# 14 — MVC UI implementation: model = class, view = htmlx + scss, controller = .wo

**Context sources:** [`./exploration/ui/08-mvc-structure.md`](./exploration/ui/08-mvc-structure.md) (the design this plan implements), [`./exploration/ui/01-htmlx-format-spec.md`](./exploration/ui/01-htmlx-format-spec.md) / [`02-ui-compiler.md`](./exploration/ui/02-ui-compiler.md) / [`03-client-runtime.md`](./exploration/ui/03-client-runtime.md) (the three UI-track pieces this plan sequences, each with port sources and LOC budgets), [`./13-class-model-live-pricing.md`](./13-class-model-live-pricing.md) (the class methods controllers call: 13a/13b; the LIVE deltas views consume: 13c), [`../examples/pricing/ui/pricing/`](../examples/pricing/ui/pricing/) (the reference MVC triplet), [`reference/crates/wo-htmlx/`](../../reference/crates/wo-htmlx/) (the v1 template engine, primary port source).

## Context

[`exploration/ui/08-mvc-structure.md`](./exploration/ui/08-mvc-structure.md) locks the screen anatomy: **model** = the `class`/`type` itself, **view** = plain `.htmlx` + external `.scss`, **controller** = a `.wo` file (`route:`/`view:`/`styles:`/`model:`/`actions:`) that binds the model into the view and is the only place UI may call class methods. The UI exploration docs 01–03 already specify the htmlx engine, the `##ui` compiler, and the client runtime in implementable detail. What's missing is the build order, the two genuinely new pieces (the controller format and the SCSS subset compiler), and the wiring into the `13` class-model track. This doc is that sequence.

Everything lands in **`crates/ui`** (currently a placeholder) and small deltas to `crates/rt` — consistent with the crate inventory in [`crates/README.md`](../../crates/README.md). The deployment shape never changes: one binary serving SSR + database + API on the kernel-primitive runtime.

## Goal

`cargo run --bin wo -- run docs/examples/pricing` (after plan 13a–13c land) serves `GET /pricing` as styled SSR HTML; clicking ☆ dispatches a controller action; an Ops `set-price` action calls `Product.set_price`, the commit pushes a delta, and the price cell patches in every open browser without reload — the [plan 13d exit criterion](./13-class-model-live-pricing.md), implemented MVC-shaped.

## Dependency graph

```
14a htmlx engine ──────┬─→ 14c controller format ─→ 14d SSR routes ─→ 14e actions ─→ 14f live patch
14b scss compiler ─────┘                                  │               │              │
   (14a ∥ 14b — no shared code)                    needs engine     needs 13a+13b   needs 13c
                                                   (exists today)
```

Phases 05/06 (hand-rolled JSON / bespoke error) are orthogonal: `crates/ui` adopts `serde`/`serde_json` per the ui/01 decision and migrates when 05 lands. Phase 08 (`sendfile`) upgrades static-asset serving in 14d when it arrives; 14d ships with plain buffered writes first.

## Sub-phase sequence

### `14a-htmlx-engine.md` — port the view engine into `crates/ui`

Execute [`exploration/ui/01-htmlx-format-spec.md`](./exploration/ui/01-htmlx-format-spec.md) as written: port `reference/crates/wo-htmlx` (585 LOC — `parser.rs`, `ast.rs`, `value.rs`, `registry.rs`, `render.rs` carried over per its table) into `crates/ui/src/htmlx/`, extend with `<wo:live>` structured nodes, `wo:bind` capture, and the `data-wo-manifest` JSON emitter (~250 LOC new). One addition beyond the 01 spec, from the MVC design: `<wo:live source="…">` records whether `source` is a bare name (controller model binding, resolved in 14d) or an inline query — a one-field change to `LiveSubscription`.
**Exit:** the 01 spec's criteria — `cargo build -p ui` green, golden parse+render for every `.htmlx` under `docs/examples/{blog,ecommerce}` **plus** [`pricing/ui/pricing/pricing.htmlx`](../examples/pricing/ui/pricing/pricing.htmlx), manifest matches the 01 schema.

### `14b-scss-subset.md` — the stylesheet compiler

New, no port source (~400 LOC at `crates/ui/src/scss/`): scanner → rule tree → flattener. Exactly the subset locked in [08-mvc decision 3](./exploration/ui/08-mvc-structure.md): `$variables`, nesting (including `&`-less descendant flattening), `@use "partials"` (`ui/styles/_*.scss`), comments. **No mixins, functions, `@extend`, or color math** — `rgba($accent, 0.06)` in the reference file compiles by literal substitution of `$accent` and is the only function-form supported. Output is one flat `.css` per screen, written to `target/wo/<app>/static/`.
**Exit:** [`pricing.scss`](../examples/pricing/ui/pricing/pricing.scss) → golden-file CSS; unknown construct = compile error naming file:line (never silent passthrough); runs standalone (`wo build` integration is 14d).

### `14c-controller-format.md` — parse the controller, keep the shorthand

The `Kind::HashHash("ui")` skip arm in `crates/rt/src/parser.rs` (L80–88) parses for real, into one of two IRs by key-shape dispatch:

- **Controller form** (`route:`/`view:`/`styles:`/`model:`/`actions:` — the MVC triplet's `pricing.wo`) → new `Controller` IR: route pattern, view/styles paths resolved relative to the screen directory, `model:` entries as named query strings (`LIVE` flag captured, execution deferred), `actions:` entries as `(name, params, target-method-or-fn, role-set)`.
- **Shorthand form** (`source:`/`columns:`/… — the existing ecommerce/blog screens) → the `Screen` IR of [`exploration/ui/02-ui-compiler.md`](./exploration/ui/02-ui-compiler.md), whose codegen emits a generated view + a synthesized `Controller` — [08-mvc decision 5](./exploration/ui/08-mvc-structure.md): the shorthand is sugar over the triplet, one downstream path.

`##app` and `##component` keep their current skip behaviour (owned by ui/05 and ui/02 respectively).
**Exit:** `pricing.wo` parses to a `Controller` with 2 model bindings + 3 actions; every existing `##ui` screen in blog/ecommerce parses to `Screen` and compiles to an `.htmlx` that 14a round-trips; a controller naming a missing view file is a compile error.

### `14d-ssr-routes.md` — the single binary serves the screen

Wire controllers into `crates/rt`'s router (`crates/rt/src/server.rs`): each `Controller.route` becomes a GET route; the handler resolves `model:` bindings against the in-process engine (snapshot `select` now — the `LIVE` flag additionally registers a 13c subscription when that phase is live), renders the view via 14a with the model names as root scope, and emits HTML + manifest + `<link href="/static/<screen>.css">`. `wo build`/`wo run` gain the asset step: compile SCSS (14b), bake `/_wo/runtime.js` (`include_bytes!`, per ui/03 decision 6), serve `target/wo/.../static/` with buffered writes (upgraded to `sendfile` when [phase 08](./08-sendfile-static-assets.md) lands).
**Exit:** `GET /pricing` returns styled SSR HTML with a valid manifest and resolvable CSS/JS links; `GET /` on the blog sample is unaffected; route table printed at boot includes UI routes alongside REST.

### `14e-action-dispatch.md` — controller actions call class methods

`wo:action` buttons POST to `/_wo/action/<screen>/<action>` with `wo:args` + form payload. The dispatcher looks up the controller's action table, enforces the `role:` set server-side (per-app policy model of [`exploration/ui/07-per-app-policies.md`](./exploration/ui/07-per-app-policies.md)), and invokes the target: a class method via the 13b row-scoped RPC path (`Product{ id == id }.set_price(amount)`) or a free `fn`. Response is 204 — the UI never re-renders from the action response; the visible change arrives as a 13c delta, keeping one update path.
**Requires:** 13a + 13b. **Exit:** the ☆/★ watch toggle round-trips; `set-price` with an Ops session commits a Price; without the role it's 403 and no transaction starts.

### `14f-live-patching.md` — the browser follows commits

Execute [`exploration/ui/03-client-runtime.md`](./exploration/ui/03-client-runtime.md) as written (~500 LOC vanilla JS at `crates/ui/assets/wo-runtime.js`, JSON frames over one WebSocket, targeted DOM patching by `data-key` + `wo:bind`, coalescing backpressure, snapshot resync on reconnect), pointed at the 13c subscription endpoint.
**Requires:** 13c. **Exit:** the plan-13d criterion — `set_price` via curl in one terminal, the price cell changes in every open `/pricing` browser without reload; kill the server, restart, the page resyncs on reconnect.

## Verification targets (after 14f)

| Check | Target | How |
| --- | --- | --- |
| Golden corpus | every `.htmlx` in blog/ecommerce/pricing parses + renders byte-stable | `cargo test -p ui` golden files |
| SCSS | `pricing.scss` → golden CSS; errors carry file:line | `cargo test -p ui scss` |
| SSR | `GET /pricing` < 5 ms p99 on the dev box (RAM engine, no I/O on read path) | scripted curl loop |
| End-to-end | 13d criterion green | two-terminal demo, scripted in `just pricing-demo` |
| Single binary | UI + DB + API + WS in one `wo build` output, no Node anywhere | `ldd` shows libc only; no build-step JS |
| Dep budget | `crates/ui`: `serde`/`serde_json` only (dropped when phase 05 lands) | `Cargo.toml` review |

## Non-scope

- **No SPA router, no client-side templates.** Navigation is full-page SSR; only `wo:bind` cells and `<wo:live>` subtrees mutate in place. (ui/00 decision; unchanged.)
- **No SCSS mixins/functions/`@extend`/color math** beyond literal variable substitution — the subset is a floor, widened only by demonstrated need in the sample corpus.
- **No theme system / design tokens.** Shared partials under `ui/styles/_*.scss` are the only sharing mechanism for now.
- **No component framework.** `##component` partials render server-side via the 14a engine; they have no client behaviour beyond inherited `wo:bind` sites.
- **No changes to the REST API surface.** UI routes live beside `/api/*`; nothing under `/api` changes shape in this plan.

## Cross-references

- [`./exploration/ui/08-mvc-structure.md`](./exploration/ui/08-mvc-structure.md) — the design; its exit criteria are satisfied by 14c/14b/14f respectively.
- [`./13-class-model-live-pricing.md`](./13-class-model-live-pricing.md) — 13a/13b gate 14e; 13c gates 14f; 13d's exit criterion is this plan's end-to-end target.
- [`./exploration/ui/00-overview.md`](./exploration/ui/00-overview.md) — the UI track's master frame (per-app binaries, shared DB daemon) that 14d's asset/serving choices stay compatible with.
- [`reference/crates/wo-htmlx/`](../../reference/crates/wo-htmlx/) — primary port source (585 LOC), per ui/01.
- [`../examples/pricing/ui/pricing/`](../examples/pricing/ui/pricing/) — the reference triplet every sub-phase tests against.
