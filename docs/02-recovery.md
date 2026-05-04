# Recovery — The Target Architecture

This document describes where writeonce is going: a single, self-contained binary that owns its own storage, serves its own content, and pushes updates to connected clients in real-time — with no external database, no cloud pipeline, and no separate API server.

## Guiding Principle

**Everything in one process.** The database, the server logic, and the client-facing interface all live in a single codebase and ship as a single executable. If you can run the binary, you have the full platform.

## Own Database

The current PostgreSQL instance is a derived cache — it stores JSONB copies of files that already exist as the source of truth. The recovery architecture eliminates this indirection entirely.

### What Changes

- **No external database.** No PostgreSQL, no Diesel ORM, no connection pooling, no migrations.
- **Local file storage.** Markdown files and JSON metadata files are stored in a local directory, just as they are today in `writeonce-articles-s3/`. The file system *is* the database.
- **Custom storage segments (.seg files).** Research area: segment files that provide efficient read access, indexing, and potentially append-only writes for content. Think of these as a lightweight, purpose-built storage layer — not a general-purpose database engine, but enough to support indexed lookups by `blog-title` and ordered listing by date.
- **Indexed by blog-title.** The `sys_title` / blog-title field remains the primary key for content retrieval. The embedded storage must support O(1) or O(log n) lookups by this field.

### What Stays the Same

- Articles are still structured as JSON metadata + Markdown content pairs.
- The `sys_title`, `published`, `tags`, `author`, and section structure remain the content model.
- Content is still the source of truth — but now it's read directly from local storage instead of being derived through a sync pipeline.

## No AWS Infrastructure

The current architecture uses S3 as a file host and Lambda as a sync trigger. In the target architecture, there is nothing to sync *to* — the files are already where they need to be.

### What Gets Removed

| Current Component | Why It Existed | Why It's No Longer Needed |
|---|---|---|
| S3 bucket | Remote file storage | Files live locally alongside the binary |
| Lambda function (Go) | Watch S3 for changes, call API | No remote store to watch — file changes are local |
| aws-infra service (Rust) | Bridge to AWS S3/EC2 APIs | No AWS dependency |
| Pulumi IaC | Manage Lambda + S3 resources | No cloud resources to manage |

### What Replaces It

The binary watches its own content directory. When a file changes (new article, updated metadata), the embedded database re-indexes and notifies subscribers. The deployment model becomes:

```
1. Place the binary on a server
2. Point it at a content directory
3. It serves
```

No credentials, no IAM roles, no SDK configuration.

## No Separate API

Today, `writeonce-api` is a standalone Actix-web server that mediates between the frontend and the database. In the target architecture, the server logic is embedded in the same process as the database and the content renderer.

### What This Means

- **No HTTP hop between database and server.** Queries go directly from the request handler to the storage engine in-process. No network serialization, no connection pool, no ORM layer.
- **Single codebase.** No multi-repo coordination. A new article field is added once — in the content model — and it flows through storage, indexing, and rendering in the same compilation unit.
- **Single deployment.** One binary, one container, one process. No docker-compose orchestrating API + database + infra services.

The binary still exposes HTTP endpoints — it's still a web server. But it's a web server with an embedded database, not a web server that talks to an external one.

## Real-Time Subscriptions Without WebSocket

The current architecture has no mechanism for pushing content updates to connected clients. The target architecture adds real-time subscriptions, but explicitly without WebSocket.

### Why Not WebSocket

WebSocket adds connection state management, heartbeat logic, reconnection handling, and protocol upgrade complexity. For a content platform where updates are infrequent (articles are published, not streamed), the overhead isn't justified.

### Subscription Model

The target is a subscription mechanism where:

- A client subscribes to a content query (e.g., "all published articles" or "article with sys_title X")
- When the underlying data changes, the server pushes the relevant diff to the subscriber
- No polling from the client side

Candidate approaches to research:

- **Server-Sent Events (SSE)** — unidirectional push over HTTP. Simple, well-supported, no protocol upgrade. Natural fit for infrequent content updates.
- **SpacetimeDB-style subscriptions** — clients register queries, the engine tracks which rows match, and only sends diffs when the result set changes. This is the aspirational model.
- **Long polling** — fallback option. Simple but less efficient than SSE for multiple subscribers.

The key constraint: the subscription mechanism must work without requiring clients to maintain persistent bidirectional connections.

## Target Architecture

```
              content directory
            (JSON + MD files, .seg index)
                      |
                      |  file watch + re-index
                      v
         +---------------------------+
         |      writeonce binary      |
         |                           |
         |   +-------------------+   |
         |   | embedded storage  |   |   .seg files, blog-title index
         |   | (read/write/index)|   |
         |   +-------------------+   |
         |            |              |
         |   +-------------------+   |
         |   |   server logic    |   |   route handlers, content queries
         |   | (HTTP endpoints)  |   |
         |   +-------------------+   |
         |            |              |
         |   +-------------------+   |
         |   | subscription mgr  |   |   SSE / query-based push
         |   | (real-time push)  |   |
         |   +-------------------+   |
         |                           |
         +---------------------------+
                   |
              HTTP / SSE
                   |
                   v
         +-------------------+
         |    frontend app   |    Angular or successor
         |  (browser client) |
         +-------------------+
```

## Single Repository

The five current repos collapse into one:

```
writeonce/
  content/              # articles (JSON + MD), images, assets
  storage/              # embedded database engine (.seg files, indexing)
  server/               # HTTP handlers, subscription manager
  frontend/             # client application
  writeonce.toml        # configuration (port, content dir, index settings)
```

One repo. One build. One deploy artifact.

## What Needs Research

| Area | Question | Notes |
|------|----------|-------|
| **.seg file format** | What storage format gives efficient indexed reads over JSON+MD content? | Look at LSM trees, append-only logs, SQLite's page format for inspiration |
| **File watching** | How to efficiently detect content changes on Linux/macOS? | `inotify` on Linux, `kqueue` on macOS, or cross-platform via `notify` crate |
| **SSE vs alternatives** | Is SSE sufficient for the subscription model, or is something custom needed? | SSE handles the "push diffs to subscribers" case well for low-frequency updates |
| **Index structure** | What index structure supports `blog-title` lookup + date-ordered listing? | B-tree or hash index for title, sorted set for date ordering |
| **Language choice** | Continue with Rust for the unified binary? | Rust fits: single binary output, no runtime, strong typing, existing team knowledge |
| **Frontend coupling** | Should the frontend be embedded in the binary (serve static assets) or remain separate? | Embedding simplifies deployment; separate allows independent frontend iteration |

## Migration Path

The transition from current to target doesn't have to be all-or-nothing:

1. **Phase 1** — Build the embedded storage engine. Read JSON+MD files from a local directory, index by `blog-title`, serve via HTTP. No AWS, no PostgreSQL. This alone replaces `writeonce-api` + `aws-infra` + `lambda-function` + PostgreSQL.
2. **Phase 2** — Add real-time subscriptions (SSE). Clients subscribe to content queries and receive push updates when files change.
3. **Phase 3** — Collapse repositories. Move frontend into the unified codebase. Ship as a single binary that serves both API and static assets.

Each phase produces a working system. The current architecture can run in parallel until the new one is ready.

## Implementation phases

The "embedded storage engine" of Phase 1 above lands in three numbered plan docs under [`docs/plan/`](./plan/):

| Phase | Doc | What it ships |
| --- | --- | --- |
| 10 | [`plan/10-storage-foundations.md`](./plan/10-storage-foundations.md) | On-disk row codec (length-prefix + flags + LSN + CRC32C); per-type segment files (`data/<TypeName>.seg`); `posix_fallocate` preallocation; `pwrite`-only append path. Reads still in-memory. |
| 11 | [`plan/11-wal-and-recovery.md`](./plan/11-wal-and-recovery.md) | WAL log with `fdatasync` at commit; group commit per loop tick; control file with `last_durable_lsn` (rename-on-write); replay loop on startup. `kill -9` mid-write loses nothing acknowledged. |
| 12 | [`plan/12-engine-disk-cutover.md`](./plan/12-engine-disk-cutover.md) | `Engine`'s row payload moves to disk; in-memory map becomes `BTreeMap<i64, SegmentOffset>`. Periodic checkpoint flushes segments + advances the control file. RAM bounded by id-count, not row size. |

Postgres' storage subsystem is the design reference — see [`docs/plan/exploration/postgresql/`](./plan/exploration/postgresql/) for which Postgres modules informed which decision and what writeonce skips (multi-process IPC, latches, separate writer processes).

The durability syscalls themselves live in [`docs/plan/exploration/linux/12-pwrite-fsync.md`](./plan/exploration/linux/12-pwrite-fsync.md).
