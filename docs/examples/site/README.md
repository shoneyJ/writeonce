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
