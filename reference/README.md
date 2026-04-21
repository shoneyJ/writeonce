# `reference/` — archived for reference

Source material preserved alongside the active codebase. Nothing here is on the build path of the root workspace.

## `reference/crates/` — v1 writeonce blog

The original writeonce blog engine: 13 Rust crates implementing a flat-file content store, three sidecar indexes, a minimal HTTP server, and an `.htmlx` template layer.

| Crate | Responsibility |
| --- | --- |
| `wo-model` | `Article` domain type + content loader |
| `wo-seg` | Append-only `.seg` binary file format (~475 LOC) |
| `wo-index` | `title.idx`, `date.idx`, `tags.idx` sidecar indexes |
| `wo-store` | Composes seg + indexes, exposes the query API |
| `wo-watch` | inotify-driven content ingest |
| `wo-event` | Domain event type |
| `wo-sub` | Subscription layer (pre-engine-native `LIVE`) |
| `wo-rt` | v1 runtime glue |
| `wo-http`, `wo-route`, `wo-serve` | HTTP server |
| `wo-htmlx` | `.htmlx` template engine |
| `wo-md` | Markdown rendering |

It is a nested workspace. Build or test it standalone:

```bash
cd reference/crates
cargo build
cargo test
```

See [docs/runtime/database/07-wo-seg-migration.md](../docs/runtime/database/07-wo-seg-migration.md) for the plan to replace these crates with the new `.wo` runtime at `crates/wo-rt/` via phased coexistence. The v1 codebase is the source that migration reads from.

## `reference/writeonce-api/` and `reference/writeonce-app/`

Earlier exploration snapshots that predate the v1 crates. Self-contained; no active build wiring.

## `reference/linux/` and `reference/go/` (symlinks)

Research source trees, gitignored. User-specific absolute paths — each contributor sets their own:

```bash
ln -s <path-to-linux-src> reference/linux
ln -s <path-to-go-src>    reference/go
```

- **`reference/linux/`** — the Linux kernel source. Read `io_uring/`, `fs/notify/inotify/`, `kernel/eventfd.c`, `include/uapi/linux/*.h` when designing kernel-primitive modules. See per-primitive reference cards under [`docs/plan/linux/`](../docs/plan/linux/).
- **`reference/go/`** — the Go source tree. Read `src/runtime/netpoll_epoll.go`, `src/runtime/netpoll.go`, `src/runtime/asm_*.s`, `src/runtime/sys_linux_*.s` when designing the runtime layer — writeonce's `crates/rt/src/runtime/` mirrors Go's `src/runtime/` file-per-flavour naming. See [`docs/plan/assembly/`](../docs/plan/assembly/) for the asm policy doc that cites this tree.
