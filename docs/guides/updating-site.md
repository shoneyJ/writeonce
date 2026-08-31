# Updating the writeonce.de application

> **Status:** current as of 2026-08-31. This is the DEVELOPER loop — changing
> the site's code, content, or schema. Shipping the result to the host is
> [`deploying-site.md`](deploying-site.md); this guide ends where that one
> begins. Layout and environment live in the site's own
> [README](../examples/site/README.md).

## Where the code actually lives

`docs/examples/site` is a **submodule** of
`github.com/shoneyJ/writeonce-site`. The monorepo tracks it by revision, not
by content, which makes one mistake easy and expensive:

**Editing files under `docs/examples/site` and committing in the monorepo
records nothing.** The change lives in a directory the monorepo only points
at. The working order is:

```
cd docs/examples/site
git checkout master            # the submodule checks out detached by default
# ...edit, test (below)...
git add <files> && git commit
git push                       # writeonce-site must have it BEFORE the bump
cd ../../..
git add docs/examples/site     # the pointer, nothing else
git commit                     # "bump site to <short-hash>: <what changed>"
```

Push the submodule first: a monorepo pointer naming a commit that exists only
on one laptop breaks every other clone's `git submodule update`.

The site's gate lives in the **monorepo** (`scripts/site-accept.sh`), not the
submodule — a change that needs a new gate leg is two coordinated commits by
design: behaviour in `writeonce-site`, its proof in the monorepo.

## The loop

1. Edit in `docs/examples/site/`.
2. `just site` — builds against `docs/examples/porch` and
   `docs/examples/writeonce-view` as local `file://` remotes and runs the
   whole matrix (23 checks: pages, escaping, 404, 401, authed edit, SIGTERM,
   WAL persistence). This is the only signal worth trusting; the site cannot
   even build standalone (its `[deps]` point at unpublished repos).
3. New behaviour gets a leg in `site-accept.sh` in the same change — the
   storage chapter's two legs are the pattern to copy.
4. Commit the pair as above.

Framework work is different: `porch` and `writeonce-view` are ordinary
monorepo directories (`docs/examples/porch`, `docs/examples/writeonce-view`),
so a site change that needs a framework change is a normal monorepo commit
plus the submodule pair. The `rev = "v0.1.0"` pins in `wo.toml` are
aspirational until those two repos are published.

## Content updates — chapter text, new chapters

Chapter bodies are code (`content.wo` builders), so a reworded chapter is an
ordinary edit: the compiler checks it, `code_block()` escapes it, `just site`
proves it renders.

What content edits do NOT do is reach a host that already has a `WO_DATA`
directory: `seed_if_empty()` fills an empty table only, and the admin route
cannot create a chapter. A new chapter (or renumbered `ord`s) therefore ships
with the data-refresh step in
[`deploying-site.md`](deploying-site.md#refreshing-content--trap-1-in-practice)
— measured there, silent otherwise.

## Schema updates — changing the `Chapter` class

Two facts, both proven against the real app on 2026-08-31:

**The compiler makes you finish the job at compile time.** There is no
field-default syntax, so adding a field breaks every insert literal until the
seeds carry it:

```
content.wo:105:3: error WO-E206: missing field `views` in insert of `Chapter`
```

All ten seed inserts (and any other `insert Chapter { ... }`) must name the
new field. This is a feature: the seed can never silently drift from the
schema.

**The live database migrates itself at boot** (schema migrations, databasev2
12). Booting the new binary against the existing `WO_DATA`:

```
wovm: .../shard-0.wal: migrating `Chapter`: +views
wovm: .../shard-0.wal: schema migrated
```

- All ten chapters render; the new field reads the kind's zero.
- **Live admin edits survive** — unlike the content-refresh wipe, a schema
  migration rewrites rows in place. (An edited title stayed edited through
  the `+views` migration.)
- Deleting a field drops its stored values at the swap, permanently.
- A retype refuses to start, by name, with the old log intact:

```
wovm: ...: refusing to start — class `Chapter`: field `views` changed its
type — v1 has no conversions; add a new field and backfill instead
```

- A rename reads as delete+add of the same type and refuses the same way:
  deploy the delete and the add as two separate releases when both are meant.
- Rolling back a deployed schema change is a migration in the other
  direction; the old binary refuses the migrated log — see the rollback
  section of `deploying-site.md`.

So a schema change ships as: edit `types.wo`, satisfy `WO-E206` everywhere,
`just site`, the two-repo commit pair, deploy the binary — no data step,
unlike content. The one combination that still needs the wipe is a schema
change WITH new seed content in the same release: the migration handles the
shape, the seeds still cannot reach a non-empty table.

## Evidence

The schema claims above were exercised against the built site, not inferred:
seeded a `WO_DATA` under v1, edited a chapter through `/admin`, rebuilt with
`views: Int` added (ten seed inserts updated after `WO-E206` stopped the
build), booted against the same data — migration lines printed, 10 chapters
rendered, the edit survived, `/ch/storage` intact — then retyped `views` to
`Float` and got the refusal, exit 2, data untouched.
