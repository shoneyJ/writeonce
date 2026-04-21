# Phase 6 — Low-Code Full-Stack: `.wo` as an Application Language

> Expand `.wo` from a query language into a declarative application DSL — schema, services, UI, business logic, and authorization in one language, compiled into a single binary.

**Previous**: [Phase 5 — Go Client SDK](./05-go-sdk.md) | **Index**: [database.md](../database.md)

---

Up to this point `.wo` is a **query language**. The next move is to expand it into an **application language** — a declarative, low-code/no-code DSL in the shape of **SAP Core Data Services (CDS)**: one language, one file extension, one compilation pipeline that produces database schema, service endpoints, UI screens, and business logic from the same source tree.

The reference precedent is SAP CDS, where a small amount of `.cds` code declares:

- Entities (tables), types, associations (relationships)
- Services that project entities to OData/REST
- UI annotations (`@UI.LineItem`, `@UI.Facet`) that drive SAP Fiori rendering
- Actions, functions, and authorization rules

From those declarations, SAP generates a full running application — data model, REST API, CRUD UI, authorization layer — with the developer writing almost no imperative code. `.wo` aims at the same target, for the same reason: **most enterprise and e-commerce applications are 90% CRUD on structured data with live views; declaring what you want and letting the runtime generate the rest is faster than hand-writing it**.

## Two Authoring Styles — Type-Attached vs Standalone

Behavior declarations fall into two groups:

| Group | Blocks | Authoring style |
| --- | --- | --- |
| **Behavior ON an entity** | `policy`, `on <event>`, `service` | **Type-attached** — declared inside the `type` block they govern. One name-resolution rule, zero cross-file coupling for single-entity behavior. |
| **Cross-entity composition** | `##ui`, `##app`, `##logic` that spans entities | **Standalone** — a screen composes multiple entities via `source:`; an app manifest names routes; a workflow that touches both orders and inventory needs its own block. |

Type-attached is the default — and the preferred authoring style because the schema layer ([Phase 2](./02-wo-language.md)) already names entities, and `policy read when author == $session.user` is most legible next to the `author: ref User` declaration it references.

Both forms compile to the same runtime model. A type-attached `policy` block is de-sugared into the same planner rewrite rule as a standalone `##policy` block — splitting is an authoring convenience.

## Project Layout

Convention over configuration. A `.wo` project is a tree of `.wo` files, each in a role-specific directory. The compiler discovers files by path.

```
myapp/
├── app/
│   ├── database/                 # schema — `type` declarations (with inline
│   │   ├── article.wo            #   policy, on, service blocks per type)
│   │   ├── user.wo
│   │   └── order.wo
│   ├── ui/                       # standalone screens — ##ui
│   │   ├── list.wo
│   │   ├── detail.wo
│   │   ├── form.wo
│   │   └── dashboard.wo
│   ├── logic/                    # cross-entity workflows — ##logic
│   │   └── order-workflow.wo
│   ├── auth/                     # cross-entity / session-level policies —
│   │   └── policies.wo           #   ##policy for things that don't fit on a type
│   ├── api/                      # service bundles that expose many types —
│   │   └── services.wo           #   ##service for multi-entity APIs
│   └── app.wo                    # root: name, routes, theme, i18n
├── migrations/                   # generated, versioned schema migrations
├── static/                       # hand-written assets (images, custom CSS)
└── wo.toml                       # project metadata
```

Most single-entity behavior lives next to the `type` in `database/`; `logic/` and `auth/` and `api/` are for the cross-entity cases. Every file contributes to a **single compiled model**. Splitting is for humans; the runtime sees one graph of declarations.

## File Type Examples

**`app/database/article.wo`** — one `type` declaration covers relational fields, embedded document, graph edges, **and** the per-entity policy/triggers/service:

```wo
type Article {
  id:         Id
  sys_title:  Slug @unique
  title:      Text
  published:  Bool = false
  author:     ref User                              -- foreign key
  meta: {                                           -- embedded document
    tags:    [Text]
    excerpt: Text
    body_md: Markdown
    reviews: [{ user: ref User, stars: Int, body: Markdown, at: Timestamp }]
  }
  created_at:    Timestamp = now()
  published_at:  Timestamp?

  related:       multi Article  @edge(:RELATED_TO)
  prerequisites: multi Article  @edge(:PREREQUISITE)

  -- type-attached policy — replaces a separate ##policy block for
  -- the single-entity case
  policy read  when published == true
  policy read  for role editor
  policy read  for role owner   when author == $session.user
  policy write for role editor
  policy write for role owner   when author == $session.user
  policy delete for role admin

  -- type-attached trigger — fires inside the transaction on commit
  on update when old.published == false and new.published == true
    do set self.published_at = now()
    do emit "article.published"(self)
    do enqueue "send-subscriber-emails" with { article_id: self.id }

  -- type-attached service — exposes CRUD + subscribe on a REST path
  service rest "/api/articles" expose list, get, create, update, delete, subscribe
}

type User { ... }   -- authors; AUTHORED is derivable from Article.author via backlink
```

The compiler emits the underlying `##sql` relational row, `##doc` embedded structure, and `##graph` edges from the single `type` declaration. `multi Article @edge(:RELATED_TO)` declares a zero-property graph edge whose direction and label match the original graph sketch; `ref User` emits a foreign-key column in the relational store. Inverses (`User.articles: backlink Article.author`) generate an `AUTHORED` edge — or a plain inverse column, at the planner's discretion.

**`app/ui/list.wo`** — a live list view; renders to HTML, wires subscriptions automatically:

```wo
##ui
#article-list
    title: "Articles"
    source: article
    live: true                             -- auto-subscribes via LIVE query

    filter:
        published = true

    columns:
        - sys_title    label: "Slug"
        - title        label: "Title"         searchable
        - meta.tags    label: "Tags"          renderer: tag-chips
        - created_at   label: "Created"       renderer: relative-date
        - author.name  label: "Author"        join: author_id -> user

    sort:
        default: created_at desc

    actions:
        row-click:  /article/:sys_title
        create:     /article/new              role: editor
        row-edit:   /article/edit/:id         role: editor | owner
        row-delete: delete                    role: editor      confirm: true

    pagination: 20
```

**`app/ui/detail.wo`** — a detail view composed of nested renderers, including a graph traversal:

```wo
##ui
#article-detail
    title: $article.title
    source: article
    key: sys_title
    live: true

    sections:
        - header:
            fields: [title, author.name, created_at]
        - body:
            renderer: markdown
            source: meta.body_md
        - related:
            title: "Related Articles"
            renderer: list
            source: Article{ sys_title == $key }.related    -- schema-layer path
            columns: [title, meta.excerpt]
            live: true
```

Screens stay standalone because they compose data from multiple types. The `source:` expression is a schema-layer path (preferred) or a raw `MATCH`/`SELECT` from the query layer — both resolve to the same planner input.

**`app/logic/order-workflow.wo`** — `##logic` is reserved for **cross-entity** triggers that don't belong on a single type. The on-article-published trigger lives on `type Article` (shown above); the on-order-placed trigger touches orders **and** every product in the line items, so it stays standalone:

```wo
##logic
#on-order-placed
    when: insert(Order)
    do:
        - validate: self.total == sum(self.line_items.*.qty * self.line_items.*.unit)
        - for-each item in self.line_items:
            - update: Product{ id == item.product.id }
                      set inventory.on_hand -= item.qty
                      assert inventory.on_hand >= 0
```

**`app/auth/policies.wo`** — reserved for **session-level** or **cross-entity** rules that don't fit on a single type. Most RBAC lives type-attached (see the `policy read ...` block on `type Article` above). A standalone `##policy` is useful for things like "admins bypass all row filters":

```wo
##policy
#admin-bypass
    applies_to: Article, Order, User
    when: role == admin
    effect: skip-row-filters
```

**`app/api/services.wo`** — reserved for **multi-entity** API bundles. Single-entity services live type-attached (see `service rest "/api/articles"` on `type Article` above). A bundle endpoint that exposes a curated subset or a custom aggregation goes here:

```wo
##service
#storefront
    path: /api/storefront
    protocols: [rest, graphql]
    expose:
        - Product     as products    operations: [list, get, subscribe]
        - Article     as articles    operations: [list, get]
        - categories: Product{ featured == true }.category    -- custom path
```

**`app/app.wo`** — root manifest:

```wo
##app
name: "writeonce"
version: 1
theme: "light"
i18n: [en, de]

routes:
    /                 -> ui.article-list { filter: { published: true } }
    /article/:slug    -> ui.article-detail { key: $slug }
    /admin/articles   -> ui.article-list { role: editor }
```

## Compilation Pipeline

The `.wo` compiler loads every `.wo` file in the tree and emits a single runtime bundle:

```
  app/**/*.wo
       │
       ▼
  ┌─────────────┐
  │   Parser    │  one grammar, all block types
  └─────────────┘
       │
       ▼
  ┌─────────────┐
  │  Analyzer   │  name resolution across files, type check, policy check
  └─────────────┘
       │
       ▼
  ┌─────────────────────────────────────────┐
  │     Unified Application Model (AST)     │
  └─────────────────────────────────────────┘
       │              │              │           │            │
       ▼              ▼              ▼           ▼            ▼
  ┌────────┐   ┌──────────┐   ┌─────────┐  ┌────────┐   ┌──────────┐
  │ Schema │   │ Services │   │   UI    │  │ Logic  │   │ Policies │
  │ (DDL)  │   │(endpoints)│   │(widgets)│  │(hooks) │   │  (rbac)  │
  └────────┘   └──────────┘   └─────────┘  └────────┘   └──────────┘
       │              │              │           │            │
       ▼              ▼              ▼           ▼            ▼
  Migrations    HTTP/GraphQL     HTML / JSON   Commit-     Planner
  applied to    / native          manifest     time         filters
  engine        dispatch          (SSR or      triggers     merged into
                                  client)                   every query
```

Each leaf maps to a runtime component from the earlier phases:

- **Schema → engine**: the `.wo` DDL goes to the in-memory engine from [Phase 3](./03-inmemory-engine.md). Migrations rebuild the schema; data is preserved where possible.
- **Services → endpoints**: HTTP/GraphQL/native dispatch via the wire-protocol layer from [Phase 4](./04-client-api.md).
- **UI → widgets**: a new component — UI declarations compile to a render tree. Default renderer is server-rendered HTML (SSR) with a thin client runtime for subscription wiring. `live: true` on any UI node issues a `LIVE SELECT`/`LIVE MATCH` through the subscription engine and the client runtime swaps DOM fragments on each delta.
- **Logic → hooks**: triggers compile to server-side procedures invoked by the transaction coordinator on matching commits. Same transaction as the mutation — ACID across the hook's writes.
- **Policies → planner**: predicates intersected with every query/subscription at registration time (already covered in [Phase 4](./04-client-api.md)).

## How Subscriptions Wire Themselves

The key low-code payoff: a UI developer never writes subscription code. They write `live: true` on a list or detail, and:

1. The UI compiler inspects the view's `source` (table, document query, or graph `MATCH`).
2. It generates a `LIVE` query that returns exactly the fields the UI displays.
3. It emits a subscription handle in the rendered page.
4. The client runtime opens a WebSocket, registers the subscription, and binds incoming deltas to DOM fragments by key.
5. When a row changes in the engine, the delta flows: engine → subscription registry → client runtime → DOM patch.

Zero hand-written subscription code. Zero polling. Adding a new live column to a list is one line in a `.wo` file.

## Generated Application Stack

For the writeonce schema above, `sa build` produces:

| Layer | Generated From | Output |
| --- | --- | --- |
| Database schema | `app/database/*.wo` | In-memory engine arenas + migrations |
| REST / GraphQL / native endpoints | `app/api/*.wo` + `app/database/*.wo` | HTTP handlers, OpenAPI spec, GraphQL SDL |
| SSR HTML | `app/ui/*.wo` + routes in `app.wo` | Per-route renderers compiled into the server binary |
| Client runtime | `app/ui/*.wo` | Small JS bundle: subscription client + DOM patcher + form binding |
| Admin UI | All of the above | Auto-generated CRUD screens for every `##sql`/`##doc` entity (override any with a `##ui` block) |
| Typed SDKs | `app/database/*.wo` | Go/TypeScript/Rust clients per [Phase 5](./05-go-sdk.md) |
| Migrations | Schema diff vs. current database | Versioned forward/backward migrations in `migrations/` |
| Observability | Everything | Structured logs, query metrics, subscription lag dashboards |

Equivalent hand-written stack: schema (SQL), ORM models, REST controllers, GraphQL schema + resolvers, HTML templates, client JS, subscription plumbing, migrations, admin CRUD, SDKs. Likely **10,000–50,000 lines** for a small e-commerce site. `.wo` target: **~500 lines** across the `app/` tree.

## Developer Experience — The CLI

```bash
sa init myapp                      # scaffold with sensible defaults
cd myapp
sa dev                             # live-reload server — edit .wo, see changes instantly
sa build --target linux-amd64      # single static binary with everything inside
sa migrate --plan                  # preview schema migrations
sa migrate --apply                 # apply migrations
sa gen sdk --lang go --out ./sdk   # emit typed client
sa deploy                          # upload to a running runtime node
```

`sa dev` is the make-or-break command. It must:

- Detect `.wo` changes via inotify (same mechanism as `wo-watch`)
- Recompile incrementally (~50 ms for a single-file change)
- Hot-swap UI renderers without losing client state
- Run schema migrations in a sandbox, surface conflicts before applying
- Keep open subscriptions alive across reloads (re-register on connect)

## Comparison With Other Declarative Full-Stack Systems

| System | Schema | UI | Logic | Live queries | Single binary |
| --- | --- | --- | --- | --- | --- |
| **SAP CDS** | `.cds` entities | `@UI` annotations → Fiori | Actions, functions | No (request-response) | No (Java/Node runtime) |
| **Hasura** | Reads from Postgres | No (external UI) | Actions, event triggers | Yes (polling-based internally) | No |
| **Supabase** | Postgres schema | Auto-admin UI only | Edge functions, triggers | Yes (logical replication) | No |
| **Retool / AppSmith / Budibase** | External DB | Visual drag-and-drop | JS snippets | Partial | No |
| **Wasp** (`wasp-lang.org`) | `.wasp` + Prisma | React components | JS functions | No | No (Node + React) |
| **RedwoodJS** | `.sdl` + Prisma | React | JS | No | No |
| **Django + Admin** | Python models | Auto-admin only | Python views | No | No |
| **Phoenix LiveView** | Ecto schemas | HEEx templates | Elixir functions | **Yes** (native) | No (BEAM runtime) |
| **Anvil** (`anvil.works`) | Proprietary | Python drag-and-drop | Python | No | No |
| **`.wo`** (this design) | `type` DSL (unified) over `##sql`+`##doc`+`##graph` substrate | `##ui` declarations + type-attached | type-attached `on <event>` + `##logic` | **Yes** (native) | **Yes** |

The differentiators: **cross-paradigm schema** (no competitor unifies SQL + document + graph in one DDL), **engine-native subscriptions** (most bolt on a replication layer or poll), and **single static binary** as the deployment unit (no separate database process, no separate UI server, no separate message bus).

Closest philosophical precedents:

- **SAP CDS** for the declaration-first application language — the explicit inspiration.
- **Phoenix LiveView** for the subscription-wired UI model.
- **Wasp** for the `.wasp` → full stack compilation pipeline.
- **Django Admin** for the "generate CRUD from the model" reflex.

## Scope Addition

This is a compiler + a renderer + a UI toolkit on top of Phases 2–4. Rough new components:

| Component | Work |
| --- | --- |
| `type` DSL parser + schema compiler | Entity declarations → `##sql`/`##doc`/`##graph` physical schema; type-attached `policy`/`on`/`service` → same runtime components as standalone blocks |
| `##ui` grammar + analyzer | UI widget tree, field bindings, renderer dispatch |
| Trigger compiler | Type-attached `on <event>` + standalone `##logic`; both run inside txn coordinator |
| Policy compiler + planner integration | Type-attached `policy` + standalone `##policy` both intersected with every query at registration (some of this exists in Phase 2 already) |
| Service compiler + dispatch table | Type-attached `service` + standalone `##service`; endpoint registration at startup |
| UI render tree → SSR HTML | Template engine, layout system, component library (table, form, chart, etc.) |
| Client runtime (~50 KB JS) | Subscription client, DOM patcher, form binding, validation |
| Auto-admin UI | Generic CRUD screens per entity, override-able with `##ui` |
| Migration engine | Schema diffing, forward/backward migrations, online reshape for the in-memory engine |
| CLI (`sa` binary) | `init`, `dev`, `build`, `migrate`, `gen`, `deploy` |
| Dev-mode live reload | inotify + incremental compiler + client hot-swap |
| Hosted runtime | Optional — for `sa deploy` to work without self-hosting |

Rough effort on top of Phases 2–4: **12–24 months** with a small team, most of it in the UI compiler and client runtime — that's where the complexity lives, not in the language spec.

## Honest Framing

This section is the endgame, not the next step. The sensible build order:

1. Ship the engine ([Phase 2](./02-wo-language.md), in-memory + io_uring durability via [Phase 3](./03-inmemory-engine.md)) — query layer (`##sql`/`##doc`/`##graph`) only.
2. Ship the wire protocol + Go SDK ([Phase 4](./04-client-api.md) + [Phase 5](./05-go-sdk.md)).
3. Ship the **schema-layer `type` DSL** that compiles to the three paradigm blocks. From this point forward, authoring happens against types; the paradigm blocks become an artifact the compiler emits.
4. Add type-attached `service` (and standalone `##service` for bundles) — declarative endpoints.
5. Add type-attached `policy` (and standalone `##policy` for cross-entity rules) — declarative authorization.
6. Add type-attached `on <event>` triggers (and standalone `##logic` for cross-entity workflows) — declarative triggers.
7. Add `##ui` — declarative rendering. **This is where `.wo` becomes a low-code platform.**
8. Add the CLI, live-reload, admin UI.

Each step is shippable on its own. Every step after (3) converts imperative code developers are writing by hand into declarative code they write once. The value compounds: by step (7), a small e-commerce app is a ~500-line `.wo` tree instead of a ~50 KLOC TypeScript/Go/SQL repository.

The risk is the same as every low-code platform: the 20% of use cases outside the declarative model have to have an escape hatch. `.wo` reserves one: any `##ui` node can point to a custom server-rendered template, and any `##logic` block can call out to a host-language plugin (Go/Rust/Wasm). Without that escape hatch, low-code becomes no-code in the pejorative sense — you can build 80% of the app and the rest is impossible.

## Reference Implementations Worth Studying

- **SAP CDS** — <https://cap.cloud.sap/docs/cds/>. Read the CDS Language Reference cover to cover before designing `##ui`. Thirty years of ERP app-generation is condensed into that spec.
- **Wasp** — <https://github.com/wasp-lang/wasp>. Open-source `.wasp` → React + Node + Prisma compiler. Closest living sibling to `.wo`.
- **Phoenix LiveView** — <https://github.com/phoenixframework/phoenix_live_view>. The rendering-subscription loop done right in Elixir.
- **HTMX + Hyperscript** — <https://htmx.org>. Tiny client runtime that consumes server-rendered HTML fragments on events. Good model for `.wo`'s client bundle.
- **Retool / Budibase / Appsmith (open source)** — <https://github.com/Budibase/budibase>, <https://github.com/appsmithorg/appsmith>. Visual low-code; useful to see which UI primitives users actually ask for.
- **SurrealDB `define` syntax** — SurrealQL includes declarative `DEFINE TABLE`, `DEFINE FIELD`, `DEFINE EVENT` that are partway toward an application DSL. Worth studying for how much declaration fits inside a query language.
- **Django admin source** — `django/contrib/admin/`. The canonical "CRUD from models" implementation; read how it introspects schema to generate list/detail/edit views.
- **PocketBase** — <https://github.com/pocketbase/pocketbase>. Single-binary Go app with SQLite, auto-admin UI, realtime subscriptions. Proves the single-binary low-code model is buildable. `.wo` is what PocketBase would look like if its data model were multi-paradigm and its query engine were custom.

## Why This Belongs in This Series

The question that opened [Phase 1](./01-evaluation.md) — "should writeonce use a document or graph database?" — has now inverted. The answer drove past "no, use flat files" through "build your own query language" and "build your own ACID engine" and "build your own wire protocol" to arrive here: **a full-stack declarative application platform where the database, the subscriptions, the UI, and the business logic are one artifact compiled from one language**.

That is the actual ambition. Every earlier phase is a subcomponent of this one. Decide honestly whether the project is a blog engine that needed a graph index, or an application platform that happens to start as a blog engine. The answer determines which phases are scope and which are cautionary.
