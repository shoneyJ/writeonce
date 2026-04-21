# writeonce — the `.wo` Language and Runtime

> A declarative programming language with database and subscription-native HTTP in its standard runtime. Like `go run`, you write `.wo` files and execute them — but your program is a full-stack application.

---

## What writeonce is

`writeonce` is a programming language, a standard runtime, and a toolchain. Three layers of one product:

1. **The language** — `.wo` source files. Declarative by default (`type`, `service`, `policy`, `on <event>`) with a hybrid SQL+Cypher query sublanguage for the imperative parts. Types, queries, transactions, subscriptions, policies, triggers, HTTP endpoints, and UI screens are all first-class language constructs.
2. **The runtime** — an ACID multi-paradigm database (relational + document + graph), an HTTP server, a subscription engine, and a scheduler. All of it links into a single binary with your program. No external Postgres, no external Redis, no separate Node process.
3. **The toolchain** — the `wo` command: `wo run`, `wo build`, `wo test`, `wo fmt`, `wo mod`, `wo gen`. Modelled directly on the Go toolchain. One binary per project; no runtime to install on the target host.

The one-line pitch: **Go + Postgres + `net/http` + Phoenix LiveView, folded into one language and one binary.**

## Hello, world

> Full example projects:
> - [`docs/examples/blog/`](../examples/blog/) — a blog (~200 lines): articles, authors, tags, comments, live subscriptions, row-level policies, typed Go client.
> - [`docs/examples/ecommerce/`](../examples/ecommerce/) — an e-commerce store (~300 lines): cross-paradigm ACID checkout, link types with properties, tagged unions, a **live order-ops table** that delta-updates in place.

A complete `.wo` program that creates a database table, exposes six REST endpoints with live subscriptions, and emits a typed Go client:

```wo
-- article.wo
type Article {
  id:         Id
  title:      Text
  body:       Markdown
  author:     Text
  created_at: Timestamp = now()

  service rest "/api/articles"
    expose list, get, create, update, delete, subscribe
}
```

Run it:

```bash
$ wo run
[wo] compiling ./article.wo
[wo] schema: 1 type, 0 migrations needed
[wo] listening on :8080
     GET    /api/articles          list
     GET    /api/articles/:id      get
     POST   /api/articles          create
     PATCH  /api/articles/:id      update
     DELETE /api/articles/:id      delete
     WS     /api/articles/live     subscribe
```

Use it:

```bash
$ curl -X POST localhost:8080/api/articles \
    -H "Content-Type: application/json" \
    -d '{"title":"Hello","body":"# First post","author":"me"}'
{"id":1,"title":"Hello","body":"# First post","author":"me","created_at":"2026-04-17T..."}

$ curl localhost:8080/api/articles
[{"id":1,"title":"Hello","...":"..."}]
```

Generate a typed client:

```bash
$ wo gen sdk --lang go --out ./client
# produces ./client/sdk.go with typed Article struct and Subscribe helper
```

Subscribe from the client — deltas push on every commit, no polling:

```go
import "myapp.example.com/client"

c, _ := client.Connect("wo://localhost:8080")
sub, _ := c.Articles.Subscribe(ctx, client.Where{Author: "me"})
for delta := range sub.C {
    fmt.Printf("%s: %+v\n", delta.Kind, delta.Row)
}
```

Three files, five commands, zero infrastructure. Compare the same thing in Go+Postgres+React: one SQL schema, one migration tool, one ORM, one HTTP router, one subscription layer (polling or Redis pub-sub), one hand-written client, one React hook — roughly 2000 lines before you write any business logic.

## The toolchain

Go-literal. Every command maps to a Go equivalent so the mental model transfers:

| Command | Go equivalent | Purpose |
| --- | --- | --- |
| `wo init <name>` | `go mod init` | scaffold a new project |
| `wo run` | `go run ./...` | compile and execute |
| `wo build` | `go build` | emit a static binary |
| `wo test` | `go test` | run `.wo` tests |
| `wo fmt` | `gofmt` | canonical formatter |
| `wo vet` | `go vet` | lint + type-check without running |
| `wo mod <cmd>` | `go mod` | dependencies |
| `wo doc <sym>` | `go doc` | render docs for a type |
| `wo gen sdk --lang <L>` | `go generate` (codegen) | emit a client SDK |
| `wo migrate [--plan\|--apply]` | no direct equivalent | schema evolution |
| `wo dev` | no direct equivalent | hot-reload dev server |

**`wo run` vs `wo build`.** Same as Go: `wo run` compiles to a temp binary and executes it; `wo build` writes a named binary. No interpreter mode — `.wo` is compiled, always.

**`wo dev` is the one non-Go addition.** Edit a `.wo` file, the runtime hot-swaps the affected module without restarting. Live subscriptions survive the reload. This is the Phoenix LiveView influence.

## Program structure

```
myapp/
├── wo.toml           # like go.mod — name, version, dependencies
├── main.wo           # optional entry point
├── types/            # `type` declarations (one file per domain concept)
│   ├── article.wo
│   └── user.wo
├── ui/               # ##ui screens (optional)
├── tests/            # *_test.wo files
└── wo.lock           # locked dependency graph (like go.sum)
```

Minimum project is one `.wo` file with one `type` declaration. The compiler generates:

- the database schema (relational row, document structures, graph edges) from the type's fields
- HTTP handlers from type-attached `service` blocks
- transactional triggers from `on <event>` blocks
- row-level policies from `policy` blocks
- typed client SDKs from the same type, on demand

No `main()` is required for a pure type-and-service app. The runtime starts the HTTP server, loads the database, and dispatches. If you need procedural entry logic (CLI args, graceful shutdown hooks, cron jobs), add `main.wo` with a `main { ... }` block.

## The runtime — what's in the standard library

Every `wo build` links these in. They're not external packages you import — they're the language.

| Component | Responsibility | Mapped to phase |
| --- | --- | --- |
| **Database** | In-RAM ACID multi-paradigm (relational + doc + graph) with WAL durability | [Phase 2](./database/02-wo-language.md) + [Phase 3](./database/03-inmemory-engine.md) |
| **Transaction coordinator** | MVCC, snapshot isolation, cross-paradigm `RETURNING` alias table | [Phase 2](./database/02-wo-language.md) |
| **HTTP server** | REST + GraphQL dispatch generated from `service` blocks | [Phase 4](./database/04-client-api.md) + `crates/http` |
| **Subscription engine** | `LIVE` queries push deltas on commit, zero polling | [Phase 4](./database/04-client-api.md) |
| **Wire protocol** | Native binary codec for typed clients | [Phase 4](./database/04-client-api.md) |
| **Codegen** | `wo gen sdk` — Go, TypeScript, Rust, Python clients from `type` declarations | [Phase 5](./database/05-go-sdk.md) |
| **UI renderer** | `##ui` screens → SSR HTML + client runtime | [Phase 6](./database/06-lowcode-fullstack.md) |
| **Authorization** | `policy` blocks compiled into planner rewrite rules | [Phase 6](./database/06-lowcode-fullstack.md) |
| **Scheduler** | Single-threaded event loop over io_uring; one core per process (shard to scale) | [async.md](./async.md) + [Phase 2 concurrency](./database/02-wo-language.md#concurrency-model) |

Comparison to Go's stdlib:

| Need | Go | writeonce |
| --- | --- | --- |
| HTTP server | `net/http` | built-in `service rest` |
| Database | none (use `database/sql` + driver + Postgres) | **built-in** |
| Template rendering | `html/template` | `##ui` blocks |
| Concurrency | goroutines + channels | single-threaded event loop (Redis-style); shard to scale past one core |
| Testing | `testing` | `wo test` + `.wo` test syntax |
| Formatting | `gofmt` | `wo fmt` |
| Modules | `go.mod` + `go.sum` | `wo.toml` + `wo.lock` |

## Clients — who consumes your program

The same `.wo` type declarations that define the database also define the wire format. `wo gen sdk` emits:

- **Go** — typed structs, `*Client`, `TypedSubscription[T]` generics over a channel
- **TypeScript / browser** — types + `fetch` + WebSocket subscriptions
- **Rust** — structs, `tokio` async client, `impl Stream<Item = Delta>`
- **Python** — dataclasses, `async for delta in sub`
- **curl / raw REST** — documented via auto-generated OpenAPI spec at `/openapi.json`
- **GraphQL clients** — SDL auto-generated at `/graphql/schema.graphql`

**Raw `.wo` DML is a first-class escape hatch.** Every client SDK exposes a single method — `client.Wo(ctx, src, params)` in Go, equivalents in TypeScript/Rust/Python — that accepts any `.wo` source the server would accept: mixed SQL + Cypher, `BEGIN … COMMIT` blocks with `RETURNING` aliases threading across statements, ad-hoc MATCH-then-SELECT queries that cross multiple generated types. The typed methods are sugar; the engine speaks `.wo` on the wire. A Go program can send a cross-paradigm transaction as a single string and the server parses + executes it exactly like `wo run` would — see [Phase 5: Go Client SDK](./database/05-go-sdk.md) for the full API.

One schema, every protocol. A browser app, a mobile client, and a background worker can all subscribe to the same live query and receive the same delta stream.

## What this is, and isn't

**Is.** A declarative, full-stack, single-binary language for building CRUD apps with live data. A replacement for the "Go backend + Postgres + Redis + React + Prisma + GraphQL server" stack.

**Isn't.**

- Not a general-purpose language like Rust or Go. You can't write a kernel module or a video codec in `.wo`. The scope is data-shaped applications.
- Not a JavaScript meta-framework. No Node, no React. The UI layer (`##ui`) is declarative and compiles to SSR HTML with a small vanilla-JS client.
- Not a DSL that transpiles to another language. `.wo` has its own lexer, parser, analyzer, and bytecode. The [`prototypes/`db`/`](../../prototypes/`db`/) C++ prototype and the planned Rust crates implement the runtime natively.
- Not a hosted service. Your binary owns its own DB file. No managed cloud offering is required.

## How this maps to the design series

This overview is the user-facing frame. The underlying engineering plan is the 7-phase series linked from [database.md](./database.md):

- **[Phase 2](./database/02-wo-language.md)** designs the language and the transaction coordinator.
- **[Phase 3](./database/03-inmemory-engine.md)** builds the storage engine.
- **[Phase 4](./database/04-client-api.md)** builds the wire protocol and subscription engine.
- **[Phase 5](./database/05-go-sdk.md)** builds the first typed client (Go) and `wo gen`.
- **[Phase 6](./database/06-lowcode-fullstack.md)** adds `##ui` and the application-level blocks.
- **[Phase 7](./database/07-wo-seg-migration.md)** migrates writeonce-the-blog from `wo-seg` onto this runtime.

Phase 1 (evaluation) and the case studies in [surreal-case-study.md](./surreal-case-study.md) argue *why* the language exists at all. Read those first if you're skeptical; read the phase docs if you're implementing; read this page if you want to know what it feels like to use.

## Reference points

The design absorbs lessons from several systems. In order of influence:

- **Go** — toolchain shape, single-binary deployment, "the language is the build system"
- **Phoenix LiveView** — subscription-native UI, hot-reloading dev server
- **SAP CDS** — declarative entity/service language, admin UI generation
- **SurrealDB** — multi-paradigm query language, `LIVE` subscriptions over wire
- **PocketBase** — single-binary CRUD backend (the proof of concept that this is shippable)
- **EdgeDB** — unified type system above storage paradigms
- **Elixir / Erlang / OTP** — hot code loading, supervision, subscription semantics
- **Django** — admin UI as a built-in, not a bolt-on

None of these give you all of: a language, a database, a subscription engine, a UI toolkit, a client codegen, and a single-binary output. writeonce is the attempt to fuse the best of each into one thing.

## Minimal "hello, world" as a full program

If you want a pure procedural test, without the server:

```wo
-- hello.wo
main {
  print("hello, world")
}
```

```bash
$ wo run hello.wo
hello, world
```

If you want the database without HTTP:

```wo
type Counter {
  name:  Text @unique
  value: Int = 0
}

main {
  insert Counter { name: "visits" };
  update Counter{ name == "visits" }.value += 1;
  let c = select Counter{ name == "visits" };
  print(c.value);
}
```

If you want the full app — database, HTTP, subscriptions, clients — it's the article example at the top of this page.

Three progressive shapes, one language, one command to run each.
