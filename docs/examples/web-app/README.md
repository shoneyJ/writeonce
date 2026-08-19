# web-app — the storefront sample

A small store: `Product`/`Order` as `@table` classes, JSON routes, one auth
middleware — built on [`writeonce-framework`](../writeonce-framework/), which
it imports **through `[deps]`** (iteration 15). This app is iteration 16's
acceptance workload: `just web-app` runs the whole chain — fetch → lock →
build → serve → curl matrix → restart persistence → SIGTERM.

## Routes

| Route | What |
| --- | --- |
| `GET /products` | list (JSON array) |
| `GET /products/:id` | one product or 404 |
| `POST /products` | create from a JSON body (`name`, `price`, `stock`); 400 on malformed JSON; 409 on a duplicate name (`@unique`) |
| `POST /orders` | create (`product`, `qty`); FK checked |
| `DELETE /products/:id` | 409 while orders reference it (FK restrict), 200 after |

Every request needs `authorization: Bearer <token>` (the auth middleware);
the token comes from the `WA_TOKEN` env var.

## Run

    woc .                       # fetches deps, builds target/web-app
    WA_TOKEN=secret WO_DATA=./data ./target/web-app 8080

## TLS / HTTP2

None here, deliberately: deploy behind nginx/caddy — the proxy terminates
TLS+ALPN and speaks h2 to browsers while this backend serves HTTP/1.1
keep-alive. Sketch:

    server {
      listen 443 ssl;
      http2 on;
      location / {
        proxy_pass http://127.0.0.1:8080;
        proxy_http_version 1.1;
        proxy_set_header Connection "";
      }
    }
