# writeonce-pl — the `.wo` language at a glance

writeonce is a **declarative full-stack programming language**. You write `.wo` files; the `wo` toolchain compiles them into a single binary that owns its database, serves REST, and pushes live subscriptions. The one-line pitch: **Go + Postgres + `net/http` + Phoenix LiveView, folded into one language and one binary.**

It is declarative by default — programs are built from `type`, `class`, `service`, `policy`, and `on <event>` declarations. A `type` declares a data shape; a `class` is its behavior-bearing sibling — the same fields plus `fn` methods with a `self` receiver. **There is no inheritance**: no `extends`, no overriding, no virtual dispatch — composition via `ref`/`multi`, Go-style. From either declaration the compiler derives the database schema, HTTP endpoints, triggers, and client SDKs. The imperative parts (queries, transactions, method bodies) use a hybrid SQL + Cypher sublanguage.

## The three pillars

| Pillar                    | Language construct                                                                                                                               | Status                                                                                                                |
| ------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------ | --------------------------------------------------------------------------------------------------------------------- |
| **Live subscriptions**    | `LIVE` prefix on queries; `subscribe` in a service's `expose` list. Deltas push on every commit over WebSocket — no polling.                     | Stage 3 — `/api/<type>/live` is a 501 stub today                                                                      |
| **Front-end development** | `##ui` blocks compile to server-rendered HTML plus a small vanilla-JS client runtime. No Node, no React.                                         | Phase 6 — design-only ([spec](runtime/database/06-lowcode-fullstack.md))                                              |
| **Database DML**          | Hybrid SQL + Cypher + document paths: `$name` parameters everywhere, cross-paradigm `RETURNING col AS alias`, one `BEGIN … COMMIT` block syntax. | Query layer prototyped in C++ ([`prototypes/wo-db/`](../prototypes/wo-db/README.md)); schema layer shipped in Stage 2 |

## A complete program

One file is a full application — a database table, six REST endpoints, and a live subscription:

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

```bash
$ wo run
[wo] listening on :8080
```

All three pillars in ~10 lines: the `type` fields are the DML schema, `expose subscribe` is the live subscription, and (from Phase 6 on) a `##ui` block alongside it renders the screen.

## Implementation status

| Stage | What                                                         | Status                                                      |
| ----- | ------------------------------------------------------------ | ----------------------------------------------------------- |
| 1     | `wo run <dir>` discovers `.wo` files                         | ✅ shipped                                                  |
| 2     | parser + in-memory engine + REST CRUD                        | ✅ shipped — `cargo run --bin wo -- run docs/examples/blog` |
| 3     | LIVE subscriptions over WebSocket                            | pending (501 stub)                                          |
| 4+    | transactional `fn`, policies, triggers, `##ui`, WAL, codegen | design-only                                                 |

**Class model status:** `class` declarations parse and serve REST CRUD today (plan 13a, shipped); method execution (13b), live pricing push (13c), and the MVC UI ([plan 14](plan/14-mvc-ui-implementation.md)) follow. Demo: [`examples/pricing/`](examples/pricing/); master plan: [plan 13](plan/13-class-model-live-pricing.md).

The runtime itself targets **zero external dependencies** — all I/O driven directly by Linux kernel primitives (`epoll`, `inotify`, `sendfile`, …); see the [kernel-primitive catalogue](plan/exploration/linux/00-linux.md).

## Where to read next

- [`runtime/wo-language.md`](runtime/wo-language.md) — the full user-facing language overview: toolchain, runtime stdlib, client SDKs
- [`runtime/database.md`](runtime/database.md) — the 7-phase engineering series behind the language
- [`runtime/database/02-wo-language.md`](runtime/database/02-wo-language.md) — the two-layer language spec (schema layer + query layer)
- [`examples/blog/README.md`](examples/blog/README.md) — the canonical worked example
- [`../prototypes/wo-db/README.md`](../prototypes/wo-db/README.md) — the C++ prototype of the query-layer engine

## Understanding the basics

Every layer of writeonce ultimately reduces to one primitive operation: **ask the kernel for memory, store a value in it, read it back**. Walking that operation up the abstraction ladder shows what the `.wo` syntax is actually hiding.

### Level 0 — assembly: the kernel gives you a page

```asm
; x86-64 Linux — map one anonymous page, store 42 in it
mov rax, 9            ; syscall number: mmap
xor rdi, rdi          ; addr  = NULL (kernel picks)
mov rsi, 4096         ; len   = one page
mov rdx, 3            ; prot  = PROT_READ | PROT_WRITE
mov r10, 0x22         ; flags = MAP_PRIVATE | MAP_ANONYMOUS
mov r8, -1            ; fd    = none
xor r9, r9            ; off   = 0
syscall               ; rax now holds the page address

mov qword [rax], 42   ; store the value
mov rbx, [rax]        ; read it back
```

There is no "variable" — only an address the kernel handed back and a `mov` into it.

### Level 1 — C: the libc wrapper names the address

```c
long *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
*p = 42;              /* store  */
long v = *p;          /* read   */
```

Same syscall, same page — C just gives the address a typed name and lets the compiler emit the `mov`s. (`malloc` is one more layer: a userland allocator carving up pages obtained exactly this way.)

### Level 2 — `.wo`: the value gets a type, a lifetime, and durability

```wo
type Counter {
  name:  Text @unique
  value: Int = 0
}

main {
  insert Counter { name: "visits" };       -- allocate + store
  update Counter{ name == "visits" }.value += 1;
  let c = select Counter{ name == "visits" };
  print(c.value);                          -- read back
}
```

The `insert` is still, underneath, "obtain memory, write bytes at an offset" — but the declaration has been folded into the language: the `type` decides the layout, the engine owns the allocation (in-RAM rows over `mmap`-backed segments), and Phase 3+ adds what raw memory never had — ACID transactions, WAL durability, and `LIVE` subscribers notified on every store.

This is the whole design in miniature: the runtime is written in Rust against raw kernel primitives (level 0–1, see [`plan/exploration/linux/00-linux.md`](plan/exploration/linux/00-linux.md) and the [assembly stance](plan/exploration/assembly/00-overview.md)), so that the `.wo` author never has to leave level 2.
