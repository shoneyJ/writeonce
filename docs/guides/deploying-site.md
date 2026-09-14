# Redeploying writeonce.de

> **Status:** current as of 2026-08-30. Companion to
> [`releasing.md`](releasing.md), which covers cutting a language release;
> this covers shipping the *site* that advertises it. Layout, nginx and the
> environment variables are documented once, in the site's own
> [README](../examples/site/README.md) — this is the update runbook, and the
> three things that go wrong. Changing the application itself (code,
> content, schema) is [`updating-site.md`](updating-site.md).

## Read this first — three traps, in the order they bite

**1. Shipping a new binary does NOT update the chapters.** `seed_if_empty()`
seeds `content.wo` only into an *empty* `Chapter` table, and
`POST /admin/ch/:slug` answers `not_found()` when the slug does not already
exist — it can update a chapter, never create one. So on a host that already
has a `WO_DATA` directory, a redeploy carrying a brand-new chapter shows the
**old** chapter list forever, with no error anywhere. Renumbered `ord` values
are equally invisible. See *Refreshing content* below; this is the step people
skip.

Measured 2026-08-30, not reasoned about — a 9-chapter build seeded a fresh
`WO_DATA`, then the 10-chapter binary ran against that same directory:

| binary | `WO_DATA` | `/ch/storage` | chapters in nav |
| --- | --- | --- | --- |
| 9-chapter | fresh | 404 | 9 |
| **10-chapter** | **the 9-chapter one** | **404** | **9** |
| 10-chapter | wiped | 200 | 10 |

The middle row is the trap: a correct binary, a healthy process, a 200 on
every other route, and the new chapter simply absent.

**2. The site cannot be built from its own repository.** `docs/examples/site`
is a submodule of `github.com/shoneyJ/writeonce-site`, but its two `[deps]`
point at `github.com/shoneyj/porch` and `github.com/shoneyj/writeonce-view`,
and **both of those 404** — they have never been published. `wo.lock` is not
committed either, so there is no offline fallback. The only way to build is
from a monorepo checkout, substituting local `file://` remotes built out of
`docs/examples/porch` and `docs/examples/writeonce-view`, exactly as
`scripts/site-accept.sh` does. Cloning `writeonce-site` alone and running
`woc .` fails at dependency resolution.

**3. The binary inherits the build machine's glibc floor.** `woc` embeds the
`wovm` you point `[build] runtime` at, so the deployed binary requires
whatever that VM requires. This dev machine's `wovm` needs **GLIBC_2.38**,
which does not exist on Ubuntu 22.04 (2.35). Build on a machine whose glibc
is no newer than the host's, or the binary dies on `exec` with a loader
error that says nothing about deployment. `releasing.md` step 8 covers the
same trap for release tarballs.

## What actually travels to the host

Three things, and only the first is a build artifact:

| on the host | what it is | comes from |
| --- | --- | --- |
| `site` | the binary — VM, bytecode and database engine inside it | the build below |
| `dist/` | what `/dl` serves | **fetched from the GitHub release**, never built locally |
| `data/` | `WO_DATA` — the WAL holding the chapters | created once, then it is state |

`dist/` must hold the *published* assets. `just dist` produces a different
digest on every run, so a locally built tarball would not match the
`.sha256` the release publishes, and the mirror would disagree with GitHub:

```
gh release download v0.1.0 -D dist -R shoneyJ/writeonce
```

## Build

From a monorepo checkout, with the submodule present:

```
git submodule update --init docs/examples/site
just woc-build && just wovm-build
```

Then stage the two dependencies as local git remotes and build against them.
This is the same substitution `scripts/site-accept.sh` performs — read it if
anything below drifts, it is the executable version of this section:

```
ROOT=$(pwd)
W=$(mktemp -d)

cp -r "$ROOT/docs/examples/porch"          "$W/fw"
cp -r "$ROOT/docs/examples/writeonce-view" "$W/lib"
for d in "$W/fw" "$W/lib"; do
  git -C "$d" init -q
  git -C "$d" add -A
  git -C "$d" -c user.email=b@b -c user.name=b commit -qm v01
  git -C "$d" tag v0.1.0
done

cp -r "$ROOT/docs/examples/site" "$W/app"
sed -i "s|https://github.com/shoneyj/porch|file://$W/fw|; \
        s|https://github.com/shoneyj/writeonce-view|file://$W/lib|" "$W/app/wo.toml"
printf '[build]\nruntime = "%s"\n' "$ROOT/runtime/wovm" >> "$W/app/wo.toml"

"$ROOT/compiler/_build/default/bin/woc" "$W/app"
```

That leaves the binary at `$W/app/target/site`. Do not commit the rewritten
`wo.toml` or the generated `wo.lock` — they name a temporary directory.

Prove it before it leaves the building. `just site` runs the whole matrix
(build, pages, escaping, 404, 401, an authed edit, SIGTERM, and an edit
surviving a restart) and is the only signal worth trusting:

```
just site        # expect: site-accept: 23 checks, 0 failures
```

The recipe above was run verbatim on 2026-08-30: binary at
`target/site` (293 531 bytes), `wo.lock` written, and the served pages
answering `/health`, the homepage, `/ch/storage`, a 404, a 401 and the `/dl`
tarball. The one warning it prints (`WO-W202: unused use porch/internal`,
inside the dependency) is pre-existing and not a build failure.

## Refreshing content — trap 1, in practice

Chapter bodies live in `content.wo`, in git. That is their source of truth;
`WO_DATA` is a *replica* seeded on first boot (a program with any durable table (the default) refuses to start without `WO_DATA`; `WO_EPHEMERAL=1` opts into a RAM-only run, `@table(durable: false)` opts a table out). The site's only table is
`Chapter`, so the WAL holds nothing else — which is what makes the fix safe:

```
systemctl stop writeonce-site
mv /srv/writeonce-site/data /srv/writeonce-site/data.bak-$(date +%F)
install -m755 site /srv/writeonce-site/site
systemctl start writeonce-site       # empty WO_DATA -> seeds all chapters
```

The only thing lost is **live admin edits** — anything typed through
`POST /admin/ch/:slug` rather than committed to `content.wo`. Keep the
`.bak` directory until you have confirmed you did not want any of them.

If you must preserve live edits, there is no supported path: the admin route
cannot create the new chapter, so adding one means either the wipe above or a
code change to `AdminEdit` (see *Known gaps*).

Redeploying with **no content change** needs none of this — replace the
binary and restart; the WAL replays and the chapters are untouched.

## Run it under systemd

The README says to use systemd but ships no unit. This one matches the
environment contract:

```ini
[Unit]
Description=writeonce.de
After=network-online.target

[Service]
ExecStart=/srv/writeonce-site/site 8080
WorkingDirectory=/srv/writeonce-site
Environment=SITE_TOKEN=<bearer for /admin>
Environment=WO_DATA=/srv/writeonce-site/data
Environment=WO_DIST=/srv/writeonce-site/dist
# SITE_HOST deliberately UNSET: the process binds 127.0.0.1 and is
# reachable only through nginx. Set it only to expose the port directly.
Restart=on-failure
KillSignal=SIGTERM
TimeoutStopSec=30

[Install]
WantedBy=multi-user.target
```

`SITE_TOKEN` is required — the process refuses to start without it. SIGTERM
drains cleanly, so `systemctl stop` and `restart` are safe. TLS terminates at
nginx; the framework speaks HTTP/1.1 keep-alive and no TLS by design.

## Verify after deploying

Check the things that break silently, not just that the port answers:

```
curl -fsS localhost:8080/health                      # the liveness probe
curl -fsS localhost:8080/ | grep -c 'Learn writeonce' # homepage rendered
curl -fsS localhost:8080/ch/storage | grep -c 'resident: keys'
curl -fsS -o /dev/null -w '%{http_code}\n' localhost:8080/ch/nope   # 404
curl -fsS -o /dev/null -w '%{http_code}\n' \
     -X POST -d title=x localhost:8080/admin/ch/hello               # 401
curl -fsSI localhost:8080/dl/writeonce-0.1.0-linux-amd64.tar.gz | head -1
```

The third line is the one that catches trap 1: if it prints `0`, the new
chapter is missing and `WO_DATA` was never reseeded. Substitute whichever
slug you just added.

The startup log prints the bound address — the quickest confirmation that
`SITE_HOST` is unset and the process is on loopback.

## Rollback

Keep the previous binary. Nothing in a redeploy migrates the WAL, so rolling
the binary back is just replacing the file — *unless* you wiped `data/`, in
which case restore the `.bak` directory alongside it:

```
systemctl stop writeonce-site
install -m755 site.prev /srv/writeonce-site/site
rm -rf /srv/writeonce-site/data && mv /srv/writeonce-site/data.bak-<date> /srv/writeonce-site/data
systemctl start writeonce-site
```

Schema changes are handled since databasev2 12: a binary whose `@table`
classes gained or lost fields migrates the WAL at startup (the log's head
record carries the shape that wrote it), and an incompatible change — a
retyped field, a vanished class — refuses to start by name instead of
reporting corruption. Rolling BACK across a migration is itself a schema
change in the other direction: the old binary predates the head record's
shape, so expect the same refusal — restore the `.bak` data directory
alongside the old binary rather than pointing it at migrated data.

## Known gaps

Each of these turns a documented workaround above into something that would
not need documenting:

- **`AdminEdit` cannot create a chapter**, so adding content requires wiping
  `WO_DATA`. An upsert arm — or a seed that reconciles `content.wo` against
  the table on boot instead of only seeding an empty one — removes trap 1.
- **`porch` and `writeonce-view` are unpublished**, so the site submodule
  cannot build standalone and every build needs the monorepo plus a `sed`.
  Publishing the two repos, or committing `wo.lock`, removes trap 2.
- **No deploy script.** The build recipe above is copy-paste from
  `site-accept.sh`; the two will drift. Extracting a shared
  `scripts/site-build.sh` that both call would fix that.
- **No unit file in the repo** — the one above lives only in this guide.
