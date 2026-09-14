# writeonce

**A small compiled language with a database built in.** You write `.wo`
files; one command turns them into a single native binary that carries its
own storage engine — a typed, WAL-durable, crash-recoverable database — with
no server to install, no ORM, and no query strings. Tables are just classes,
queries are written in the language and checked by the compiler, and the whole
program ships as one file that depends only on the system C library.

> **Status: early, honest.** Everything documented on this page compiles and
> runs today and is exercised by the acceptance tests in this repository.
> Features that are planned but **not yet available** are listed separately
> under [Roadmap](#roadmap) — they are not described as if they work. Nothing
> here is API-stable yet.

---

## Why writeonce

- **The database is part of the language.** A `class` marked `@table` *is* a
  table. Its rows persist through a write-ahead log, survive a restart, and are
  reached by navigating typed relations — not by assembling SQL text.
- **Queries are compiled, not interpreted.** `from e in Employee where
  e.salary > 90000 select e` lowers to bytecode loops over the engine. A
  mistyped field name is a **compile error**, not a runtime surprise. There is
  no SQL string anywhere in the shipped binary.
- **One binary, no runtime dependencies.** `woc .` produces a self-contained
  executable (160–260 KB for the sample programs in this repository) that links
  only libc. Copy it to a server and run it.
- **Small on purpose.** No FFI, no reflection, no package registry —
  dependencies are exact-rev git URLs and nothing else. The standard library is
  a handful of OS modules. The language is designed to be read.

writeonce is a systems language whose distinguishing feature is the embedded
database. HTTP/1.1 and WebSockets **do** work today — but as `.wo` libraries you
consume through `[deps]` (`porch` for serving, `writeonce-view` for
HTML), never as runtime features: the runtime stays framework-agnostic on
purpose. TLS is always terminated by a proxy in front. If you have seen an older
"writeonce" that served REST from `cargo run`, that was a separate, earlier
runtime; this page documents the current `woc`/`wovm` toolchain.

---

## System requirements

**To run a compiled writeonce program:**

- Linux on x86-64. The produced binary is a native executable that links only
  the system C library (`libc`); nothing else is required at runtime.

**To build programs from source (the toolchain), you need:**

| Tool | Version tested | Purpose |
| --- | --- | --- |
| OCaml | 4.14+ | builds `woc`, the compiler front end |
| dune | 3.14+ | OCaml build driver |
| A C11 compiler | gcc 13 / clang | builds `wovm`, the runtime VM |
| just | 1.x | task runner for the build/test recipes |
| make | any | drives the runtime build |

Other POSIX platforms (macOS, BSD) are untested. The toolchain itself has no
network or package-download step — it builds entirely from the checked-in
source.

---

## Getting the toolchain

Two artifacts make up the toolchain:

- **`woc`** — the compiler (OCaml). Reads `.wo` source, type-checks it, runs
  the ownership pass, and emits a `.wob` image or a standalone binary.
- **`wovm`** — the runtime (C11). Loads a `.wob` image and executes it. When
  `woc` builds a standalone binary, it embeds the image into a copy of `wovm`.

Build both from the repository root:

```bash
just woc-build      # builds compiler/_build/default/bin/woc
just wovm-build     # builds runtime/wovm

# gate them (optional but recommended)
just woc-test       # compiler unit + golden suites
just wovm-test      # runtime unit suites, both dispatch flavors, ASan-clean
```

---

## Your first program

A writeonce project is a directory with a `wo.toml` manifest and one or more
`.wo` files. Every program has an entry point:

```
-- hello/main.wo
fn main(args: multi Text) -> Int {
  print("hello, writeonce");
  return 0;
}
```

```toml
# hello/wo.toml
name    = "hello"
version = "0.1.0"

[runtime]
wo = ">= 0.1"
```

Compile the directory into a single binary and run it:

```bash
woc hello/           # produces hello/target/hello
./hello/target/hello
# hello, writeonce
```

`main` returns an `Int` — that value is the process **exit code**. `args` is
the command-line arguments (the program name is not included).

### The two build paths

```bash
# 1. standalone binary (what you ship): woc reads wo.toml, emits target/<name>
woc myproject/

# 2. image + VM (handy while developing): emit a .wob, run it with wovm
woc --emit myproject/ -o app.wob
wovm app.wob arg1 arg2
```

Both paths run the same program. The standalone binary is the release artifact;
the image path lets you inspect or move the image around.

---

## Language at a glance

writeonce is statically typed with a compile-time ownership model — every value
has a known owner, memory is freed deterministically, and values that form
cycles are collected by an inferred garbage collector (you never annotate GC-
ness; the compiler infers it). The surface will look familiar:

- **Types:** `Int`, `Float`, `Bool`, `Text`, `Bytes`, `Timestamp`, `Id`, and
  user `class` types. `?T` marks an optional (nullable) value; `nil` is the
  empty case. `Int` and `Float` never mix implicitly — `float` and `trunc` are
  the only bridges.
- **Containers:** `multi T` (a growable list) and `map<K, V>`. Literals:
  `[]`, `[a, b]`, `{}`.
- **Classes & records:** classes with fields and methods, `static const` /
  `static fn` members, module-scoped across files.
- **Control flow:** `if`/`else`, `for x in xs`, `for k, v in m`, `switch`
  expressions, and `try { … } catch (e) { … }` (also an expression form).
- **Strings:** interpolation with `${expr}` inside a `"…"` literal.
- **Functions:** free functions and methods; arguments and returns are typed.
- **Concurrency:** `spawn C { … }` starts an actor and yields an `actor M`
  address; `send` is fire-and-forget, `call` parks the calling fiber until the
  receive returns. A class becomes an actor by declaring `fn receive(msg: M)`.
  Blocking stdlib calls park the fiber — there is no `async`, no `await`, and no
  user-visible thread.

```
fn classify(n: Int) -> Text {
  if n < 0 { return "negative"; }
  return switch n {
    case 0: "zero";
    default: "positive";
  };
}
```

### Standard library

A compact set of OS modules, reached by their reserved names — no imports:

| Module | What it does |
| --- | --- |
| `fs` | `exists`, `list`, `stat`, `read_all`, `read_at`, `append` — read and append; a file cannot yet be replaced, truncated, deleted or renamed |
| `time` | `sleep`, `now`, `ticks` (µs monotonic), `local`, `iso` |
| `env` | `get`, `stopping` (a cooperative shutdown flag) |
| `net` | `listen` / `accept` / `read` / `write` / `close`, per-call deadline twins `read_dl` / `accept_dl` / `write_dl`, `listen_unix`, `peer`. Listeners only — there is no outbound `connect` |
| `proc` | `run` a child process, capture stdout/stderr/exit |
| `json` | `encode` / `decode` (`json.decode(t) as T` yields `?T`) |

These are deliberately minimal — the surface a real program needs, and no more.
Alongside them sit free builtins for text, containers, the `Float`/`Bytes`
bridges, `base64`, and the digests `sha1` / `sha256` / `hmac_sha256`. The full
list is `docs/guides/language-surface.md`.

---

## The database

This is the point of the language. Declaring storage is declaring a class:

```
@table(name: "departments", index: [name])
class Department {
  name:  Text @unique
  staff: backlink Employee.dept    -- reverse relation, not a stored column
}

@table(name: "employees", index: [dept], index: [dept, salary])
class Employee {
  name:   Text
  salary: Int
  hired:  Int
  dept:   ref Department           -- foreign key: stored as the row id
}
```

- **`@table`** makes a class persistent — named storage plus declared secondary
  indexes. Every instance you `insert` is written to a write-ahead log **before**
  it is acknowledged, so an acked write survives a crash; on the next start the
  log is replayed.
- **`ref T`** is a typed foreign key (a forward relation). **`backlink T.f`** is
  its inverse — a virtual field, no stored column, resolved by an index scan.
- **`@unique`** enforces uniqueness at insert/update; a violation is a
  **catchable** trap.
- **Foreign keys restrict deletes**: deleting a row that another row still
  references traps rather than orphaning it.

### Writing and reading data

Mutation is direct; queries are a comprehension the compiler lowers to engine
operations:

```
-- insert (WAL-durable); @unique makes a re-insert trap, and try/catch it:
let eng = try insert Department { name: "Engineering" } catch (e) nil;
insert Employee { name: "Asha", salary: 9200000, hired: 1704067200000, dept: eng };

-- query: filter, order, limit, project — checked at compile time
for e in from s in Employee where s.salary > 8000000 order by s.salary desc select s {
  print("${e.name} ${e.salary} (${e.dept.name})");   -- ref navigation
}

-- navigate a backlink (the department's staff), update through the result
for e in from s in dept.staff select s {
  e.salary = e.salary + e.salary * 5 / 100;           -- update-through-row
}

-- delete (restricted if still referenced)
let ok = try delete row catch (e) nil;
```

The query surface available today is **`from v in <table | relation> where …
[order by k [desc]] [take n] select v | v.field`**, plus `insert`, delete, and
update-through-a-row. It is proven end to end by the `employee` sample, whose
data survives a process restart via log replay.

---

## Project layout & the manifest

```
myproject/
├── wo.toml         # manifest: name, version, [runtime], [build]
├── main.wo         # entry point (fn main)
├── types.wo        # your @table classes, other types
└── target/         # build output (the standalone binary lands here)
```

```toml
name    = "myproject"
version = "0.1.0"

[runtime]
wo = ">= 0.1"

[build]
runtime = "../../../runtime/wovm"   # path to the wovm the binary is built from
```

`woc myproject/` compiles every `.wo` file under the directory as one program.

### Dependencies

A project can depend on other writeonce repositories — exact-rev git
dependencies, declared in the manifest:

```toml
[deps]
porch = { git = "https://github.com/shoneyj/porch", rev = "v0.1.0" }
```

**The `[deps]` key IS the module name** `use` imports — the repository name
never appears in your source. `woc` fetches each dep (via the `git` binary)
into `.wo-deps/<name>/`, pins the resolved commit in `wo.lock`, and `use porch`
(or `use porch/router`) imports its public names like any module. Builds never
touch the network once the lock is satisfied; a moved tag is reported, and
`woc --update-deps myproject/` refreshes the lock deliberately. Flat
dependencies only (a dep may not have its own `[deps]`) — honest and small,
by design.

Programs that create tables read their data directory from the `WO_DATA`
environment variable at run time:

```bash
WO_DATA=./data ./target/myproject seed
WO_DATA=./data ./target/myproject report     # a fresh process still sees the data
```

A program with any durable table (the default) refuses to start without `WO_DATA`; `WO_EPHEMERAL=1` opts into a RAM-only run, `@table(durable: false)` opts a table out.

---

## Worked examples

Thirteen sample programs live under `docs/examples/`; eight of them are wired to
a `just` recipe and double as the language's acceptance tests. The three worth
reading first:

- **`docs/examples/employee/`** — departments and employees related by
  `ref`/`backlink`, `@unique`, foreign-key restrict on delete, per-department
  reports, and persistence across a restart. Run it:

  ```bash
  just employee            # compile + run every mode against a durable database
  ```

- **`docs/examples/porch/` + `docs/examples/web-app/`** — a web
  framework written in writeonce (HTTP/1.1 behind a TLS-terminating proxy,
  router with `:param` captures, interface-based handlers) and a storefront
  consuming it **as a `[deps]` dependency**, with `@table` persistence. Run:

  ```bash
  just web-app
  ```

- **`docs/examples/log-watcher/`** — a long-running daemon that watches log
  files for silent death, using the `fs`/`time`/`net`/`proc` stdlib. Run it:

  ```bash
  just log-watcher
  ```

Read any of their `main.wo` files for idiomatic, working writeonce. The rest —
`site` (the writeonce.de tutorial, server-rendered, `just site`), `fibers`,
`db-actor`, `db-bench`, `gc-cycle`, `operators`, `shop` — cover the concurrency,
GC and benchmark surfaces.

---

## Roadmap

Planned, **not yet available** — listed so the shipped surface above stays
honest. These exist as design iterations and/or work-in-progress branches, not
as features you can use today:

- **Query aggregates** — `group … by … into g` with `count`/`avg`/`min`/`max`
  and projection records. The clause parses and is then refused by the
  typechecker; today the same result is written by hand from the shipped
  primitives.
- **File mutation and outbound sockets** — `fs` can create, grow and read a
  file but never replace, truncate, delete or rename one, and there is no
  `net.connect` at all, so nothing reaches out (no OIDC, SMTP, object store or
  webhook). Both are iteration 38.
- **`service` blocks** — a declaration form that routes requests to methods,
  lowering onto the framework library. Today you register routes as ordinary
  framework calls, which works and is what every sample does.
- **Cross-program database access** — one program attaching to another's
  database over a local channel, with keypair authentication and per-client
  rights.
- **Blue-green deployment** — in-process recompile and atomic version switch.
- **Compile-time metaprogramming** — `@derive(Json/Csv/Eq/…)` generated from a
  class's own metadata, no reflection.

Known current limits worth naming: `proc.run` has no timeout or signal control;
there is no stdin/stdout byte I/O and no FFI; `map` lookup is a linear scan;
actor mailboxes are bounded but there is no supervision tree yet; the WAL is
append-only, so it grows and boot replays all of it; TLS is always a proxy's
job.

---

*writeonce is a work in progress. Interfaces will change. If you build
something with it, pin to a commit.*
