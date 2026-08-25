# site — writeonce.de

The language tutorial, served BY the language. One binary carries the HTTP
server, the router, the pages and the database; the chapters you read are
rows in a `@table`, the markup is built by the `wo-html` dependency, and
the whole thing is chapter 9's own example.

```
[deps]
framework = { git = "https://github.com/shoneyj/writeonce-framework", rev = "v0.1.0" }
html      = { git = "https://github.com/shoneyj/wo-html",             rev = "v0.1.0" }
```

## Run it

```
woc . && SITE_TOKEN=change-me WO_DATA=./data ./target/site 8080
```

- `GET /` — the chapter index; `GET /ch/<slug>` — one chapter.
- `POST /admin/ch/<slug>` — edit a chapter (`title`/`body`, form-encoded,
  `authorization: Bearer $SITE_TOKEN`). Edits are WAL-durable under
  `WO_DATA` and replay on restart — that is chapter 6, demonstrated by
  the site that teaches it.
- Without `WO_DATA` the chapters live in RAM and reseed on every boot.

The acceptance gate is `just site` (scripts/site-accept.sh): two file://
dep remotes, build, the page matrix, 401, an authed edit, SIGTERM, and
the edit surviving a restart.

## The file map

MVC, laid out exactly like the program template
([`docs/examples/shop`](../shop/README.md)) so the two read the same way:

| this app | layer |
| --- | --- |
| `types.wo` | MODEL — the `Chapter` `@table`, and seed-if-empty |
| `content.wo` | the nine chapter bodies + `seed_chapters()` |
| `layout/` | the chrome: `AppShell` (+ the two named widths), header, footer, `html_error` |
| `home/`, `chapter/`, `admin/`, `health/` | one module per feature: its `view.wo` (components: fields in, Text out) and its `controller.wo` (query the model, fill the components, answer a `Resp`) |
| `main.wo` | bootstrap: seed, routes, serve — nothing else |

Every `render()` makes its class a component (wo-html's structural
`Component`). `HomePage` and `ChapterPage` each take the chapter nav as
an already-built child component in a slot, so neither knows what a
chapter is; `ChapterNav` is therefore literally the same component on the
homepage and on every chapter page, differing only by which `ord` is
current.

The seam that keeps it honest: **every query lives in a controller.**
`chapter_links()` sits in `chapter.controller.wo` and hands the views a
`multi ChapterLink` projection — no component in this sample touches the
database, and wo-html contains no query at all.

One feature = one directory = one module, holding that feature's view
and its controller. The `@table` lives in `types.wo` at the root and is
reachable from every feature module without being exported — a CLASS
crosses module lines, only a free `fn` is module-scoped (`WO-E210`).
That is why the query both pages need is `Chapters.links()`, a `static
fn` on a root class, rather than a free function one of them would have
to import from the other.

## writeonce.de deployment

The framework speaks HTTP/1.1 keep-alive and no TLS by design — terminate
TLS at the proxy and forward:

```
server {
  server_name writeonce.de;
  listen 443 ssl http2;         # certs via certbot/acme
  location / { proxy_pass http://127.0.0.1:8080; }
}
```

Run the binary under systemd (`Restart=on-failure`, `Environment=SITE_TOKEN=...`,
`Environment=WO_DATA=/var/lib/writeonce-site`); SIGTERM drains cleanly.

## What it demonstrates

Chapters 1–9 teach the language (values, containers, classes, optionals,
tables, actors, deps, serving); the app itself exercises the framework's
routing/:params, the Logging middleware, bearer auth (mechanism from
`http/auth.wo`, policy here), `form_values`, `@table` + query + update by
assignment, and `wo-html`'s escaping/builders/Tailwind-style utility
sheet — self-contained pages, no CDN, no JS, no build step.
