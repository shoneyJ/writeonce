# 08 — MVC structure: model = class, view = htmlx + scss, controller = .wo

**Context sources:** [`reference/writeonce-app/src/app/`](../../../../reference/writeonce-app/src/app/) (the v1 Angular app whose component anatomy this formalizes), [`./00-overview.md`](./00-overview.md) ("Angular-component-style layout" — `home/{home.wo, home.htmlx, home.css}`), [`./01-htmlx-format-spec.md`](./01-htmlx-format-spec.md) (the view grammar: Mustache + `<wo:live>` + `wo:bind`), [`./02-ui-compiler.md`](./02-ui-compiler.md), [`./03-client-runtime.md`](./03-client-runtime.md), [`../../13-class-model-live-pricing.md`](../../13-class-model-live-pricing.md) (the class methods controllers call), [`../../../examples/pricing/ui/pricing/`](../../../examples/pricing/ui/pricing/) (the reference screen).

## Goal

Every writeonce UI screen follows **Model–View–Controller**, with the same file anatomy the v1 Angular app used — but collapsed into the single binary. One screen = one directory with three files:

```
ui/pricing/
├── pricing.wo       # Controller — binds the model into the view, exposes actions
├── pricing.htmlx    # View — plain htmlx (Mustache + wo:live/wo:bind), no logic
└── pricing.scss     # View styles — external, compiled at `wo build`
```

## The mapping, against the v1 Angular app

| MVC role | v1 Angular (`reference/writeonce-app/src/app/`) | writeonce |
| --- | --- | --- |
| **Model** | `models/article.ts` (interface) + `services/article.service.ts` (HTTP fetch) | the `class` / `type` declaration itself (`types/product.wo`). No service layer: the database is in-process, and a model binding **is** a query — `LIVE select` for push, `select` for snapshot |
| **View** | `article.component.html` + `article.component.css` | `pricing.htmlx` + `pricing.scss`. Plain markup; the only dynamic constructs are Mustache paths and `<wo:live>` / `wo:bind` from [`01-htmlx-format-spec.md`](./01-htmlx-format-spec.md) |
| **Controller** | `article.component.ts` (`@Component({templateUrl, styleUrl})`, fields, methods, `service.subscribe(...)`) | `pricing.wo` — declares `view:` / `styles:` (≈ `templateUrl` / `styleUrl`), a `model:` block (≈ component fields), and an `actions:` block whose handlers **call class methods** |

What Angular needed four layers for (interface, service, component class, template) writeonce does in three files against one runtime — there is no HTTP client between controller and model because there is no network between them.

## Design decisions

1. **The controller is declarative, like everything else in `.wo`.** It does not contain imperative rendering code; it declares *what* is bound and *which* method each action invokes. Shape:

   ```wo
   ##ui
   #pricing
       route:  /pricing
       view:   pricing.htmlx              -- ≈ Angular templateUrl
       styles: pricing.scss               -- ≈ Angular styleUrl

       -- Model → View binding. Names declared here are the root scope of
       -- the .htmlx file; LIVE bindings re-patch the view on every commit.
       model:
           products:  LIVE select Product{ name, sku, prices }
           watchlist: $session.watchlist

       -- Controller actions: the only place UI may invoke class methods.
       actions:
           set-price(id, amount): Product{ id == id }.set_price(amount)   role: Ops | Admin
           watch(id):             session.watchlist += id
   ```

2. **The view is plain `.htmlx`, logic-free.** Mustache paths, `{{#each}}`/`{{#if}}`, partials, helpers, `<wo:live>` subtrees, `wo:bind` attributes — nothing else. A `<wo:live source="products">` whose `source` is a bare name resolves against the controller's `model:` block (the M→V binding); an inline query in `source` remains legal for controller-less partials. Views never call methods — they raise actions (`wo:action="set-price"`), the controller dispatches.

3. **Styles are external SCSS, compiled at `wo build`.** No `<style>` blocks in views, no inline styles, one `.scss` per screen plus shared partials (`ui/styles/_*.scss`). `wo build` compiles a **strict SCSS subset** — variables, nesting, `@use` of partials; no mixins/functions in the first cut — to flat CSS served as a static asset via `sendfile` ([`../../08-sendfile-static-assets.md`](../../08-sendfile-static-assets.md)). Hand-rolled in `crates/ui` (~400 LOC scanner + nesting flattener), zero external dependencies — same stance as every other phase.

4. **One binary, unchanged.** `wo build` links the SSR renderer, the compiled views + manifest, the flattened CSS, the database engine, the REST/WS API, and the kernel-primitive concurrency runtime (epoll today, thread-per-core io_uring per [`../../09-concurrency-scaleout.md`](../../09-concurrency-scaleout.md)) into the single output. MVC changes the *source layout*, not the deployment shape.

5. **The `##ui` table shorthand survives as sugar.** The earlier declarative screen spec (`columns:` / `sort:` / `pagination:`, as in the ecommerce `home.wo`) compiles to a generated view + controller pair. Writing the triplet by hand is the general form; the shorthand is the 80% case. Either way the compiler output is identical: SSR HTML + manifest + bindings.

## Request flow

```
GET /pricing
  → router (controller route:)                       [crates/http]
  → controller resolves model: bindings against the engine (in-process)
  → SSR renders pricing.htmlx with model scope        [crates/ui, per 01/02]
  → emits HTML + <script data-wo-manifest> + <link pricing.css>

browser action wo:action="set-price"
  → POST dispatched to the controller action
  → action calls Product.set_price(amount)            [class method, plan 13b]
  → commit → delta → every <wo:live> subscriber       [plan 13c]
  → client runtime patches wo:bind cells              [03-client-runtime.md]
```

## Migration note

The two existing screen specs (`docs/examples/ecommerce/apps/*/ui/*/`, single-file `##ui` shorthand) stay valid under decision 5. New screens — starting with [`docs/examples/pricing/ui/pricing/`](../../../examples/pricing/ui/pricing/) — use the triplet. The v1 Angular app stays archived; its components are the *shape* reference, not a port source (the htmlx port source remains `reference/crates/wo-htmlx`).

## Exit criteria (implementation sequenced in [plan 14](../../14-mvc-ui-implementation.md), landing with plan 13d)

1. `crates/ui` resolves a controller file: `route:`/`view:`/`styles:`/`model:`/`actions:` parsed, view rendered with model scope, actions dispatched to class methods.
2. SCSS subset compiler: `pricing.scss` → flat CSS at build, golden-file tested.
3. The pricing screen works end-to-end per [plan 13d's exit criterion](../../13-class-model-live-pricing.md): a `set_price` commit patches the price cell in every open browser without reload.
