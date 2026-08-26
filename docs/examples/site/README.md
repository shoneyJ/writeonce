# site — writeonce.de

The language tutorial, served BY the language. One binary carries the HTTP
server, the router, the pages and the database; the chapters you read are
rows in a `@table`, the markup is built by the `writeonce-view` dependency, and
the whole thing is chapter 9's own example.

```
[deps]
porch     = { git = "https://github.com/shoneyj/porch", rev = "v0.1.0" }
view      = { git = "https://github.com/shoneyj/writeonce-view",             rev = "v0.1.0" }
```

## Run it

```
woc . && SITE_TOKEN=change-me WO_DATA=./data ./target/site 8080
```

It binds loopback by default. To reach it from another machine while
developing, name the interface:

```
SITE_HOST=0.0.0.0 SITE_TOKEN=change-me WO_DATA=./data ./target/site 8080
```

- `GET /` — the homepage; `GET /ch/<slug>` — one chapter.
- `GET /install` — the installation guide; `GET /packages` and
  `GET /packages/<name>` — the package catalogue with copy-paste
  `[deps]` lines and usage.
- `GET /favicon.svg` — the mark, inline SVG, no asset pipeline.
- `GET /dl/<file>` — release tarballs, served by the framework's
  `StaticFiles` from `$WO_DIST` (default `./dist`, where `just dist`
  writes them).
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
| `home/`, `chapter/`, `install/`, `packages/` | one module per feature: its `view.wo` (components: fields in, Text out) and its `controller.wo` (query the model, fill the components, answer a `Resp`) |
| `admin/`, `health/`, `favicon/` | controller-only features — a redirect, a text probe, an SVG |
| `layout/logo.wo` | the mark as inline SVG: one source for the nav brand and `/favicon.svg` |
| `main.wo` | bootstrap: seed, routes, serve — nothing else |

Every `render()` makes its class a component (writeonce-view's structural
`Component`). `HomePage` and `ChapterPage` each take the chapter nav as
an already-built child component in a slot, so neither knows what a
chapter is; `ChapterNav` is therefore literally the same component on the
homepage and on every chapter page, differing only by which `ord` is
current.

The seam that keeps it honest: **every query lives in a controller.**
`chapter_links()` sits in `chapter.controller.wo` and hands the views a
`multi ChapterLink` projection — no component in this sample touches the
database, and writeonce-view contains no query at all.

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

### What to copy to the host

The binary is self-contained — VM, bytecode and database engine are
inside it — but two directories are read at RUNTIME and must travel
with it:

```
/srv/writeonce-site/
  site                     the binary (docs/examples/site/target/site)
  dist/                    what /dl serves — the RELEASE assets:
    writeonce-0.1.0-linux-amd64.tar.gz
    writeonce-0.1.0-linux-amd64.tar.gz.sha256
  data/                    WO_DATA — the WAL; create it, keep it
```

```
SITE_TOKEN=<bearer for /admin>   required, the process refuses to start without it
WO_DATA=/srv/writeonce-site/data durable chapters; omit for RAM-only
WO_DIST=/srv/writeonce-site/dist where /dl reads from (default ./dist)
SITE_HOST                        leave UNSET behind a proxy — loopback is the
                                 right default; set it only to expose directly
```

Behind a proxy `SITE_HOST` stays unset, so the process binds
`127.0.0.1` and is unreachable except through nginx. It prints the
bound address at startup, which is the quickest way to confirm that.

**Keep `dist/` the published release, not a local build.** `just dist`
produces a different digest on every run, so a locally built tarball
would not match the `.sha256` the release publishes and would make the
mirror disagree with the GitHub download. Fetch the assets from the
release instead:

```
gh release download v0.1.0 -D dist -R shoneyJ/writeonce
```

## What it demonstrates

Chapters 1–9 teach the language (values, containers, classes, optionals,
tables, actors, deps, serving); the app itself exercises the framework's
routing/:params, the Logging middleware, bearer auth (mechanism from
`http/auth.wo`, policy here), `form_values`, `@table` + query + update by
assignment, and `writeonce-view`'s escaping/builders/Tailwind-style utility
sheet — self-contained pages, no CDN, no JS, no build step.
