# Data Layer — Local Storage with Subscriptions

This document describes the embedded data layer that replaces PostgreSQL: local `.seg` files with indexing, and a subscription model where clients register queries and receive diffs on route visit — no polling required.

## .seg File Storage

The `.seg` (segment) format is the on-disk representation of article data. Each segment file holds serialized article content with positional indexing for fast lookups.

### Design Goals

- **No external database process.** The binary reads and writes `.seg` files directly. No socket connections, no protocol negotiation, no separate daemon.
- **Indexed by blog-title.** The primary access pattern is `GET /blog/:sys_title`. The storage layer must resolve a `sys_title` to its article content without scanning all files.
- **Append-friendly.** New articles and updates append to the segment. Deletes are tombstoned and compacted later.
- **Human-readable source.** The JSON + Markdown files remain the authoring format. `.seg` files are a derived index — if they're deleted, they can be rebuilt from the content directory.

### Proposed Structure

```
content/
  linux-misc/
    linux-misc.json          # authored metadata (source of truth)
    linux-misc.md            # authored content (source of truth)
  aws-lambda-pulumi/
    aws-lambda-pulumi.json
    aws-lambda-pulumi.md

data/
  articles.seg               # serialized article records
  index/
    title.idx                 # blog-title -> offset mapping
    date.idx                  # publish date -> offset (sorted)
    tags.idx                  # tag -> [offsets] (inverted index)
```

The `content/` directory is what the author edits. The `data/` directory is what the engine builds and queries. Losing `data/` is a cold start, not data loss.

### Segment File Internals

```
+------------------+
| segment header   |   magic bytes, version, record count
+------------------+
| record 0         |   length-prefixed serialized article
+------------------+
| record 1         |
+------------------+
| ...              |
+------------------+
| record N         |
+------------------+
```

Each record is a length-prefixed byte sequence containing the full article (metadata + content merged). Records are addressed by byte offset from the start of the file.

### Index Files

**title.idx** — Hash map serialized to disk. Maps `sys_title` (string) to byte offset in `articles.seg`. Loaded into memory at startup for O(1) lookups.

**date.idx** — Sorted array of `(timestamp, offset)` pairs. Supports range queries for "articles published between X and Y" and ordered listing for the homepage.

**tags.idx** — Inverted index. Maps each tag string to a list of offsets. Supports "all articles tagged with X" queries.

On startup, index files are memory-mapped or loaded into heap. On content change, affected indexes are rebuilt incrementally.

## Subscription Model

The subscription model is inspired by SpacetimeDB: clients register queries, and the engine tracks which results match. When underlying data changes, only the relevant diffs are pushed to subscribers.

### How It Works

```
   Client A                    Server                     Content Dir
      |                          |                             |
      |--- GET /blog/linux-misc -|                             |
      |                          |-- read from .seg index ---->|
      |<-- article + SSE stream -|                             |
      |                          |                             |
      |   (subscribed to         |                             |
      |    sys_title=linux-misc) |                             |
      |                          |                             |
      |                          |<-- file change detected ----|
      |                          |                             |
      |                          |-- re-index article -------->|
      |                          |-- diff against last push -->|
      |                          |                             |
      |<-- SSE: updated content -|                             |
      |                          |                             |
```

### Route-Based Subscription

When a user visits a route, the response includes both the current content and an SSE stream. The client is automatically subscribed to changes for that query — no explicit subscription handshake needed.

```
GET /blog/linux-misc
```

Response:
```
HTTP/1.1 200 OK
Content-Type: text/html

<!-- full article content rendered -->

<!-- SSE connection opened for this query -->
<script>
  const source = new EventSource('/subscribe/blog/linux-misc');
  source.onmessage = (event) => {
    // apply diff to current content
  };
</script>
```

The subscription lives as long as the browser tab is open. When the user navigates away, the EventSource closes and the server drops the subscription. No heartbeat management, no reconnection logic beyond what SSE provides natively (automatic reconnect is built into the EventSource API).

### Query Registration

Subscriptions are not limited to single-article lookups. The engine supports registering arbitrary content queries:

| Query Type | Example | Subscription Behavior |
|---|---|---|
| Single article | `sys_title = "linux-misc"` | Push when this specific article changes |
| All published | `published = true` | Push when any article is published or unpublished |
| By tag | `tags contains "rust"` | Push when a rust-tagged article is added, removed, or updated |
| Homepage list | `published = true ORDER BY date DESC LIMIT 10` | Push when the top-10 list changes |

The server maintains a registry of active subscriptions. On each content change, it evaluates which subscriptions are affected and pushes diffs only to those clients.

### Diff Format

When content changes, the server doesn't resend the full article. It sends a minimal diff:

```json
{
  "type": "update",
  "sys_title": "linux-misc",
  "changes": {
    "content.sections[2].paragraphs[0]": "Updated paragraph text...",
    "content.tags": ["linux", "kernel", "new-tag"]
  },
  "version": 42
}
```

The `version` field enables clients to detect missed updates and request a full resync if needed.

## Sample Dataset

To validate the storage engine and subscription model, a sample dataset should exercise the core access patterns:

### Articles

| sys_title | tags | published | purpose |
|---|---|---|---|
| `sample-getting-started` | `[tutorial, beginner]` | true | Basic article, tests single-article subscription |
| `sample-rust-patterns` | `[rust, patterns]` | true | Tests tag-based queries |
| `sample-draft-wip` | `[draft]` | false | Tests published filter — should not appear in public queries |
| `sample-long-form` | `[deep-dive, rust]` | true | Multiple sections, images, code snippets — tests complex content rendering |
| `sample-frequently-updated` | `[changelog]` | true | Updated often — tests subscription diff delivery |

### Test Scenarios

1. **Cold start** — Delete `data/`, start the binary. It should rebuild `.seg` and index files from `content/` and serve all articles.
2. **Single article query** — `GET /blog/sample-getting-started` returns the article and opens an SSE subscription.
3. **Live update** — Edit `sample-frequently-updated.json` while a client is subscribed. The client should receive an SSE event with the diff.
4. **Tag query** — Subscribe to `tags contains "rust"`. Both `sample-rust-patterns` and `sample-long-form` should be in the result set. Adding a new article tagged `rust` should trigger a push.
5. **Publish toggle** — Change `sample-draft-wip` from `published: false` to `true`. Clients subscribed to the homepage list should receive a push with the new article added.

## SpacetimeDB Reference

SpacetimeDB is the primary architectural inspiration for the subscription model. Key concepts to study:

- **Modules** — server logic that runs inside the database, not beside it
- **Subscription queries** — clients register SQL-like queries; the engine evaluates them incrementally on each transaction
- **Incremental view maintenance** — only recompute the parts of a query result that changed
- **Client SDK generation** — type-safe client code generated from the server schema

Add SpacetimeDB as a reference submodule for quick access to their implementation patterns:

```bash
git submodule add https://github.com/clockworklabs/SpacetimeDB.git references/spacetimedb
```

The goal is not to replicate SpacetimeDB — it's to take its subscription semantics and apply them to a much narrower domain (blog content), where the simplicity of the problem allows a simpler implementation.
