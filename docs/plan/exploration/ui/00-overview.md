# UI track — `.htmlx` live templates + Angular-style monorepo

**Context sources:** [`docs/examples/ecommerce/ui/`](../../examples/ecommerce/ui/) (current `##ui` screens — storefront, order_tracker, admin_orders), [`docs/examples/ecommerce/types/`](../../examples/ecommerce/types/) + [`docs/examples/ecommerce/logic/`](../../examples/ecommerce/logic/) (the shared-schema + shared-fn anchor), [`reference/crates/wo-htmlx/`](../../../reference/crates/wo-htmlx/) (v1 template engine — `{{path}}`, `{{#each}}`, `{{> partial}}`, `data-bind` attributes), [`templates/`](../../../templates/) (v1 blog's concrete `.htmlx` usage), [`docs/runtime/database/06-lowcode-fullstack.md`](../../runtime/database/06-lowcode-fullstack.md) (Phase 6's `##ui` + `##app` block spec).

## Context

Three threads converge into one plan:

1. **`##ui` needs a concrete output format.** Phase 6's spec says screens "compile to a render tree" served as SSR HTML with a thin client runtime, but the actual template format isn't named. The v1 `.htmlx` engine at [`reference/crates/wo-htmlx/`](../../../reference/crates/wo-htmlx/) already speaks `{{bindings}}`, `{{#each}}`, `{{> partials}}`, and `data-bind` attributes — it's 90% of what the new runtime needs and already has a working parser + renderer. Adopting it (and extending it with live-subscription semantics) is cheaper than inventing a new format.

2. **The samples want a home that matches how real frontends are organised.** The ecommerce sample today is one flat directory with `types/`, `logic/`, and `ui/` beside each other. A real deployment has *multiple apps* against the same data: a customer storefront, an admin dashboard, a fulfillment console, maybe a read-only analytics viewer. Each has its own routes, its own policies, its own ideal binary shape. Angular (via Nx / Angular CLI workspaces) solved this with `apps/*` + `libs/*` on top of a shared root config — writeonce adopts the same shape.

3. **Each app wants to be its own binary but share a database.** Running storefront and admin as one monolith conflates concerns: a CPU spike in admin stalls customer checkout; an admin auth bug opens customer data paths. Splitting into per-app binaries that share a single database backend (via the Phase-4 native wire protocol) gives blast-radius isolation without duplicating data.

Intended outcome: `docs/examples/ecommerce/` refactors into a workspace with `shared/` + `apps/storefront/` + `apps/admin/`. `wo build apps/storefront` produces a `storefront` binary. `wo db serve` runs the shared database. The apps connect over `wo://…` and serve `.htmlx` SSR pages that subscribe to LIVE queries without a page reload.

## Goal

After this track's sub-phases land:

- A **monorepo workspace** at `docs/examples/ecommerce/` with `shared/{types,logic,components}` + `apps/{storefront,admin}` structure.
- A **per-app binary** for each app under `apps/`: `wo build apps/storefront` → `./target/wo/storefront`, `wo build apps/admin` → `./target/wo/admin`. Each binary includes only its own `##ui` / `##app` / app-local types and logic; shared code compiles in by path reference.
- A **shared DB daemon** (`wo db serve`) — one process, no UI, just the engine and wire-protocol server. Each app binary connects as a client via the Phase-4 native protocol.
- **`.htmlx` as the compiled UI output.** Every `##ui` screen compiles into an `.htmlx` template file that the app binary serves; hand-written `.htmlx` files under `apps/X/ui/*.htmlx` are accepted as a first-class authoring alternative.
- **`.htmlx` subscribes.** A `<wo:live source="...">` subtree in the template registers a LIVE query at page load; a ~20 KB client JS runtime patches DOM nodes on delta frames without reloading the page.
- **Per-app users + policies.** Each app declares its role set in `apps/X/app.wo`; row-level policies on shared types stay global, app-scoped policies layer on top per route.

## Design decisions (locked)

1. **`.htmlx` is the template format; `##ui` is the DSL that emits it.** Authors choose per-screen: declare `##ui #home { source: Product, columns: [...] }` in `.wo` and let the compiler produce `home.htmlx`; OR hand-write `home.htmlx` for a custom page. Both flow through the same `wo-htmlx` renderer.
2. **Extend v1 `.htmlx` with two new constructs** — `<wo:live source="..." key="...">...</wo:live>` (subscription subtree) and `wo:bind="field"` (field-level live binding). The rest of the v1 syntax (`{{path}}`, `{{#each}}`, `{{> partial}}`) carries through unchanged.
3. **One binary per app, shared database process.** Not a shared library, not a monolith. Apps connect via the Phase-4 wire protocol (`wo://host:port`) — the same connection a Go/TS client would use. No in-process shared state between apps; their isolation is enforced by the OS process boundary.
4. **Angular-parallel workspace layout.** `apps/*` for deployable binaries, `shared/*` for libs shared across apps (types, logic, UI components), `wo.toml` at the workspace root. Each app also has its own `wo.toml` that names which `shared/` directories it depends on.
5. **File structure mirrors Angular component organisation.** Each UI screen lives in its own directory: `apps/storefront/ui/home/{home.wo, home.htmlx, home.css}`. Tests go in `home.test.wo`. Matches the Angular component pattern (`home.component.ts`, `home.component.html`, `home.component.scss`).
6. **Per-app routes, not per-screen routes.** `apps/storefront/app.wo` declares route table; each route maps to a `ui.<screen>` declared under `apps/storefront/ui/*/`. Cross-app navigation is an external redirect, not an internal route.
7. **Policy composition.** Global policies live in `shared/types/<type>.wo` next to the `type` declaration (today). App-scoped policies live in `apps/X/app.wo` and AND with the global set — an admin app might relax a storefront policy for ops roles but can never relax beyond what the type's own policy permits.

## Angular parallels — what writeonce copies, what it doesn't

| Angular feature | Writeonce translation | Notes |
| --- | --- | --- |
| `nx workspace` / `angular.json` | Root `wo.toml` with `[workspace] apps = [...], shared = [...]` | Path references, not package registry |
| `apps/<app>/` | `apps/<app>/` with `app.wo` + `ui/` + local `types/` + local `logic/` | 1:1 naming |
| `libs/<lib>/` | `shared/<lib>/` | Used `shared/` instead of `libs/` — matches the more common monorepo idiom (Nx defaults to `libs`, but `shared` is clearer for this audience) |
| `<component>.ts` + `.html` + `.scss` | `<screen>.wo` + `.htmlx` + `.css` under `ui/<screen>/` | One-directory-per-screen |
| `ng build <app>` | `wo build apps/<app>` | Per-app binary output |
| Dependency injection | Service-block resolution — `service rest` blocks in shared types are callable from any app by import | No runtime DI container; bindings are resolved at compile time |
| RxJS observables | LIVE subscription frames on a WebSocket | Declarative `live` attribute instead of imperative `.subscribe(...)` |
| Zone.js change detection | Per-row delta dispatch + field-level `wo:bind` | No full-tree change detection — only the rows/fields the delta names get repainted |
| `HttpClient` | Built-in wire-protocol client inside the app binary | No separate library to import; always present |

### What we don't copy

- **No TypeScript.** Authoring is `.wo` (for logic) + `.htmlx` (for templates) + `.css`. If a page needs bespoke JS interactivity beyond what `wo:bind` covers, a `<script>` tag inside `.htmlx` is fine — but the client runtime itself is vanilla JS, not a framework.
- **No component library split.** Angular's `@angular/core`, `@angular/common`, etc. are a package hierarchy. Writeonce's runtime is one binary; "components" are just shared `.htmlx` partials under `shared/components/`.
- **No decorator metadata / reflect-metadata.** Rust macros + compile-time codegen do the same work.

## Reference materials

Read before writing each sub-phase:

| Source | Why |
| --- | --- |
| [`reference/crates/wo-htmlx/src/parser.rs`](../../../reference/crates/wo-htmlx/src/parser.rs) + [`render.rs`](../../../reference/crates/wo-htmlx/src/render.rs) | The v1 template engine's exact surface — what parses, what renders, what the AST looks like. ~500 LOC total. |
| [`templates/article.htmlx`](../../../templates/article.htmlx), [`templates/home.htmlx`](../../../templates/home.htmlx) | Concrete usage of the v1 format — how `{{path}}` and `data-bind` actually read in real templates. |
| [`docs/examples/ecommerce/ui/{storefront,order_tracker,admin_orders}.wo`](../../examples/ecommerce/ui/) | The `##ui` side — what the declarative DSL promises to produce. These screens are the target of the first compiler pass. |
| [`docs/runtime/database/06-lowcode-fullstack.md`](../../runtime/database/06-lowcode-fullstack.md) | Phase 6's full-stack block spec — `##ui`, `##app`, `##policy`, `##service`, `##logic` — already designed but not yet compiled. |
| [`docs/runtime/database/04-client-api.md`](../../runtime/database/04-client-api.md) | Phase 4's wire protocol — what the per-app binary speaks to the shared DB over. |
| [Nx monorepo docs](https://nx.dev/concepts/more-concepts/why-monorepos) | Background on the apps/libs split pattern; shape of `nx.json`. |

## Target layout

```
docs/examples/ecommerce/
├── wo.toml                      # workspace — lists apps, names the shared DB port
├── shared/                      # imported by any apps that need it
│   ├── types/
│   │   ├── customer.wo          # Customer + role union + policy read/write
│   │   ├── product.wo           # Product + inventory + similar_to graph
│   │   ├── order.wo             # Order + line_items + lifecycle triggers
│   │   └── purchase.wo          # link Customer -> Product
│   ├── logic/
│   │   └── checkout.wo          # fn checkout / mark_paid / mark_shipped
│   └── components/              # reusable .htmlx partials
│       ├── layout.htmlx         # top-level page chrome
│       ├── header.htmlx
│       ├── money.htmlx          # {{> money amount=x}} → $x.xx
│       └── order-row.htmlx      # used by both storefront and admin
├── apps/
│   ├── storefront/              # customer-facing; no admin routes
│   │   ├── wo.toml              # declares `shared = ["../shared"]`
│   │   ├── app.wo               # routes: / → home, /product/:sku → product-detail
│   │   ├── logic/
│   │   │   └── cart.wo          # app-local: fn add_to_cart, fn remove_from_cart
│   │   ├── types/
│   │   │   └── cart.wo          # type Cart { lines: [CartLine], ... } — not shared
│   │   └── ui/
│   │       ├── home/
│   │       │   ├── home.wo      # ##ui #home — declarative spec
│   │       │   ├── home.htmlx   # optional hand-written override
│   │       │   └── home.css
│   │       └── product-detail/
│   │           └── product-detail.wo
│   └── admin/
│       ├── wo.toml
│       ├── app.wo               # routes: /orders → orders, /inventory → inventory; gated role Admin|Ops
│       ├── logic/
│       │   └── fulfillment.wo   # app-local: fn ship_order calls shared.mark_shipped
│       └── ui/
│           ├── orders/
│           │   ├── orders.wo    # ##ui #admin-orders, live: true
│           │   └── orders.htmlx # hand-tuned layout overrides the auto-generated
│           └── inventory/
│               └── inventory.wo
└── tests/                       # workspace-level integration
    └── cross-app.test.wo        # a checkout from storefront visible in admin live feed
```

Compile outputs:
```
target/wo/
├── storefront       # ~12 MB static binary — app.wo compiled + ui/ templates baked in
├── admin            # ~12 MB static binary
└── db               # the `wo db` server (shared by all apps)
```

## `.htmlx` with live subscriptions — target format

v1 carries forward unchanged:

```htmlx
<h1>{{article.title}}</h1>
<ul>
  {{#each articles}}
    <li><a href="/article/{{slug}}">{{title}}</a></li>
  {{/each}}
</ul>
{{> layout.footer}}
```

Two new constructs for live:

```htmlx
<!-- Subtree bound to a LIVE query; client subscribes at page load -->
<wo:live source="Order{ status != Cancelled }" sort="placed_at desc" key="id">
  <table class="orders">
    <thead><tr><th>#</th><th>Status</th><th>Customer</th><th>Total</th></tr></thead>
    <tbody>
      {{#each rows}}
        <tr data-key="{{id}}">
          <td>{{id}}</td>
          <td wo:bind="status" class="status-{{status}}">{{status}}</td>
          <td>{{customer.name}}</td>
          <td>{{> money amount=total}}</td>
        </tr>
      {{/each}}
    </tbody>
  </table>
</wo:live>
```

Semantics:
- `<wo:live source="...">` emits a `LIVE <source>` query registration at SSR time. The initial result renders the `{{#each rows}}` body.
- The compiler also emits a JSON manifest (in a `<script data-wo-manifest>` tag) telling the client runtime which DOM id maps to which row key and what fields are `wo:bind`-ed.
- On page load, the client runtime opens a WebSocket back to the app, subscribes, and processes delta frames: `Insert` appends a row, `Update` finds `[data-key="<id>"]` and replaces `wo:bind`-ed cells, `Delete` removes the row.
- `wo:bind="field"` on any element tells the runtime "this element's text content reflects `row.field`"; delta Updates patch it in place.

## Sub-phase sequence

Each lands as its own plan doc under `docs/plan/ui/`. No code yet — this master plan outlines the order.

| # | File | Goal |
| --- | --- | --- |
| `01` | `01-htmlx-format-spec.md` | Nail down the exact `.htmlx` grammar — everything v1 has plus `<wo:live>` and `wo:bind`. Includes a manifest-emission spec so the client knows what to subscribe to. |
| `02` | `02-ui-compiler.md` | `##ui` → `.htmlx` compiler. Walks the parsed Phase-6 block and emits the template with the right `<wo:live>` / `{{#each}}` / `wo:bind` skeleton. Falls back gracefully when a hand-written `.htmlx` exists beside the `.wo`. |
| `03` | `03-client-runtime.md` | ~20 KB vanilla-JS runtime bundled with the app binary. Parses `<script data-wo-manifest>`, opens WebSocket, handles `snapshot`/`insert`/`update`/`delete` frames, patches DOM by `data-key` + `wo:bind`. |
| `04` | `04-workspace-layout.md` | Concrete refactor of `docs/examples/ecommerce/` from the current flat shape into `shared/` + `apps/*`. Defines `wo.toml` workspace grammar. |
| `05` | `05-per-app-binaries.md` | `wo build apps/X` produces one static binary per app. Each contains its own types/logic/ui + the shared dirs it imports. Shared DB connection is configured via `WO_DB` env var. |
| `06` | `06-shared-db-daemon.md` | `wo db serve` — headless database daemon. Per-app authn (API key per app), per-app connection scope. Apps see only types their policy allows. |
| `07` | `07-per-app-policies.md` | App-scope policy composition — global `policy read ...` on a type AND app-local `policy` in `app.wo` ⇒ effective policy = AND of both. Admin app's relaxations, storefront's restrictions. |

## Verification

After all seven sub-phases land:

| Target | Measure |
| --- | --- |
| `wo build apps/storefront` succeeds, produces one static binary | `file target/wo/storefront` → ELF, `ldd` shows only libc |
| `wo build apps/admin` succeeds | same |
| `wo db serve` + `storefront --db wo://localhost:5555` + `admin --db wo://localhost:5555` all run simultaneously | three processes, three ports, one data directory |
| Admin live orders table updates within 100 ms of a checkout on the storefront | Open `/admin/orders` in a browser, fire `POST /api/fn/checkout` against storefront's wire port, observe DOM patch |
| Customer's Order visible in their storefront order tracker but not to other customers; admin sees all | Policy round-trip |
| `wo run apps/storefront` serves hand-written `home.htmlx` if present, falls back to `##ui #home` generation if not | File-presence-based dispatch |
| `curl http://localhost:8080/healthz` from each app process | `200 ok` — standard liveness across the tracks |

## Non-scope

- **No TypeScript, no JSX.** `.htmlx` is HTML + Mustache + two `wo:` tags. The client runtime is 500 lines of vanilla JS.
- **No build-time Angular-style bundling.** No Webpack, no esbuild, no tree-shaking. The client JS is a pre-compiled static artifact inside each app binary.
- **No hot module reload in production.** `wo dev` reloads in development (inotify watches `apps/*/ui/`); production binaries don't self-reload.
- **No cross-app shared session state.** Each app authenticates independently. Shared identity is the customer row in the shared DB — both apps see the same user, but each app issues its own session token.
- **No dynamic shared-library linking between apps.** Sharing happens at source level (`shared/` dirs imported by path). Each binary is a fully-static blob.
- **No React/Vue compatibility layer.** If a downstream app wants those, they sit outside the writeonce runtime and talk to the shared DB over the wire protocol — same as any other client.

## Cross-references

- [`../09-concurrency-scaleout.md`](../09-concurrency-scaleout.md) — when the shared DB daemon needs to handle 10k connections across multiple apps, that plan's thread-per-core model applies to the daemon process.
- [`../assembly/02-writeonce-stance.md`](../assembly/02-writeonce-stance.md) — still no asm. The client runtime is vanilla JS, no WASM.
- [`../../runtime/database/06-lowcode-fullstack.md`](../../runtime/database/06-lowcode-fullstack.md) — Phase 6's full-stack block spec that this track implements.
- [`../../runtime/database/04-client-api.md`](../../runtime/database/04-client-api.md) — the wire protocol per-app binaries speak to the shared DB over.
- [`../../examples/ecommerce/ui/admin_orders.wo`](../../examples/ecommerce/ui/admin_orders.wo) — the motivating workload: a live ops table bound to the order stream.
- [`reference/crates/wo-htmlx/`](../../../reference/crates/wo-htmlx/) — the template engine ~90% of this track will reuse.
- [`templates/`](../../../templates/) — v1 blog's actual `.htmlx` files; the format this track extends.
