# skill-catalog — the query grammar corpus (iteration 9g)

> Corpus #1 for the query-grammar method (story
> [09g](../../stories/language-runtime-database/09g-query-grammar-corpus.md)):
> take a real application backed by an embedded SQL database, translate its
> every statement to the writeonce query surface, and add only the grammar it
> forces. The application is `~/projects/skillhost` (a C++ MCP host whose
> in-memory SQLite holds its skill catalog).

**Result: skillhost forced no new grammar.** Its entire SQL footprint — one
table, a single-row `INSERT`, and four `SELECT`s (whose only non-trivial
features are `COUNT(*)` and a correlated `NOT EXISTS`) — is expressible on the
surface iteration 9b already shipped. The five statements, translated:

| skillhost SQL (`src/catalog/catalog.cpp`) | writeonce | mode |
| --- | --- | --- |
| `INSERT INTO skills (…) VALUES (?,…)` + `SQLITE_CONSTRAINT` dup check | `insert Skill { … }` + `try…catch` on the `@unique` trap | `seed` |
| `SELECT … WHERE name = ?` | `from x in Skill where x.name == n take 1 select x` | `get <name>` |
| `SELECT … ORDER BY name` | `from x in Skill order by x.name select x` | `list` |
| `SELECT COUNT(*) FROM skills` | `count(from x in Skill select x)` | `count` |
| `SELECT … WHERE NOT EXISTS (SELECT 1 FROM skills c WHERE c.parent = s.name) ORDER BY name` | `from x in Skill where len(x.children) == 0 order by x.name select x` | `roots` |

The one translation choice: skillhost's correlated `NOT EXISTS` (skills that
are nobody's parent) becomes a **backlink emptiness** — `Skill.children` is the
inverse of `parent`, and `len(x.children) == 0` is the childless test. No
subquery construct is needed; the general `exists`/`not exists` is deferred
until a corpus uses a correlation a backlink cannot express.

`skill-catalog seed | list | roots | count | get <name>`, WAL-durable under
`WO_DATA`. Acceptance: `scripts/skill-catalog-accept.sh`.
