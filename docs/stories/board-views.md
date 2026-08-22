# Live board views (Obsidian Dataview)

Every story iteration file carries YAML frontmatter — **the frontmatter
is the source of truth**:

```yaml
---
iteration: "8"        # immutable id (string: "7b", "9b" exist)
status: in-progress   # done | in-progress | refine | hold — mirrors its folder
chain: 1              # concurrency-chain position, chain stories only (1–6)
---
```

The folder move IS the status change: moving a story between `done/`,
`in-progress/`, `refine/`, `hold/` must update its `status:` in the same
change — the two never disagree. The prose board
([`00-status.md`](00-status.md)) stays the standup narrative; these
queries are the live views over the same facts.

Adjust the `FROM` path to your vault root (queries below assume the
vault opens at the repo root).

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
the place status is edited.** Status changes happen by moving the story
file between folders + updating its `status:` key (one commit); a
Kanban card drag that only rewrites the Kanban file is a lie the next
query won't see.
