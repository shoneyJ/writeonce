# wo-db — `.wo` language prototype (C++)

Phase 2 milestone 1 from [docs/runtime/database.md](../../docs/runtime/database.md):
**parser + analyzer + in-memory executor, single-user, no durability.**

Implements a cut-down `.wo` dialect spanning all three paradigms described in
[02-wo-language.md](../../docs/runtime/database/02-wo-language.md): relational,
document, and graph — all in one grammar, one process, one in-RAM store.

## Build & run

```bash
make                    # builds build/wo-db
make test               # runs tests/smoke.wo + tests/checkout.wo
./build/wo-db           # interactive REPL
./build/wo-db < file.wo # batch mode
```

Requires `g++` with C++20 (tested on 13.3). No external dependencies.

## Supported grammar

### Schema

```wo
##sql
#users
    id int
    name string
    email string

##doc
#article_meta
    id int
    title string

##graph
#recommendations
    (user)-[:PURCHASED]->(product)
```

Paradigms: `sql`, `doc`, `graph`. Types are parsed but not enforced (prototype).
Graph schema blocks are documentation only — nodes and edges are created at
runtime by `CREATE`.

### Queries

```wo
-- relational
INSERT INTO users (id, name) VALUES (1, 'Alice');
SELECT id, name FROM users WHERE id > 1 AND name != 'Carol';
UPDATE users SET email = 'a@b.com' WHERE id = 1;
DELETE FROM users WHERE id = 3;

-- document (same syntax as sql; dotted columns write nested objects)
INSERT INTO article_meta (id, title) VALUES (100, 'Intro');
SELECT title FROM article_meta;

-- graph
CREATE (u:user {id: 1, name: 'Alice'});
CREATE (u:user {id: 1})-[:PURCHASED {qty: 2}]->(p:product {id: 10});
MATCH (u:user {id: 1})-[:PURCHASED]->(p:product) RETURN u, p;
```

### Fixed-glue: parameters, RETURNING, transactions, LIVE

The five things from the two-layer design ([docs/runtime/database/02-wo-language.md](../../docs/runtime/database/02-wo-language.md)) that tie the three grammars together:

```wo
-- $name parameters work everywhere (SQL, Cypher, expressions)
INSERT INTO users (email) VALUES ('a@b.com') RETURNING id AS uid;
SELECT * FROM users WHERE id = $uid;

-- cross-paradigm RETURNING threads ids from SQL into Cypher
BEGIN SNAPSHOT;
  INSERT INTO orders (user_id, status) VALUES ($uid, 'pending') RETURNING id AS oid;
  CREATE (u:user {id: $uid})-[:PURCHASED {order_id: $oid}]->(p:product {id: $pid});
COMMIT;

-- SAVEPOINT / ROLLBACK TO are parsed but not yet enforced in the prototype
BEGIN;
  SAVEPOINT s1;
  INSERT INTO users (email) VALUES ('typo@example.com');
  ROLLBACK TO s1;
COMMIT;

-- LIVE prefix reserved — inner query runs now, subscription activation in Phase 3
LIVE SELECT id, email FROM users;
LIVE MATCH (u:user)-[:PURCHASED]->(p:product) RETURN u, p;
```

Auto-populated `id` columns: when a table declares an `id int` column and `INSERT` omits it, the engine mints a fresh id from a per-table counter and fills it in. Combined with `RETURNING id AS alias`, this is how ids thread from SQL into Cypher inside a transaction without `LAST_INSERT_ID()`.

### Expressions

Literals (`int`, `string`, `true`/`false`/`null`, `[array]`, `{object}`),
dotted paths (`meta.title`), comparisons (`= != < <= > >=`), boolean
(`AND OR NOT`).

### REPL meta-commands

```
.tables     list sql/doc tables
.schema     dump schema
.exit
```

## What's intentionally missing

This is a Phase 2 *prototype*, not a database. It does not implement:

- **real** transactions — `BEGIN/COMMIT/SAVEPOINT/ROLLBACK` parse and are acknowledged, but no atomic rollback, no MVCC, no WAL
- **real** `LIVE` subscriptions — the inner query runs; no delta frames, no push (Phase 3/4)
- durability, crash recovery (Phase 3)
- real document operations (array push/splice, deep path updates beyond simple dotted SET)
- joins, aggregations, subqueries
- the LSM document engine, B+ tree relational pages, native graph adjacency (Phase 3)
- wire protocol, auth (Phase 4+)

See [docs/runtime/database.md](../../docs/runtime/database.md) for the full phase plan
and [docs/runtime/database/02-wo-language.md](../../docs/runtime/database/02-wo-language.md)
for the target language spec.

## Source layout

```
src/
  value.hpp     tagged Value (null/bool/int/string/array/object)
  lexer.{hpp,cpp}   tokenizer
  ast.hpp       AST node types
  parser.{hpp,cpp}  recursive-descent parser
  storage.{hpp,cpp} in-memory Database (sql tables + doc tables + graph)
  executor.{hpp,cpp} walks AST, returns ResultSet
  main.cpp      REPL + batch driver
tests/
  smoke.wo      core statement coverage
  checkout.wo   cross-paradigm RETURNING + BEGIN/SAVEPOINT/COMMIT + LIVE
```

Roughly 1.1k lines of C++ in the `wo` namespace.
