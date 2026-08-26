# Live board views (Obsidian Dataview)

Every story iteration file carries YAML frontmatter — **the frontmatter
is the source of truth**:

```yaml
---
track: porch          # OMITTED on language-runtime-database stories
iteration: "8"        # immutable id, LOCAL TO ITS TRACK (string: "7b", "9b" exist)
status: in-progress   # done | in-progress | refine | hold — the ONLY place status lives
chain: 1              # concurrency-chain position, chain stories only (1–6)
---
```

**Editing `status:` IS the status change.** Story files sit flat in
`docs/stories/language-runtime-database/`; no directory encodes state, so
there is nothing to move and nothing that can disagree. This replaced the
2026-08-20/21 folder scheme on **2026-08-26** — under that scheme a status
change moved the file, which broke every relative link in and to it, and
the repo's two link audits were largely the cleanup.

The closed set `status:` may take is `done`, `in-progress`, `refine` (needs
a brainstorm before it can be planned) and `hold`. A value outside it will
simply not appear in the lanes below, which is the cheapest possible
validation. The prose board ([`00-status.md`](00-status.md)) stays the
standup narrative; these queries are the live views over the same facts.

Adjust the `FROM` path to your vault root (queries below assume the
vault opens at the repo root).

Two tracks now carry iterations, each numbered from 1:
`language-runtime-database/` (the language, runtime and database) and `porch/`
(the web framework, added 2026-08-26). Iteration ids therefore repeat across
tracks — a porch 3 is not a language 3 — so every query below is scoped by
`FROM` path, and porch stories carry `track: porch` so a combined query can
still tell them apart.

## Everything not done, chain order first

```dataview
TABLE iteration, status, chain
FROM "docs/stories/language-runtime-database"
WHERE status != "done"
SORT chain ASC, iteration ASC
```

## Grouped by status (the kanban lanes, as data)

```dataview
TABLE rows.file.link AS story, rows.iteration AS iteration
FROM "docs/stories/language-runtime-database"
WHERE status != "done"
GROUP BY status
```

## The concurrency chain, in execution order

```dataview
TABLE iteration, status
FROM "docs/stories/language-runtime-database"
WHERE chain
SORT chain ASC
```

## Active right now

```dataview
LIST
FROM "docs/stories/language-runtime-database"
WHERE status = "in-progress"
```

## Kanban caveat

The Kanban plugin stores board state in its own markdown file — a
second copy of status. To keep frontmatter the single source of truth:
**use Dataview for querying; treat any Kanban board as a VIEW, never
the place status is edited.** A status change is one edit to one
`status:` key; a Kanban card drag that only rewrites the Kanban file is a
lie the next query won't see.

## The porch track

```dataview
TABLE iteration, status
FROM "docs/stories/porch"
WHERE status != "done"
SORT iteration ASC
```

## Both tracks at once, grouped

Relies on `track:` being present on porch stories and absent on language ones,
so the language track shows up under an empty group.

```dataview
TABLE rows.file.link AS story, rows.iteration AS iteration, rows.status AS status
FROM "docs/stories"
WHERE iteration AND status != "done"
GROUP BY track
```
