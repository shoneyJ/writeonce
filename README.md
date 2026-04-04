# writeonce

A single self-contained binary that serves a content platform — no external database, no cloud pipeline, no JavaScript framework. Built in Rust on raw Linux kernel primitives.

## Why

The original writeonce system spread across five repositories, four languages, AWS infrastructure (S3, Lambda, API Gateway), PostgreSQL, and an Angular frontend. All of that to serve articles from local files. This project collapses everything into one process that owns storage, serves content, and pushes real-time updates.

## Architecture

- **Single process** — one binary replaces S3 + Lambda + Rust API + PostgreSQL + Angular
- **Embedded storage** — custom `.seg` segment files with positional indexing, no external database
- **Real-time subscriptions** — route-based SSE streams push content diffs to connected clients
- **Server-rendered HTML** — `.htmlx` templates with data bindings, minimal client-side JS
- **Markdown-first content** — `.md` files are the source of truth, JSON holds only metadata
- **Linux kernel I/O** — `epoll`, `inotify`, `eventfd`, `timerfd`, `sendfile` — no tokio, no async runtime

## Workspace Crates

| Crate | Purpose |
|-------|---------|
| `wo-model` | Article and metadata types |
| `wo-seg` | Segment file reader/writer (.seg format) |
| `wo-index` | Title hash map, date sorted array, tags inverted index |
| `wo-store` | Query API over segments and indexes |
| `wo-watch` | `inotify`-based content directory watcher |
| `wo-event` | `epoll` event loop, `eventfd`, `timerfd`, `signalfd` |
| `wo-sub` | Subscription manager and diff delivery |
| `wo-rt` | Single-threaded runtime tying I/O sources together |
| `wo-http` | HTTP request parsing and response writing |
| `wo-route` | URL routing and handler dispatch |
| `wo-htmlx` | Template engine for `.htmlx` files |
| `wo-md` | Markdown to HTML rendering |
| `wo-serve` | Binary entry point — wires everything together |

## Build

```sh
cargo build --release
```

## Deploy

The binary runs behind nginx with Let's Encrypt SSL. See `infra/setup.sh` for first-time server setup and `docs/07-ssl.md` for the full deployment walkthrough.

```sh
# Build, copy binary, sync content, restart service
./infra/deploy.sh
```

## Documentation

Design documents live in `docs/`:

- `00-linux.md` — Linux kernel primitives used
- `01-problem.md` — Problem statement and motivation
- `02-recovery.md` — Target architecture
- `03-data.md` — Embedded storage and subscription model
- `04-ui.md` — Server-rendered HTMLX templates
- `05-datalayer.md` — Data layer implementation status
- `06-markdown-render.md` — Markdown-first content model
- `07-ssl.md` — SSL, nginx, and deployment
- `runtime/` — Deep dives on async runtimes, fibers, and Rust's ownership model
- `future-scope/` — Planned features including AI agent content management
