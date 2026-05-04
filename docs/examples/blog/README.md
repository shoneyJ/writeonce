# `blog` — a sample writeonce app

A complete blogging website in **~200 lines of `.wo`** that creates a database, exposes REST + live-subscription endpoints, renders HTML pages, enforces row-level policies, and emits typed client SDKs.

> This project is a **docs artifact** — it illustrates the shape of a real `wo init`'d project. The `wo` toolchain referenced here is the one specified in [`../../runtime/wo-language.md`](../../runtime/wo-language.md); the engine is at prototype stage in [`../../../prototypes/wo-db/`](../../../prototypes/wo-db/).

## What it does

| Thing | How |
| --- | --- |
| Persists articles, authors, tags, comments | `type` declarations compiled to relational rows + embedded documents + graph edges |
| Serves 24 REST endpoints (CRUD + subscribe × 4 types) | `service rest` blocks on each type |
| Serves 4 web pages (list, detail, tag, admin) | `##ui` screens + route table in `app.wo` |
| Pushes live updates on every commit | `live: true` on screens + `LIVE` queries under the hood |
| Enforces "drafts hidden from anonymous readers" | `policy read anyone when published == true` |
| Bumps `published_at` automatically | `on update` trigger inside the transaction |
| Generates a typed Go client | `wo gen sdk --lang go` |

## Project layout

```
blog/
├── wo.toml                   # project manifest (like go.mod)
├── app.wo                    # routes, theme, startup hooks
├── types/
│   ├── author.wo             # Author type + per-type service/policy
│   ├── article.wo            # Article — all three paradigms in one type
│   ├── tag.wo                # Tag taxonomy
│   └── comment.wo            # Reader comments
├── styles/
│   ├── main.css              # global stylesheet (linked from app.wo styles:)
│   └── code-theme.css        # syntax highlighting tokens
├── ui/
│   ├── article_list.wo       # home page list view (live)
│   ├── article_detail.wo     # per-article page with comments + related
│   └── components/           # reusable components (.wo + .htmlx + .css per component)
│       ├── article-card.wo   # selector + typed inputs + styles:
│       ├── article-card.htmlx
│       ├── article-card.css
│       ├── comments.wo       # selector + source + actions + role + styles:
│       ├── comments.htmlx
│       └── comments.css
└── tests/
    └── article_test.wo       # `wo test` picks this up
```

No `main.wo` is needed — a pure type+service app auto-generates its entry point. Add `main.wo` if you need CLI args, background workers, or custom startup logic beyond the `on startup` hook in `app.wo`.

### UI: components vs. screens

The `ui/` tree separates concerns the way Angular separates `@Component` / template / parent:

- **Screens** (`ui/article_list.wo`, `ui/article_detail.wo`) declare a `##ui` block — they own the route, the page-level data source, and the section layout. They embed components by selector via `use: <name>` + `with: { ... }` and pass typed inputs.
- **Components** (`ui/components/*.wo`) declare a `##component` block with `template: 'foo.htmlx'`, typed `inputs:`, and — when the component owns its own query — its `source:`, `sort:`, `live:`, and `actions:`. No HTML.
- **Templates** (`ui/components/*.htmlx`) are pure presentation. They read from the component's `inputs` and from the rows produced by its `source`. No data-source declarations, no role checks.

Screens never inline a component's HTML or its query; templates never declare data sources. Each `.wo` paired with one `.htmlx` is the unit of UI reuse.

### Styling

CSS is declared at two scopes; in both cases the compiler emits the `<link>` tags into the SSR layout and serves the files under `/static/`:

- **App-level (global)** — `##app styles: [...]` in `app.wo` lists global stylesheets. Resolved relative to `./styles/`. Linked once, in declaration order, on every page.
- **Component-scoped** — `##component styles: [...]` lists CSS files alongside the component. The compiler rewrites bare selectors in those files to `[data-component="<selector>"] <rule>`, using the `data-component` attribute the templates already emit. Rules cannot leak outside the component subtree, so two components can both declare `.title` without colliding.

A component's CSS is only fetched on pages that embed the component. Global styles always load. Neither layer requires a build step — `wo run` serves the files as-is.

## Run it

```bash
$ cd docs/examples/blog
$ wo run
[wo] parsing: 7 files, 4 types, 2 ui screens
[wo] compiling schema: 4 sql tables, 1 doc collection, 3 graph edge types
[wo] starting runtime (engine: in-memory, data_dir: ./data)
[wo] on startup: seed_admin() — inserted admin@example.com
[wo] HTTP listening on :8080

  GET    /api/articles              list
  GET    /api/articles/:id          get
  POST   /api/articles              create
  PATCH  /api/articles/:id          update
  DELETE /api/articles/:id          delete
  WS     /api/articles/live         subscribe
  GET    /api/authors               list
  GET    /api/authors/me            me
  WS     /api/authors/live          subscribe
  GET    /api/tags                  list
  GET    /api/comments              list
  POST   /api/comments              create
  WS     /api/comments/live         subscribe
  ... (and the rest)

  GET    /                          ui.article-list
  GET    /article/:slug             ui.article-detail
  GET    /tag/:slug                 ui.article-list (filtered)
  GET    /admin                     ui.article-list (role: Admin)
```

## Exercise the REST API

```bash
# Create an author (requires admin session — see auth docs; stub'd here for brevity)
$ curl -X POST localhost:8080/api/authors \
    -H "Content-Type: application/json" \
    -H "Authorization: Bearer $ADMIN_TOKEN" \
    -d '{"email":"alice@example.com","handle":"alice","display":"Alice","role":"Author"}'
{"id":2,"email":"alice@example.com","handle":"alice",...}

# Create an article as that author
$ curl -X POST localhost:8080/api/articles \
    -H "Authorization: Bearer $ALICE_TOKEN" \
    -d '{
      "slug": "hello",
      "title": "Hello, writeonce",
      "author": 2,
      "meta": {"excerpt":"First post","body_md":"# Hi\n\nHello."},
      "published": true
    }'
{"id":1,"slug":"hello","title":"Hello, writeonce","published_at":"2026-04-17T12:00:00Z",...}

# List published articles (public — no token)
$ curl localhost:8080/api/articles
[{"id":1,"slug":"hello","title":"Hello, writeonce",...}]

# Filter by tag (via the query layer)
$ curl 'localhost:8080/api/articles?tags.slug=rust'
[...]
```

## Subscribe to live updates

```bash
$ websocat ws://localhost:8080/api/articles/live?published=eq.true
{"kind":"snapshot","rows":[{"id":1,"slug":"hello",...}]}

# Now in another terminal, update article 1. The open socket receives:
{"kind":"update","id":1,"old":{"title":"Hello, writeonce"},"new":{"title":"Hello!"}}
```

No polling. The subscription predicate was registered at connect time; the engine's commit path emits the delta directly.

## Generate a Go client

```bash
$ wo gen sdk --lang go --out ./client
[wo] reading types from ./types/
[wo] writing ./client/sdk.go (4 types, 16 endpoints, 4 subscriptions)
```

Use it:

```go
import "github.com/you/blog/client"

c, _ := client.Connect(ctx, "wo://localhost:8080", client.WithToken(token))

// Typed query
articles, _ := c.Articles.List(ctx, client.Where{Published: ptr(true)})

// Typed subscription — deltas arrive on a channel
sub, _ := c.Articles.Subscribe(ctx, client.Where{Published: ptr(true)})
for d := range sub.C {
    switch d.Kind {
    case client.Insert:
        fmt.Printf("new article: %s\n", d.Row.Title)
    case client.Update:
        fmt.Printf("updated: %s\n", d.Row.Slug)
    }
}
```

## Run the tests

```bash
$ wo test
=== tests/article_test.wo ===
  create and fetch by slug                               OK (3ms)
  policy blocks public read of unpublished drafts        OK (4ms)
  graph traversal: related articles                      OK (7ms)
  live subscription receives delta on commit             OK (12ms)

PASS  4/4 tests, 0 failures (26ms)
```

Each `test` block runs against an isolated engine snapshot that's rolled back at the end — no setup/teardown code needed.

## Build a production binary

```bash
$ wo build --target linux-amd64 --out bin/blog
[wo] static binary: bin/blog (14 MB, database + HTTP + subscription engine embedded)
$ ./bin/blog
[wo] HTTP listening on :8080
```

One binary, no dependencies. Copy it to a server, run it, done. The database file lives in `./data/` relative to the binary; the WAL ensures crash safety ([Phase 3](../../runtime/database/03-inmemory-engine.md)).

## What to read next

- [`../../runtime/wo-language.md`](../../runtime/wo-language.md) — the user-facing language overview this project builds on
- [`../../runtime/database/02-wo-language.md`](../../runtime/database/02-wo-language.md) — the two-layer language spec (schema + query layers)
- [`../../runtime/database/06-lowcode-fullstack.md`](../../runtime/database/06-lowcode-fullstack.md) — the `##ui`/`##policy`/`##service`/`##app` block spec
- [`../../../prototypes/wo-db/`](../../../prototypes/wo-db/) — the C++ prototype that runs the query-layer subset today
