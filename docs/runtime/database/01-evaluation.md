# Phase 1 — Database Evaluation

> Why build a custom database instead of adopting CouchDB, Postgres, Neo4j, or an in-memory graph library?

**Next**: [Phase 2 — The `.wo` Language & ACID Engine](./02-wo-language.md) | **Index**: [database.md](../database.md)

---

Reference repositories:

- [github.com/apache/couchdb](https://github.com/apache/couchdb) — document DB with Mango query language
- [github.com/postgres/postgres](https://github.com/postgres/postgres) — relational DB with JSONB
- [github.com/neo4j/neo4j](https://github.com/neo4j/neo4j) — native graph DB, Cypher query language
- [github.com/petgraph/petgraph](https://github.com/petgraph/petgraph) — Rust in-memory graph library (NetworkX analogue)

## The Question

writeonce articles have two shapes at once: they are **documents** (per-article JSON metadata + markdown body, per [06-markdown-render.md](../../06-markdown-render.md)) and they form a **graph** (the `mappings` field — `related`, `prerequisite`, `series`, `supersedes`, `references` — per [ai-agents-content-management.md](../../future-scope/ai-agents-content-management.md)).

Should writeonce adopt an off-the-shelf document or graph database to back these two shapes, or keep the flat-file `.seg` + `.idx` storage already implemented in [05-datalayer.md](../../05-datalayer.md)?

**Short answer: no external DB.** The dataset is small (hundreds of articles, not millions of rows), single-writer (author commits), and read-heavy. A full rebuild on change is cheap. The `mappings` graph fits entirely in RAM. External databases would add a process, a protocol, a driver, and a failure mode — none of which writeonce needs.

This doc walks through each option, what it buys, and why writeonce does not adopt it.

## The Data Shape

Every article is a pair of files in `content/{sys_title}/`:

```json
{
  "sys_title": "linux-misc",
  "title": "Linux Miscellaneous",
  "published": true,
  "author": "Shoney Arickathil",
  "tags": ["Linux"],
  "published_on": 1740950884,
  "mappings": {
    "related": ["auto-scale-gitlab-runner-using-aws-spot-instance"],
    "prerequisite": ["linux-misc"],
    "series": { "name": "gitlab-runner", "order": 2 }
  }
}
```

Plus `{sys_title}.md` with the full article body.

The access patterns, per [05-datalayer.md](../../05-datalayer.md):

| Pattern | Frequency | Current Implementation |
| --- | --- | --- |
| `get_by_title(sys_title)` | Every `/blog/:sys_title` hit | `title.idx` hash — O(1) |
| `list_published(skip, limit)` | Homepage, pagination | `date.idx` sorted array — O(log n) |
| `list_by_tag(tag)` | Tag pages | `tags.idx` inverted index — O(1) + scan |
| `list_by_date_range` | Archive views | `date.idx` binary search |
| Graph traversal (`mappings`) | Agent workflows, "related" widget | **Not yet implemented** |

The last row is the gap. Everything else already works on `.seg` + `.idx`.

## Option 1: CouchDB + Mango

CouchDB is an HTTP-native document store. Each article JSON would be a document; Mango queries are JSON selectors expressive enough for most writeonce reads:

```json
{ "selector": { "published": true, "tags": { "$in": ["rust"] } }, "sort": [{ "published_on": "desc" }] }
```

What it buys:

- Built-in revisions (MVCC) — each edit gets a `_rev`
- Multi-master replication, which matters if content is authored from multiple machines
- A _changes feed, conceptually similar to writeonce's subscription model

What it costs:

- Separate Erlang process, HTTP driver, JSON over the wire on every read
- No native graph traversal — `mappings` would require client-side joins or view functions
- Duplicates what inotify + `.seg` already do (change feed, indexing)

### Comparison

| Aspect | CouchDB | writeonce |
| --- | --- | --- |
| Transport | HTTP per query | In-process function call |
| Storage | B-tree per database, append-only | `.seg` append-only, tombstoned records |
| Change feed | `_changes` HTTP long-poll | inotify → epoll → fd write |
| Index | View functions (JavaScript map/reduce), Mango indexes | `title.idx`, `date.idx`, `tags.idx` rebuilt on change |
| Revisions | Every write creates `_rev` | Git already does this for `content/` |

CouchDB's replication and revision model are attractive, but git already covers revision history for `content/`, and the dataset is too small to justify a separate daemon.

## Option 2: PostgreSQL + JSONB

Postgres with a `jsonb` column gives you SQL over the article metadata, GIN indexes for tag queries, and a real query planner. Schema would be roughly:

```sql
CREATE TABLE articles (
  sys_title TEXT PRIMARY KEY,
  metadata  JSONB NOT NULL,
  body_md   TEXT NOT NULL,
  updated   TIMESTAMPTZ DEFAULT now()
);
CREATE INDEX ON articles USING GIN ((metadata->'tags'));
CREATE INDEX ON articles (((metadata->>'published_on')::bigint));
```

What it buys:

- Mature: WAL, point-in-time recovery, replication, tooling
- `jsonb_path_query` and `@>` containment make most writeonce queries one-liners
- Recursive CTEs (`WITH RECURSIVE`) can traverse `mappings` — adequate for shallow graphs

What it costs:

- A Postgres process, a driver (tokio-postgres or raw libpq), connection pooling — writeonce's [05-datalayer.md](../../05-datalayer.md) explicitly removed all of this
- JSONB query planning is excellent but still pays per-query cost that an in-process hash does not
- Recursive CTEs on deep mapping chains are slower than a RAM graph walk

### Comparison

| Aspect | Postgres JSONB | writeonce |
| --- | --- | --- |
| Process model | External daemon, TCP or unix socket | Single binary, single process |
| Query language | SQL + JSONB operators | Rust method calls on `Store` |
| Graph traversal | `WITH RECURSIVE` CTE | (Future) in-memory adjacency list |
| Backup | `pg_dump`, WAL archive | `content/` directory + git |
| Failure modes | Connection loss, pool exhaustion, vacuum stalls | File not found |

Postgres is the default reflex for "I have structured data." But writeonce's structured data is ~500 rows that change when the author saves a file. The mismatch is two orders of magnitude on the dataset size and one process on the deployment surface.

## Option 3: Neo4j — Native Graph DB

Neo4j models articles as nodes and `mappings` as typed edges. Cypher makes the traversals natural:

```cypher
MATCH (a:Article {sys_title: 'linux-misc'})-[:PREREQUISITE*1..3]->(p:Article)
RETURN p.sys_title
```

What it buys:

- Native graph storage — constant-time edge traversal regardless of dataset size
- Cypher is the right query language for the `mappings` problem
- Useful when the graph is the primary shape

What it costs:

- JVM process, Bolt protocol, driver — heaviest option on this list
- Document storage is secondary (properties on nodes), so article body and metadata are awkwardly split
- Dataset is tiny — the graph fits in a few KB of RAM; Neo4j's disk-backed adjacency is overkill

### Comparison

| Aspect | Neo4j | writeonce |
| --- | --- | --- |
| Storage | Native adjacency on disk | (Future) `HashMap<sys_title, Vec<Edge>>` in memory |
| Traversal | Cypher over Bolt | Rust iteration over adjacency |
| Transactions | ACID with MVCC | Full rebuild on change |
| Deployment | JVM daemon + driver | Statically linked binary |
| Fit for writeonce dataset | Overprovisioned by 3+ orders of magnitude | Right-sized |

## Option 4: In-Memory Graph — NetworkX / petgraph

NetworkX (Python) and petgraph (Rust) are _libraries_, not databases. You load the graph into process memory and traverse it directly. This is the model gestured at in [ai-agents-content-management.md line 181](../../future-scope/ai-agents-content-management.md) — "traversable knowledge graphs available on RAM."

For writeonce, petgraph is the right shape:

```rust
use petgraph::graph::DiGraph;

let mut g: DiGraph<String, MappingKind> = DiGraph::new();
// nodes: one per sys_title
// edges: one per mapping (related, prerequisite, series, ...)
```

What it buys:

- Zero extra processes — compiles into the `wo-store` crate
- Constant-time neighbor lookup, standard BFS / Dijkstra / SCC algorithms included
- Rebuilt cheaply on any `content/` change by walking the `.seg` and following `mappings`

What it costs:

- No persistence layer — but the graph is derived from JSON, same as `title.idx`, so it rebuilds on cold start for free
- No query language — but the traversals agents need (`all prerequisites of X`, `next article in series Y`) are short Rust functions

### Comparison

| Aspect | petgraph (in-memory) | Neo4j |
| --- | --- | --- |
| Location | Same process as `wo-store` | External JVM |
| Build time | Single pass over `.seg` | Bulk import via CSV / Cypher |
| Cost per traversal | Pointer chase in RAM | Network round trip + disk I/O |
| Query language | Rust | Cypher |
| Scales to | Millions of nodes in RAM (plenty of headroom) | Billions on disk |

For writeonce's hundreds of articles, petgraph is the correct answer. It fits the existing architecture — derived from `content/`, rebuildable, no external process — the same shape as `title.idx` already has.

## Decision Matrix

| Option | Dataset Fit | Process Count | Graph Support | Matches writeonce Philosophy |
| --- | --- | --- | --- | --- |
| CouchDB Mango | Overprovisioned | +1 (Erlang) | Manual joins | No — external daemon |
| Postgres JSONB | Overprovisioned | +1 (Postgres) | Recursive CTE | No — external daemon |
| Neo4j | Overprovisioned | +1 (JVM) | Native, excellent | No — external daemon |
| petgraph in-memory | Right-sized | 0 | Native, in-process | **Yes** |
| Current `.seg` + `.idx` | Right-sized | 0 | None yet | Already here |

## Proposed Addition: `mappings.idx` Backed by petgraph

Per [05-datalayer.md](../../05-datalayer.md), indexes live alongside `.seg`. Add a fourth index:

```
data/
  articles.seg
  index/
    title.idx        # existing — O(1) sys_title lookup
    date.idx         # existing — sorted by published_on
    tags.idx         # existing — inverted index by tag
    mappings.idx     # NEW — serialized petgraph adjacency
```

New `wo-graph` crate (or extension to `wo-index`):

```rust
pub struct MappingGraph {
    graph: DiGraph<SysTitle, MappingKind>,
    by_title: HashMap<String, NodeIndex>,
}

impl MappingGraph {
    pub fn neighbors(&self, sys_title: &str, kind: MappingKind) -> Vec<&str> { ... }
    pub fn prerequisites_transitive(&self, sys_title: &str) -> Vec<&str> { ... }
    pub fn series(&self, name: &str) -> Vec<&str> { ... }  // ordered by .order
}
```

Rebuild on every `Store::rebuild()`. Drop and recompute on any `ContentChange` — the graph is small enough that incremental updates are not worth the bug surface.

## Key Takeaways

1. **A document+graph database is the right model — but not the right dependency.** writeonce's articles really are documents with graph edges. That does not mean importing CouchDB, Postgres, or Neo4j. It means writing ~200 lines that give you the document and graph operations the product actually uses.

2. **Dataset size dictates architecture.** SurrealDB, Postgres, and Neo4j all assume millions of rows and concurrent writers. writeonce has hundreds of articles and one author. The gap is where external databases become overhead, not infrastructure.

3. **Git already solves the problems CouchDB's revisions solve.** Content lives in `content/` under version control. `_rev`, replication, and change history are the author's git history.

4. **Query languages are a cost, not a benefit, at this scale.** Mango selectors, JSONB operators, and Cypher exist because production queries are written by humans against large evolving datasets. writeonce's queries are fixed (`get_by_title`, `list_by_tag`, `list_published`) and written once in Rust.

5. **The graph belongs in RAM.** Article `mappings` form a small, mostly static DAG. petgraph holds it with zero protocol overhead and rebuilds from `content/` on cold start — same lifecycle as `title.idx`.

## Reference

If the graph side of this ever grows beyond what petgraph comfortably handles, the reference points are:

```bash
git submodule add https://github.com/neo4j/neo4j.git references/neo4j
git submodule add https://github.com/apache/couchdb.git references/couchdb
git submodule add https://github.com/petgraph/petgraph.git references/petgraph
```

Key files to study:

- `petgraph/src/graph_impl/` — adjacency list implementation, the smallest viable graph backend
- `couchdb/src/mango/` — Mango query compilation, if a selector-style query API ever becomes useful
- `neo4j/community/cypher/` — Cypher planner, for how a real graph query language is structured

See also:

- [surreal-case-study.md](../surreal-case-study.md) — why writeonce does not use a multi-model DB for live queries
- [05-datalayer.md](../../05-datalayer.md) — current `.seg` + `.idx` implementation
- [ai-agents-content-management.md](../../future-scope/ai-agents-content-management.md) — the `mappings` feature this index supports
