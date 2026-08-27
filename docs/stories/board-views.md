# Live board views (Obsidian Dataview)

Every story iteration file carries YAML frontmatter — **the frontmatter
is the source of truth**:

```yaml
---
track: porch          # OMITTED on language-runtime-database stories
iteration: "8"        # immutable id, LOCAL TO ITS TRACK (string: "7b", "9b" exist)
status: in-progress   # done | in-progress | pending | hold — where the WORK is
readiness: ready      # ready | refine — whether the DESIGN is settled
chain: 1              # concurrency-chain position, chain stories only (1–6)
---
```

**Editing `status:` IS the status change.** Story files sit flat in
`docs/stories/language-runtime-database/`; no directory encodes state, so
there is nothing to move and nothing that can disagree. This replaced the
2026-08-20/21 folder scheme on **2026-08-26** — under that scheme a status
change moved the file, which broke every relative link in and to it, and
the repo's two link audits were largely the cleanup.

**Two axes, since 2026-08-27.** They are orthogonal and conflating them is
what `refine` used to do:

- `status` — where the WORK is: `done`, `in-progress`, `pending`, `hold`.
- `readiness` — whether the DESIGN is settled: `ready` means the brainstorm is
  complete and the decisions are LOCKED (a spec is approved, or the forks were
  confirmed); `refine` means open forks remain and it cannot be planned yet.

`status: refine` is retired. It meant both "not started" and "design not
settled", so a held iteration with an approved spec (language 18, 26) was
indistinguishable from one nobody had thought about. Those are now
`status: hold` + `readiness: ready`, and the genuinely unthought ones are
`readiness: refine`. A value outside either closed set simply will not appear
in the lanes below, which is the cheapest possible validation.

**The useful query is `readiness: ready` + `status: pending`** — design locked,
work not started. That is the startable set. The prose board ([`00-status.md`](00-status.md)) stays the
standup narrative; these queries are the live views over the same facts.

Adjust the `FROM` path to your vault root (queries below assume the
vault opens at the repo root).

Three tracks now carry iterations, each numbered from 1:
`language-runtime-database/` (the language and runtime), `porch/` (the web
framework) and `databasev2/` (the database beyond RAM) — the latter two added
2026-08-26. Iteration ids therefore repeat across tracks, so every query below is
scoped by `FROM` path; non-language stories carry `track:`, and iterations moved
between tracks keep `was_language_iteration:` so the old number stays
searchable.

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

## The databasev2 track

```dataview
TABLE iteration, status, was_language_iteration AS "was"
FROM "docs/stories/databasev2"
WHERE status != "done"
SORT iteration ASC
```

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

## Startable: design locked, work not started

The set to pick from. Anything here has its decisions made and needs no
brainstorm.

```dataview
TABLE track, iteration
FROM "docs/stories"
WHERE readiness = "ready" AND status = "pending"
SORT track ASC, iteration ASC
```

## Needs a brainstorm before it can be planned

```dataview
TABLE track, iteration, status
FROM "docs/stories"
WHERE readiness = "refine"
SORT track ASC, iteration ASC
```
