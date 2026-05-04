# `ecommerce` — a sample writeonce **monorepo**

Two apps (customer **storefront** + ops **admin**) sharing one database, built from a common pool of types + business logic. Mirrors the Nx / Angular workspace pattern: `apps/*` for deployable binaries, `shared/*` for libraries imported across apps.

> This project is a **docs artifact** — illustrative `.wo` source showing what a production-shaped writeonce workspace looks like. The master plan for the compiler + client runtime + per-app build is at [`../../plan/ui/00-overview.md`](../../plan/ui/00-overview.md). Sub-phases UI/01–07 implement each piece.

## Layout

```
ecommerce/
├── wo.toml                                 # workspace manifest (apps[] + shared[] + database)
├── README.md                               # this file
│
├── shared/                                 # code imported by one or more apps
│   ├── types/
│   │   ├── customer.wo                     # role union, policy, service rest expose
│   │   ├── product.wo                      # inventory + similar_to graph
│   │   ├── order.wo                        # tagged-union status + line_items array
│   │   └── purchase.wo                     # link Customer -> Product
│   ├── logic/
│   │   ├── checkout.wo                     # fn checkout / mark_paid / mark_shipped
│   │   └── seed.wo                         # on-startup demo seed + admin-ops bypass policy
│   └── components/                         # reusable .htmlx partials (forward-looking — UI track)
│       ├── layout.htmlx                    # page chrome shared across apps
│       ├── money.htmlx                     # {{> money amount=total}}
│       └── order-row.htmlx                 # used by both orders tables
│
├── apps/                                   # one binary per app
│   ├── storefront/                         # customer-facing
│   │   ├── wo.toml                         # listen :8080, connect WO_DB
│   │   ├── app.wo                          # routes: / → home, /product/:sku, /cart, /orders
│   │   └── ui/
│   │       ├── home/
│   │       │   └── home.wo                 # product list, live inventory
│   │       ├── product-detail/             # (future)
│   │       └── orders/
│   │           └── orders.wo               # customer's own orders
│   │
│   └── admin/                              # ops dashboard
│       ├── wo.toml                         # listen :8081, connect WO_DB
│       ├── app.wo                          # role: Admin | Ops; routes: /orders
│       └── ui/
│           └── orders/
│               └── orders.wo               # live ops table with fulfillment actions
│
└── tests/                                  # workspace-level integration
    └── checkout_test.wo
```

Each app's UI screens live in their own directory (`apps/<app>/ui/<screen>/`) with the Angular-style one-directory-per-component pattern — `.wo` declarative spec today, `.htmlx` template + `.css` stylesheet once the UI track's sub-phase 01–02 land.

## What the two apps share

- **Types** (`shared/types/`). Both apps see the same `Customer` / `Product` / `Order` / `Purchase` definitions. Row-level policies inside each `type` block control who sees what — the storefront's authenticated customer sees their own orders; the admin app's Admin/Ops role sees everyone's.
- **Logic** (`shared/logic/`). The `checkout`, `mark_paid`, `mark_shipped`, and `release_inventory` functions in `shared/logic/checkout.wo` are callable from either app (subject to role policy). `shared/logic/seed.wo` runs once when the shared DB daemon starts.
- **Components** (`shared/components/`). `.htmlx` partials — layout chrome, money formatting, an order-row renderer — reusable from either app's templates.

## What's **not** shared (per-app)

- **`app.wo`** declares app-specific routes + role gate. Storefront has no `/admin/*` routes; admin has no `/cart` or `/product/:sku`.
- **`ui/`** is per-app. The storefront's `orders/orders.wo` (customer's own orders, filtered by session) and the admin's `orders/orders.wo` (all orders with fulfillment actions) are different screens — same underlying `Order` type, different UI + policy scope.
- Each app's `wo.toml` names its own listen port and its own DB API key.

## Running it

```bash
# 1. Start the shared DB daemon — headless, just the engine + wire protocol
wo db serve --data-dir ./data                        # listens on wo://127.0.0.1:5555

# 2. Start each app, pointing at the daemon
WO_DB=wo://127.0.0.1:5555 \
  STOREFRONT_DB_KEY=$ADMIN_TOKEN \
  wo run apps/storefront                             # HTTP on :8080

WO_DB=wo://127.0.0.1:5555 \
  ADMIN_DB_KEY=$ADMIN_TOKEN \
  wo run apps/admin                                  # HTTP on :8081
```

Browser:
- `http://localhost:8080/` → storefront home (product list, live inventory)
- `http://localhost:8081/orders` → admin live orders table

## Building binaries

```bash
wo build apps/storefront                             # → target/wo/storefront
wo build apps/admin                                  # → target/wo/admin
wo build --all                                       # everything in apps/
```

Each binary is a static ELF with only the app's own `app.wo` + `ui/` + the `shared/` it imports baked in. Drop any binary on a server next to a running `wo db serve` and it works.

## Stage 2 caveat

The current runtime at [`crates/rt/`](../../../crates/rt/) is a single-process Stage 2 prototype — it doesn't yet know about:

- `[workspace]` manifests (per-app build — UI sub-phase 05)
- The `wo db serve` daemon split (UI sub-phase 06)
- Per-app `api_key_env` authorisation scope (UI sub-phase 07)
- `.htmlx` compilation from `##ui` blocks (UI sub-phases 01–03)
- Per-app policy composition (UI sub-phase 07)

So `cargo run --bin wo -- run docs/examples/ecommerce` today walks the whole tree, finds every `.wo` file under `shared/` + `apps/`, parses the types, and serves the union REST API on :8080 — treating the monorepo as one giant app. Useful for exercising the types; not reflective of the production shape. See [`docs/plan/ui/00-overview.md`](../../plan/ui/00-overview.md) for the sub-phase sequence that gets each piece online.

## Comparison with the blog sample

The [`blog` sample](../blog/) is still a single-app layout (`types/`, `ui/`, `logic/` at the root) because the blog has exactly one front-end surface — there's no customer-vs-admin split. Both patterns are first-class; pick based on whether your schema serves one app or many. A flat single-app layout is a degenerate workspace with one `apps/` entry.

## Source pointers

- **Master plan:** [`../../plan/ui/00-overview.md`](../../plan/ui/00-overview.md)
- **Language spec the `##ui`/`##app`/`policy` blocks obey:** [`../../runtime/database/06-lowcode-fullstack.md`](../../runtime/database/06-lowcode-fullstack.md)
- **Wire protocol the app binaries speak to the DB daemon:** [`../../runtime/database/04-client-api.md`](../../runtime/database/04-client-api.md)
- **v1 template engine that `.htmlx` compilation will reuse:** [`../../../reference/crates/wo-htmlx/`](../../../reference/crates/wo-htmlx/)
- **Checkout transaction that's the canonical cross-paradigm test:** [`shared/logic/checkout.wo`](shared/logic/checkout.wo)
