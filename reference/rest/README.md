# `reference/rest/` — HTTP test files for the `.wo` runtime

`.rest` (or `.http`) is the plain-text HTTP-request format supported by the two main editor HTTP clients:

- **VS Code** — install [REST Client](https://marketplace.visualstudio.com/items?itemName=humao.rest-client) (`humao.rest-client`) and click "Send Request" above any block.
- **JetBrains IDEs** (IntelliJ, WebStorm, RustRover, Goland) — built-in HTTP Client recognises `.rest` and `.http` natively.

Both clients understand:

- `### ...` block separators
- `@var = value` document-level variables referenced as `{{var}}`
- `# @name foo` on a request, whose response fields are later reachable as `{{foo.response.body.id}}` — useful for threading auto-generated ids from `create` responses into later `get`/`patch`/`delete` calls

## Files

| File | Against | What it exercises |
| --- | --- | --- |
| [`blog.rest`](./blog.rest) | `docs/examples/blog/` | Full CRUD on Article/Comment, read-only on Author/Tag (matches the sample's `expose` lists). End-to-end flow: create article → list → get by id → PATCH title → PATCH embedded doc → DELETE draft → verify final state. |
| [`ecommerce.rest`](./ecommerce.rest) | `docs/examples/ecommerce/` | What Stage 2 currently serves for the ecommerce sample: read-only Product/Order/Customer lists, 405s for non-exposed create endpoints, 501s for Stage 3 stubs. Documents the shape of Stage 3/4 endpoints (`fn checkout`, LIVE subscribe, `/me`) even though they're not wired yet. |

## Running

```bash
# one terminal — start the runtime
cargo run --bin wo -- run docs/examples/blog
#    [wo] listening on http://127.0.0.1:8080

# another terminal — or just open the .rest file in VS Code/JetBrains and click
```

Override the port via `WO_LISTEN`:

```bash
WO_LISTEN=127.0.0.1:9000 cargo run --bin wo -- run docs/examples/blog
```

…and update the `@host` line at the top of the `.rest` file to match.

## Without an editor (just `curl`)

Each `.rest` block maps directly to `curl`. Some examples:

```bash
# Runtime info
curl http://127.0.0.1:8080/

# List
curl http://127.0.0.1:8080/api/articles

# Create — server assigns `id` automatically
curl -X POST http://127.0.0.1:8080/api/articles \
  -H "Content-Type: application/json" \
  -d '{
    "slug": "hello-writeonce",
    "title": "Hello, writeonce",
    "author": 1,
    "published": true,
    "meta": { "excerpt": "first post", "body_md": "# hi" }
  }'

# Get by id
curl http://127.0.0.1:8080/api/articles/1

# Partial update
curl -X PATCH http://127.0.0.1:8080/api/articles/1 \
  -H "Content-Type: application/json" \
  -d '{ "title": "Updated" }'

# Delete
curl -X DELETE http://127.0.0.1:8080/api/articles/1    # 204 on success

# Stage 3 stub
curl -i http://127.0.0.1:8080/api/articles/live        # 501 Not Implemented
```

For a scripted smoke run against the blog sample, the top-to-bottom `curl` sequence that exactly mirrors `blog.rest` is in [`docs/examples/blog/README.md`](../../docs/examples/blog/README.md).

## Expected-status cheat sheet

Every block in the `.rest` files ends its description with the expected HTTP status. Quick legend:

| Status | Meaning in this prototype |
| --- | --- |
| `200` | OK — list / get / update succeeded |
| `201` | Created — new row, `id` in the response body |
| `204` | No Content — delete succeeded |
| `400` | Bad JSON body |
| `404` | No such row, OR no method at all is attached to the path (e.g. `/api/customers` when the `service rest` block doesn't `expose` any collection-root op) |
| `405` | Method not allowed — the path is registered for a *different* method (e.g. POST against `/api/products` when only `list` is exposed, so GET is attached but POST isn't) |
| `501` | Not Implemented — Stage 3+ feature (LIVE subscriptions, `/me`, transactional fns) |

A `405` is a *feature* of the sample — it confirms the `expose` list in the `.wo` file is being honoured. A `501` is a Stage marker — the runtime acknowledges the shape but hasn't wired the handler yet.
